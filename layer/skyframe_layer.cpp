#include "skyframe_layer.h"
#include "vulkan_warper.h"
#include "flow_ncnn.h"
#include "frame_pacer.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <deque>
#include <unordered_map>
#include <mutex>
#include <thread>
#include <condition_variable>
#include <atomic>
#include <cstring>
#include <chrono>
#include <cstdarg>
#include <cstdio>
#include <cmath>
#include <algorithm>
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#if __has_include("warp_blend_comp_spv.h")
#include "warp_blend_comp_spv.h"
#define HAVE_WARP_BLEND 1
#endif
#if __has_include("pyramid_down_comp_spv.h")
#include "pyramid_down_comp_spv.h"
#define HAVE_PYRAMID_DOWN 1
#endif
#if __has_include("flow_coarse_comp_spv.h")
#include "flow_coarse_comp_spv.h"
#define HAVE_FLOW_COARSE 1
#endif
#if __has_include("flow_refine_comp_spv.h")
#include "flow_refine_comp_spv.h"
#define HAVE_FLOW_REFINE 1
#endif

namespace skyframe {

static LayerConfig g_config;
static std::mutex g_configMutex;
static uint32_t g_graphicsQueueFamily = 0;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    // 1. Output to stderr
    std::cerr << "[SkyFrame] " << buf << std::endl;

    // 2. Global /tmp/skyframe.log (always accessible in any container/user)
    FILE* fTmp = fopen("/tmp/skyframe.log", "a");
    if (fTmp) {
        fprintf(fTmp, "[SkyFrame] %s\n", buf);
        fclose(fTmp);
        chmod("/tmp/skyframe.log", 0666);
    }

    // 3. User directory log
    const char* home = getenv("HOME");
    std::string logDir = (home && strlen(home) > 0) ? (std::string(home) + "/.local/share/skyframe") : "/home/deck/.local/share/skyframe";
    mkdir(logDir.c_str(), 0755);
    std::string logPath = logDir + "/skyframe.log";
    FILE* f = fopen(logPath.c_str(), "a");
    if (f) {
        fprintf(f, "[SkyFrame] %s\n", buf);
        fclose(f);
        chmod(logPath.c_str(), 0666);
    }
}

LayerConfig& GetConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_config;
}

static time_t g_lastConfigMtime = 0;
static long g_lastConfigNsec = 0;

void ReloadConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    const char* home = getenv("HOME");
    std::string configPath = (home && strlen(home) > 0) ? (std::string(home) + "/.config/skyframe/config.json") : "/home/deck/.config/skyframe/config.json";

    std::ifstream file(configPath);
    if (!file.is_open()) {
        if (configPath != "/home/deck/.config/skyframe/config.json") {
            file.open("/home/deck/.config/skyframe/config.json");
            if (file.is_open()) {
                configPath = "/home/deck/.config/skyframe/config.json";
            }
        }
    }

    bool fileOpened = file.is_open();
    if (fileOpened) {
        struct stat st;
        if (stat(configPath.c_str(), &st) == 0) {
            g_lastConfigMtime = st.st_mtime;
#if defined(__linux__)
            g_lastConfigNsec = st.st_mtim.tv_nsec;
#endif
        }

        std::string line;
        while (std::getline(file, line)) {
            if (line.find("\"enabled\"") != std::string::npos) {
                g_config.enabled = (line.find("true") != std::string::npos);
            } else if (line.find("\"mode\"") != std::string::npos) {
                if (line.find("0") != std::string::npos) g_config.mode = 0;
                else if (line.find("2") != std::string::npos) g_config.mode = 2;
                else g_config.mode = 1;
            } else if (line.find("\"hud_protection\"") != std::string::npos) {
                g_config.hud_protection = (line.find("true") != std::string::npos);
            } else if (line.find("\"show_hud\"") != std::string::npos) {
                g_config.show_hud = (line.find("true") != std::string::npos);
            } else if (line.find("\"target_hz\"") != std::string::npos) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    int hz = std::atoi(line.c_str() + colon + 1);
                    if (hz >= 30 && hz <= 240) g_config.target_hz = hz;
                }
            } else if (line.find("\"flow_scale\"") != std::string::npos) {
                size_t colon = line.find(':');
                if (colon != std::string::npos) {
                    float s = std::strtof(line.c_str() + colon + 1, nullptr);
                    if (s >= 0.25f && s <= 1.0f) g_config.flow_scale = s;
                }
            }
        }
    }

    // Explicit environment variable overrides
    // Note: If config.json was NOT found, fall back to ENABLE_SKYFRAME.
    if (!fileOpened) {
        const char* envEnable = getenv("ENABLE_SKYFRAME");
        if (envEnable && (strcmp(envEnable, "1") == 0 || strcmp(envEnable, "true") == 0)) {
            g_config.enabled = true;
        }
    }

    const char* envDisable = getenv("DISABLE_SKYFRAME");
    if (envDisable && (strcmp(envDisable, "1") == 0 || strcmp(envDisable, "true") == 0)) {
        g_config.enabled = false;
    }
    const char* envHud = getenv("SKYFRAME_HUD");
    if (envHud && (strcmp(envHud, "1") == 0 || strcmp(envHud, "true") == 0)) {
        g_config.show_hud = true;
    }
    const char* envFlowScale = getenv("SKYFRAME_FLOW_SCALE");
    if (envFlowScale && strlen(envFlowScale) > 0) {
        float s = std::strtof(envFlowScale, nullptr);
        if (s >= 0.25f && s <= 1.0f) g_config.flow_scale = s;
    }
}

static void CheckHotReload() {
    static auto lastCheck = std::chrono::steady_clock::now();
    auto now = std::chrono::steady_clock::now();
    if (now - lastCheck < std::chrono::milliseconds(100)) {
        return;
    }
    lastCheck = now;

    const char* home = getenv("HOME");
    std::string configPath = (home && strlen(home) > 0) ? (std::string(home) + "/.config/skyframe/config.json") : "/home/deck/.config/skyframe/config.json";

    struct stat st;
    if (stat(configPath.c_str(), &st) != 0) {
        if (configPath != "/home/deck/.config/skyframe/config.json") {
            configPath = "/home/deck/.config/skyframe/config.json";
            if (stat(configPath.c_str(), &st) != 0) {
                return;
            }
        } else {
            return;
        }
    }

    bool changed = (st.st_mtime != g_lastConfigMtime);
#if defined(__linux__)
    if (st.st_mtim.tv_nsec != g_lastConfigNsec) {
        changed = true;
    }
#endif

    if (changed) {
        ReloadConfig();
        auto& cfg = GetConfig();
        Log("Hot-reload applied: enabled=%d, mode=%d, show_hud=%d, hud_protection=%d, target_hz=%d, flow_scale=%.2f",
            cfg.enabled, cfg.mode, cfg.show_hud, cfg.hud_protection, cfg.target_hz, cfg.flow_scale);
    }
}

// Next function pointers in Vulkan chain
PFN_vkGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr   g_nextGetDeviceProcAddr = nullptr;

PFN_vkCreateInstance      g_nextCreateInstance = nullptr;
PFN_vkDestroyInstance     g_nextDestroyInstance = nullptr;
PFN_vkCreateDevice        g_nextCreateDevice = nullptr;
PFN_vkDestroyDevice       g_nextDestroyDevice = nullptr;

PFN_vkCreateSwapchainKHR     g_pfnCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR    g_pfnDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR  g_pfnGetSwapchainImagesKHR = nullptr;
PFN_vkQueuePresentKHR        g_pfnQueuePresentKHR = nullptr;
PFN_vkAcquireNextImageKHR    g_pfnAcquireNextImageKHR = nullptr;

PFN_vkCreateCommandPool      g_pfnCreateCommandPool = nullptr;
PFN_vkDestroyCommandPool     g_pfnDestroyCommandPool = nullptr;
PFN_vkAllocateCommandBuffers g_pfnAllocateCommandBuffers = nullptr;
PFN_vkBeginCommandBuffer     g_pfnBeginCommandBuffer = nullptr;
PFN_vkEndCommandBuffer       g_pfnEndCommandBuffer = nullptr;
PFN_vkCmdPipelineBarrier     g_pfnCmdPipelineBarrier = nullptr;
PFN_vkCmdCopyImage           g_pfnCmdCopyImage = nullptr;
PFN_vkCmdBlitImage           g_pfnCmdBlitImage = nullptr;
PFN_vkCreateSemaphore        g_pfnCreateSemaphore = nullptr;
PFN_vkDestroySemaphore       g_pfnDestroySemaphore = nullptr;
PFN_vkQueueSubmit            g_pfnQueueSubmit = nullptr;

PFN_vkCreateShaderModule        g_pfnCreateShaderModule = nullptr;
PFN_vkDestroyShaderModule       g_pfnDestroyShaderModule = nullptr;
PFN_vkCreateDescriptorSetLayout g_pfnCreateDescriptorSetLayout = nullptr;
PFN_vkDestroyDescriptorSetLayout g_pfnDestroyDescriptorSetLayout = nullptr;
PFN_vkCreatePipelineLayout      g_pfnCreatePipelineLayout = nullptr;
PFN_vkDestroyPipelineLayout     g_pfnDestroyPipelineLayout = nullptr;
PFN_vkCreateComputePipelines    g_pfnCreateComputePipelines = nullptr;
PFN_vkDestroyPipeline           g_pfnDestroyPipeline = nullptr;
PFN_vkCreateDescriptorPool      g_pfnCreateDescriptorPool = nullptr;
PFN_vkDestroyDescriptorPool     g_pfnDestroyDescriptorPool = nullptr;
PFN_vkAllocateDescriptorSets    g_pfnAllocateDescriptorSets = nullptr;
PFN_vkUpdateDescriptorSets      g_pfnUpdateDescriptorSets = nullptr;
PFN_vkCreateImageView           g_pfnCreateImageView = nullptr;
PFN_vkDestroyImageView          g_pfnDestroyImageView = nullptr;
PFN_vkCmdBindPipeline           g_pfnCmdBindPipeline = nullptr;
PFN_vkCmdBindDescriptorSets     g_pfnCmdBindDescriptorSets = nullptr;
PFN_vkCmdPushConstants          g_pfnCmdPushConstants = nullptr;
PFN_vkCmdDispatch               g_pfnCmdDispatch = nullptr;

PFN_vkGetPhysicalDeviceMemoryProperties g_pfnGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkCreateImage                       g_pfnCreateImage = nullptr;
PFN_vkDestroyImage                      g_pfnDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements        g_pfnGetImageMemoryRequirements = nullptr;
PFN_vkAllocateMemory                    g_pfnAllocateMemory = nullptr;
PFN_vkFreeMemory                        g_pfnFreeMemory = nullptr;
PFN_vkBindImageMemory                   g_pfnBindImageMemory = nullptr;
static VkPhysicalDevice                 g_physicalDevice = VK_NULL_HANDLE;

constexpr size_t RING_SIZE = 4;

struct FrameSlot {
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    VkSemaphore acqSemaphore = VK_NULL_HANDLE;
    VkSemaphore interDoneSemaphore = VK_NULL_HANDLE;
    VkSemaphore finalDoneSemaphore = VK_NULL_HANDLE;

    // Descriptor Sets for the 4 passes
    VkDescriptorSet downDescSet = VK_NULL_HANDLE;   // Pass 0
    VkDescriptorSet coarseDescSet = VK_NULL_HANDLE; // Pass 1
    VkDescriptorSet refineDescSet = VK_NULL_HANDLE; // Pass 2
    VkDescriptorSet blendDescSet = VK_NULL_HANDLE;  // Pass 3

    // Intermediate Pyramidal Resources
    // Luma Pyramid (320x200, R32_SFLOAT)
    VkImage lumaPyr0 = VK_NULL_HANDLE;
    VkDeviceMemory lumaPyr0Memory = VK_NULL_HANDLE;
    VkImageView lumaPyr0View = VK_NULL_HANDLE;

    VkImage lumaPyr1 = VK_NULL_HANDLE;
    VkDeviceMemory lumaPyr1Memory = VK_NULL_HANDLE;
    VkImageView lumaPyr1View = VK_NULL_HANDLE;

    // Coarse Flow (80x50, R16G16B16A16_SFLOAT)
    VkImage coarseFlow = VK_NULL_HANDLE;
    VkDeviceMemory coarseFlowMemory = VK_NULL_HANDLE;
    VkImageView coarseFlowView = VK_NULL_HANDLE;

    // Dense Flow (320x200, R16G16B16A16_SFLOAT)
    VkImage denseFlow = VK_NULL_HANDLE;
    VkDeviceMemory denseFlowMemory = VK_NULL_HANDLE;
    VkImageView denseFlowView = VK_NULL_HANDLE;
};

struct PendingPresent {
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    VkSemaphore waitSemaphore = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point targetTime;
};

struct DownPushConstants {
    int in_width;
    int in_height;
    int out_width;
    int out_height;
};

struct CoarsePushConstants {
    int luma_width;
    int luma_height;
    int grid_width;
    int grid_height;
};

struct RefinePushConstants {
    int full_width;
    int full_height;
    int dense_width;
    int dense_height;
    int coarse_width;
    int coarse_height;
};

struct BlendPushConstants {
    float alpha;
    int   width;
    int   height;
    int   show_hud;
    int   hud_protection;
    int   mode;
    int   flow_width;
    int   flow_height;
};

struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physDevice = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    FrameSlot slots[RING_SIZE];
    size_t ringIndex = 0;

    std::unique_ptr<VulkanWarper> warper;
    std::unique_ptr<FlowEstimator> flowEstimator;
    FramePacer pacer;

    // Asynchronous Frame Pacer Worker
    std::thread presentWorkerThread;
    std::atomic<bool> workerRunning{false};
    std::mutex presentMutex;
    std::condition_variable presentCv;
    std::mutex queueMutex;
    std::deque<PendingPresent> pendingPresents;

    uint32_t prevGameIdx = UINT32_MAX;

    // Pass 0: Pyramid Downsampler
    VkShaderModule downModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout downDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout downPipelineLayout = VK_NULL_HANDLE;
    VkPipeline downPipeline = VK_NULL_HANDLE;

    // Pass 1: Coarse Motion Search
    VkShaderModule coarseModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout coarseDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout coarsePipelineLayout = VK_NULL_HANDLE;
    VkPipeline coarsePipeline = VK_NULL_HANDLE;

    // Pass 2: Dense Flow Refinement
    VkShaderModule refineModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout refineDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout refinePipelineLayout = VK_NULL_HANDLE;
    VkPipeline refinePipeline = VK_NULL_HANDLE;

    // Pass 3: Warp & Bilinear Occlusion Blend
    VkShaderModule blendModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout blendDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout blendPipelineLayout = VK_NULL_HANDLE;
    VkPipeline blendPipeline = VK_NULL_HANDLE;

    VkDescriptorPool pipelineDescPool = VK_NULL_HANDLE;
    std::vector<VkImageView> imageViews;

    bool isInitialized = false;
};

static uint32_t FindMemoryType(VkPhysicalDevice physDev, uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    VkPhysicalDeviceMemoryProperties memProperties{};
    if (g_pfnGetPhysicalDeviceMemoryProperties && physDev != VK_NULL_HANDLE) {
        g_pfnGetPhysicalDeviceMemoryProperties(physDev, &memProperties);
    } else if (physDev != VK_NULL_HANDLE) {
        vkGetPhysicalDeviceMemoryProperties(physDev, &memProperties);
    } else {
        return 0;
    }
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if ((typeFilter & (1 << i)) && (memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
            return i;
        }
    }
    for (uint32_t i = 0; i < memProperties.memoryTypeCount; i++) {
        if (typeFilter & (1 << i)) {
            return i;
        }
    }
    return 0;
}

static void PresentWorkerLoop(std::shared_ptr<SwapchainContext> ctx) {
    while (ctx->workerRunning.load()) {
        PendingPresent job;
        {
            std::unique_lock<std::mutex> lock(ctx->presentMutex);
            ctx->presentCv.wait(lock, [&]() {
                return !ctx->workerRunning.load() || !ctx->pendingPresents.empty();
            });
            if (!ctx->workerRunning.load()) break;

            job = ctx->pendingPresents.front();
            ctx->pendingPresents.pop_front();
        }

        // High-precision pacing with 0.0% CPU: sleep using OS scheduler until near target
        auto now = std::chrono::steady_clock::now();
        if (job.targetTime > now) {
            auto diff = job.targetTime - now;
            if (diff > std::chrono::microseconds(300)) {
                std::this_thread::sleep_for(diff - std::chrono::microseconds(150));
            }
            while (std::chrono::steady_clock::now() < job.targetTime) {
#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#if defined(_MSC_VER)
                _mm_pause();
#else
                __builtin_ia32_pause();
#endif
#endif
            }
        }

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = (job.waitSemaphore != VK_NULL_HANDLE) ? 1 : 0;
        pi.pWaitSemaphores = (job.waitSemaphore != VK_NULL_HANDLE) ? &job.waitSemaphore : nullptr;
        pi.swapchainCount = 1;
        pi.pSwapchains = &job.swapchain;
        pi.pImageIndices = &job.imageIndex;

        {
            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            if (g_pfnQueuePresentKHR && job.queue != VK_NULL_HANDLE) {
                g_pfnQueuePresentKHR(job.queue, &pi);
            }
        }
    }
}

static void DrainPendingPresents(SwapchainContext* ctx) {
    if (!ctx) return;
    std::unique_lock<std::mutex> lock(ctx->presentMutex);
    while (!ctx->pendingPresents.empty()) {
        auto oldJob = ctx->pendingPresents.front();
        ctx->pendingPresents.pop_front();
        lock.unlock();

        VkPresentInfoKHR pi{};
        pi.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        pi.waitSemaphoreCount = (oldJob.waitSemaphore != VK_NULL_HANDLE) ? 1 : 0;
        pi.pWaitSemaphores = (oldJob.waitSemaphore != VK_NULL_HANDLE) ? &oldJob.waitSemaphore : nullptr;
        pi.swapchainCount = 1;
        pi.pSwapchains = &oldJob.swapchain;
        pi.pImageIndices = &oldJob.imageIndex;
        {
            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            if (g_pfnQueuePresentKHR && oldJob.queue != VK_NULL_HANDLE) {
                g_pfnQueuePresentKHR(oldJob.queue, &pi);
            }
        }
        lock.lock();
    }
    ctx->presentCv.notify_all();
}

static bool CreateStorageImage2D(
    VkDevice device,
    VkPhysicalDevice physDevice,
    uint32_t width,
    uint32_t height,
    VkFormat format,
    VkImage& outImage,
    VkDeviceMemory& outMemory,
    VkImageView& outView
) {
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { width, height, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_STORAGE_BIT;
    ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

    if (g_pfnCreateImage(device, &ici, nullptr, &outImage) != VK_SUCCESS) {
        return false;
    }

    VkMemoryRequirements memReqs{};
    g_pfnGetImageMemoryRequirements(device, outImage, &memReqs);

    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReqs.size;
    mai.memoryTypeIndex = FindMemoryType(physDevice, memReqs.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);

    if (g_pfnAllocateMemory(device, &mai, nullptr, &outMemory) != VK_SUCCESS) {
        return false;
    }

    if (g_pfnBindImageMemory(device, outImage, outMemory, 0) != VK_SUCCESS) {
        return false;
    }

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = outImage;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
    ivci.subresourceRange.baseMipLevel = 0;
    ivci.subresourceRange.levelCount = 1;
    ivci.subresourceRange.baseArrayLayer = 0;
    ivci.subresourceRange.layerCount = 1;

    if (g_pfnCreateImageView(device, &ivci, nullptr, &outView) != VK_SUCCESS) {
        return false;
    }
    return true;
}

static void DestroyStorageImage2D(
    VkDevice device,
    VkImage& image,
    VkDeviceMemory& memory,
    VkImageView& view
) {
    if (view != VK_NULL_HANDLE && g_pfnDestroyImageView) {
        g_pfnDestroyImageView(device, view, nullptr);
        view = VK_NULL_HANDLE;
    }
    if (image != VK_NULL_HANDLE && g_pfnDestroyImage) {
        g_pfnDestroyImage(device, image, nullptr);
        image = VK_NULL_HANDLE;
    }
    if (memory != VK_NULL_HANDLE && g_pfnFreeMemory) {
        g_pfnFreeMemory(device, memory, nullptr);
        memory = VK_NULL_HANDLE;
    }
}

static bool InitPipelines(SwapchainContext* ctx) {
#if HAVE_WARP_BLEND && HAVE_PYRAMID_DOWN && HAVE_FLOW_COARSE && HAVE_FLOW_REFINE
    if (!g_pfnCreateShaderModule || !g_pfnCreateDescriptorSetLayout ||
        !g_pfnCreatePipelineLayout || !g_pfnCreateComputePipelines ||
        !g_pfnCreateDescriptorPool || !g_pfnAllocateDescriptorSets ||
        !g_pfnCreateImageView || !g_pfnCreateImage ||
        !g_pfnGetImageMemoryRequirements || !g_pfnAllocateMemory ||
        !g_pfnBindImageMemory) {
        Log("Pipelines: required Vulkan function pointers missing");
        return false;
    }

    if (ctx->images.empty()) {
        return false;
    }

    // 1. Create Image Views for all swapchain images if needed
    if (ctx->imageViews.size() != ctx->images.size()) {
        for (auto iv : ctx->imageViews) {
            if (iv != VK_NULL_HANDLE && g_pfnDestroyImageView) {
                g_pfnDestroyImageView(ctx->device, iv, nullptr);
            }
        }
        ctx->imageViews.clear();
        ctx->imageViews.resize(ctx->images.size(), VK_NULL_HANDLE);

        for (size_t i = 0; i < ctx->images.size(); ++i) {
            VkImageViewCreateInfo ivci{};
            ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
            ivci.image = ctx->images[i];
            ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
            ivci.format = ctx->format;
            ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            ivci.subresourceRange.baseMipLevel = 0;
            ivci.subresourceRange.levelCount = 1;
            ivci.subresourceRange.baseArrayLayer = 0;
            ivci.subresourceRange.layerCount = 1;

            if (g_pfnCreateImageView(ctx->device, &ivci, nullptr, &ctx->imageViews[i]) != VK_SUCCESS) {
                Log("Pipelines: failed to create image view for image %zu", i);
                return false;
            }
        }
    }

    if (ctx->downPipeline != VK_NULL_HANDLE &&
        ctx->coarsePipeline != VK_NULL_HANDLE &&
        ctx->refinePipeline != VK_NULL_HANDLE &&
        ctx->blendPipeline != VK_NULL_HANDLE) {
        return true;
    }

    auto createComputePipeline = [&](const uint32_t* spvCode, size_t spvSize,
                                     uint32_t bindingCount, uint32_t pushConstantSize,
                                     VkShaderModule& outModule, VkDescriptorSetLayout& outDescLayout,
                                     VkPipelineLayout& outPipelineLayout, VkPipeline& outPipeline) -> bool {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = spvSize;
        smci.pCode = spvCode;
        if (g_pfnCreateShaderModule(ctx->device, &smci, nullptr, &outModule) != VK_SUCCESS) {
            return false;
        }

        std::vector<VkDescriptorSetLayoutBinding> bindings(bindingCount);
        for (uint32_t b = 0; b < bindingCount; ++b) {
            bindings[b].binding = b;
            bindings[b].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[b].descriptorCount = 1;
            bindings[b].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = bindingCount;
        dslci.pBindings = bindings.data();
        if (g_pfnCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &outDescLayout) != VK_SUCCESS) {
            return false;
        }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = pushConstantSize;

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &outDescLayout;
        plci.pushConstantRangeCount = (pushConstantSize > 0) ? 1 : 0;
        plci.pPushConstantRanges = (pushConstantSize > 0) ? &pcr : nullptr;
        if (g_pfnCreatePipelineLayout(ctx->device, &plci, nullptr, &outPipelineLayout) != VK_SUCCESS) {
            return false;
        }

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = outModule;
        cpci.stage.pName = "main";
        cpci.layout = outPipelineLayout;
        if (g_pfnCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &outPipeline) != VK_SUCCESS) {
            return false;
        }
        return true;
    };

    // 2. Pass 0: Pyramid Downsampler (4 bindings: Frame0, Frame1, Luma0, Luma1)
    if (!createComputePipeline(pyramid_down_comp_spv, pyramid_down_comp_spv_size,
                               4, sizeof(DownPushConstants),
                               ctx->downModule, ctx->downDescLayout, ctx->downPipelineLayout, ctx->downPipeline)) {
        Log("Pipelines: failed to create pyramid_down compute pipeline");
        return false;
    }

    // 3. Pass 1: Coarse Motion Search (3 bindings: Luma0, Luma1, CoarseFlow)
    if (!createComputePipeline(flow_coarse_comp_spv, flow_coarse_comp_spv_size,
                               3, sizeof(CoarsePushConstants),
                               ctx->coarseModule, ctx->coarseDescLayout, ctx->coarsePipelineLayout, ctx->coarsePipeline)) {
        Log("Pipelines: failed to create flow_coarse compute pipeline");
        return false;
    }

    // 4. Pass 2: Dense Flow Refinement (4 bindings: Frame0, Frame1, CoarseFlow, DenseFlow)
    if (!createComputePipeline(flow_refine_comp_spv, flow_refine_comp_spv_size,
                               4, sizeof(RefinePushConstants),
                               ctx->refineModule, ctx->refineDescLayout, ctx->refinePipelineLayout, ctx->refinePipeline)) {
        Log("Pipelines: failed to create flow_refine compute pipeline");
        return false;
    }

    // 5. Pass 3: Warp & Occlusion-Aware Blend (4 bindings: Frame0, Frame1, OutImage, DenseFlow)
    if (!createComputePipeline(warp_blend_comp_spv, warp_blend_comp_spv_size,
                               4, sizeof(BlendPushConstants),
                               ctx->blendModule, ctx->blendDescLayout, ctx->blendPipelineLayout, ctx->blendPipeline)) {
        Log("Pipelines: failed to create warp_blend compute pipeline");
        return false;
    }

    // 6. Descriptor Pool & Sets for Ring Buffer
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = static_cast<uint32_t>(RING_SIZE * 16 + 32);

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = static_cast<uint32_t>(RING_SIZE * 4 + 16);
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;

    if (g_pfnCreateDescriptorPool(ctx->device, &dpci, nullptr, &ctx->pipelineDescPool) != VK_SUCCESS) {
        Log("Pipelines: failed to create descriptor pool");
        return false;
    }

    for (size_t i = 0; i < RING_SIZE; ++i) {
        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = ctx->pipelineDescPool;
        dsai.descriptorSetCount = 1;

        dsai.pSetLayouts = &ctx->downDescLayout;
        if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, &ctx->slots[i].downDescSet) != VK_SUCCESS) {
            Log("Pipelines: failed to allocate downDescSet for slot %zu", i);
            return false;
        }

        dsai.pSetLayouts = &ctx->coarseDescLayout;
        if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, &ctx->slots[i].coarseDescSet) != VK_SUCCESS) {
            Log("Pipelines: failed to allocate coarseDescSet for slot %zu", i);
            return false;
        }

        dsai.pSetLayouts = &ctx->refineDescLayout;
        if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, &ctx->slots[i].refineDescSet) != VK_SUCCESS) {
            Log("Pipelines: failed to allocate refineDescSet for slot %zu", i);
            return false;
        }

        dsai.pSetLayouts = &ctx->blendDescLayout;
        if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, &ctx->slots[i].blendDescSet) != VK_SUCCESS) {
            Log("Pipelines: failed to allocate blendDescSet for slot %zu", i);
            return false;
        }
    }

    // 7. Allocate intermediate pyramidal storage images for each ring slot
    uint32_t pyrW = (ctx->extent.width + 3) / 4;
    uint32_t pyrH = (ctx->extent.height + 3) / 4;
    uint32_t coarseW = (pyrW + 3) / 4;
    uint32_t coarseH = (pyrH + 3) / 4;

    for (size_t i = 0; i < RING_SIZE; ++i) {
        if (!CreateStorageImage2D(ctx->device, ctx->physDevice, pyrW, pyrH, VK_FORMAT_R32_SFLOAT,
                                 ctx->slots[i].lumaPyr0, ctx->slots[i].lumaPyr0Memory, ctx->slots[i].lumaPyr0View)) {
            Log("Pipelines: failed to create lumaPyr0 for slot %zu", i);
            return false;
        }
        if (!CreateStorageImage2D(ctx->device, ctx->physDevice, pyrW, pyrH, VK_FORMAT_R32_SFLOAT,
                                 ctx->slots[i].lumaPyr1, ctx->slots[i].lumaPyr1Memory, ctx->slots[i].lumaPyr1View)) {
            Log("Pipelines: failed to create lumaPyr1 for slot %zu", i);
            return false;
        }
        if (!CreateStorageImage2D(ctx->device, ctx->physDevice, coarseW, coarseH, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 ctx->slots[i].coarseFlow, ctx->slots[i].coarseFlowMemory, ctx->slots[i].coarseFlowView)) {
            Log("Pipelines: failed to create coarseFlow for slot %zu", i);
            return false;
        }
        if (!CreateStorageImage2D(ctx->device, ctx->physDevice, ctx->extent.width, ctx->extent.height, VK_FORMAT_R16G16B16A16_SFLOAT,
                                 ctx->slots[i].denseFlow, ctx->slots[i].denseFlowMemory, ctx->slots[i].denseFlowView)) {
            Log("Pipelines: failed to create denseFlow for slot %zu", i);
            return false;
        }
    }

    Log("Pipelines: Option 2 Hierarchical Pyramidal Coarse-to-Fine Pipeline initialized! (Pyr: %ux%u, Coarse: %ux%u, DenseMax: %ux%u)",
        pyrW, pyrH, coarseW, coarseH, ctx->extent.width, ctx->extent.height);
    return true;
#else
    Log("Pipelines: Required shader SPV headers not available");
    return false;
#endif
}

static std::mutex g_contextMutex;
static std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainContext>> g_swapchains;

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance
) {
    Log("Hook_vkCreateInstance called");
    if (pCreateInfo && pCreateInfo->pApplicationInfo && pCreateInfo->pApplicationInfo->pApplicationName) {
        Log("Target Application: %s (engine: %s)",
            pCreateInfo->pApplicationInfo->pApplicationName,
            pCreateInfo->pApplicationInfo->pEngineName ? pCreateInfo->pApplicationInfo->pEngineName : "unknown");
    }

    VkLayerInstanceCreateInfo* chain_info = (VkLayerInstanceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && (chain_info->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
                          chain_info->function != VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerInstanceCreateInfo*)chain_info->pNext;
    }

    if (!chain_info) {
        Log("ERROR: No VK_LAYER_LINK_INFO found in vkCreateInstance chain!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    g_nextGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    g_nextCreateInstance = (PFN_vkCreateInstance)g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!g_nextCreateInstance) {
        Log("ERROR: Failed to find next vkCreateInstance!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = g_nextCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) {
        Log("vkCreateInstance downstream returned error: %d", res);
        return res;
    }

    g_nextDestroyInstance = (PFN_vkDestroyInstance)g_nextGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    g_nextCreateDevice = (PFN_vkCreateDevice)g_nextGetInstanceProcAddr(*pInstance, "vkCreateDevice");
    g_pfnGetPhysicalDeviceMemoryProperties = (PFN_vkGetPhysicalDeviceMemoryProperties)g_nextGetInstanceProcAddr(*pInstance, "vkGetPhysicalDeviceMemoryProperties");
    if (!g_pfnGetPhysicalDeviceMemoryProperties) g_pfnGetPhysicalDeviceMemoryProperties = &vkGetPhysicalDeviceMemoryProperties;

    Log("Vulkan Instance created successfully. Layer hooked.");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyInstance(
    VkInstance instance,
    const VkAllocationCallbacks* pAllocator
) {
    Log("Hook_vkDestroyInstance called");
    if (g_nextDestroyInstance) {
        g_nextDestroyInstance(instance, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateDevice(
    VkPhysicalDevice physicalDevice,
    const VkDeviceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkDevice* pDevice
) {
    Log("Hook_vkCreateDevice called");
    VkLayerDeviceCreateInfo* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && (chain_info->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                          chain_info->function != VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
    }

    if (!chain_info) {
        Log("ERROR: No VK_LAYER_LINK_INFO found in vkCreateDevice chain!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    g_nextGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    g_nextGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateDevice nextCreateDevice = (PFN_vkCreateDevice)g_nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    if (!nextCreateDevice) nextCreateDevice = g_nextCreateDevice;

    VkResult res = nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) {
        Log("vkCreateDevice downstream returned error: %d", res);
        return res;
    }

    if (pCreateInfo && pCreateInfo->queueCreateInfoCount > 0) {
        g_graphicsQueueFamily = pCreateInfo->pQueueCreateInfos[0].queueFamilyIndex;
    }

    g_nextDestroyDevice = (PFN_vkDestroyDevice)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    g_pfnCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)g_nextGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
    g_pfnDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)g_nextGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
    g_pfnGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)g_nextGetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
    g_pfnQueuePresentKHR = (PFN_vkQueuePresentKHR)g_nextGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");
    g_pfnAcquireNextImageKHR = (PFN_vkAcquireNextImageKHR)g_nextGetDeviceProcAddr(*pDevice, "vkAcquireNextImageKHR");

    g_pfnCreateCommandPool = (PFN_vkCreateCommandPool)g_nextGetDeviceProcAddr(*pDevice, "vkCreateCommandPool");
    g_pfnDestroyCommandPool = (PFN_vkDestroyCommandPool)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyCommandPool");
    g_pfnAllocateCommandBuffers = (PFN_vkAllocateCommandBuffers)g_nextGetDeviceProcAddr(*pDevice, "vkAllocateCommandBuffers");
    g_pfnBeginCommandBuffer = (PFN_vkBeginCommandBuffer)g_nextGetDeviceProcAddr(*pDevice, "vkBeginCommandBuffer");
    g_pfnEndCommandBuffer = (PFN_vkEndCommandBuffer)g_nextGetDeviceProcAddr(*pDevice, "vkEndCommandBuffer");
    g_pfnCmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)g_nextGetDeviceProcAddr(*pDevice, "vkCmdPipelineBarrier");
    g_pfnCmdCopyImage = (PFN_vkCmdCopyImage)g_nextGetDeviceProcAddr(*pDevice, "vkCmdCopyImage");
    g_pfnCmdBlitImage = (PFN_vkCmdBlitImage)g_nextGetDeviceProcAddr(*pDevice, "vkCmdBlitImage");
    g_pfnCreateSemaphore = (PFN_vkCreateSemaphore)g_nextGetDeviceProcAddr(*pDevice, "vkCreateSemaphore");
    g_pfnDestroySemaphore = (PFN_vkDestroySemaphore)g_nextGetDeviceProcAddr(*pDevice, "vkDestroySemaphore");
    g_pfnQueueSubmit = (PFN_vkQueueSubmit)g_nextGetDeviceProcAddr(*pDevice, "vkQueueSubmit");

    g_pfnCreateShaderModule = (PFN_vkCreateShaderModule)g_nextGetDeviceProcAddr(*pDevice, "vkCreateShaderModule");
    g_pfnDestroyShaderModule = (PFN_vkDestroyShaderModule)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyShaderModule");
    g_pfnCreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)g_nextGetDeviceProcAddr(*pDevice, "vkCreateDescriptorSetLayout");
    g_pfnDestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyDescriptorSetLayout");
    g_pfnCreatePipelineLayout = (PFN_vkCreatePipelineLayout)g_nextGetDeviceProcAddr(*pDevice, "vkCreatePipelineLayout");
    g_pfnDestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyPipelineLayout");
    g_pfnCreateComputePipelines = (PFN_vkCreateComputePipelines)g_nextGetDeviceProcAddr(*pDevice, "vkCreateComputePipelines");
    g_pfnDestroyPipeline = (PFN_vkDestroyPipeline)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyPipeline");
    g_pfnCreateDescriptorPool = (PFN_vkCreateDescriptorPool)g_nextGetDeviceProcAddr(*pDevice, "vkCreateDescriptorPool");
    g_pfnDestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyDescriptorPool");
    g_pfnAllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)g_nextGetDeviceProcAddr(*pDevice, "vkAllocateDescriptorSets");
    g_pfnUpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)g_nextGetDeviceProcAddr(*pDevice, "vkUpdateDescriptorSets");
    g_pfnCreateImageView = (PFN_vkCreateImageView)g_nextGetDeviceProcAddr(*pDevice, "vkCreateImageView");
    g_pfnDestroyImageView = (PFN_vkDestroyImageView)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyImageView");
    g_pfnCmdBindPipeline = (PFN_vkCmdBindPipeline)g_nextGetDeviceProcAddr(*pDevice, "vkCmdBindPipeline");
    g_pfnCmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)g_nextGetDeviceProcAddr(*pDevice, "vkCmdBindDescriptorSets");
    g_pfnCmdPushConstants = (PFN_vkCmdPushConstants)g_nextGetDeviceProcAddr(*pDevice, "vkCmdPushConstants");
    g_pfnCmdDispatch = (PFN_vkCmdDispatch)g_nextGetDeviceProcAddr(*pDevice, "vkCmdDispatch");

    g_pfnCreateImage = (PFN_vkCreateImage)g_nextGetDeviceProcAddr(*pDevice, "vkCreateImage");
    g_pfnDestroyImage = (PFN_vkDestroyImage)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyImage");
    g_pfnGetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)g_nextGetDeviceProcAddr(*pDevice, "vkGetImageMemoryRequirements");
    g_pfnAllocateMemory = (PFN_vkAllocateMemory)g_nextGetDeviceProcAddr(*pDevice, "vkAllocateMemory");
    g_pfnFreeMemory = (PFN_vkFreeMemory)g_nextGetDeviceProcAddr(*pDevice, "vkFreeMemory");
    g_pfnBindImageMemory = (PFN_vkBindImageMemory)g_nextGetDeviceProcAddr(*pDevice, "vkBindImageMemory");
    g_physicalDevice = physicalDevice;

    // Fallbacks
    if (!g_pfnCreateCommandPool) g_pfnCreateCommandPool = &vkCreateCommandPool;
    if (!g_pfnDestroyCommandPool) g_pfnDestroyCommandPool = &vkDestroyCommandPool;
    if (!g_pfnAllocateCommandBuffers) g_pfnAllocateCommandBuffers = &vkAllocateCommandBuffers;
    if (!g_pfnBeginCommandBuffer) g_pfnBeginCommandBuffer = &vkBeginCommandBuffer;
    if (!g_pfnEndCommandBuffer) g_pfnEndCommandBuffer = &vkEndCommandBuffer;
    if (!g_pfnCmdPipelineBarrier) g_pfnCmdPipelineBarrier = &vkCmdPipelineBarrier;
    if (!g_pfnCmdCopyImage) g_pfnCmdCopyImage = &vkCmdCopyImage;
    if (!g_pfnCmdBlitImage) g_pfnCmdBlitImage = &vkCmdBlitImage;
    if (!g_pfnCreateSemaphore) g_pfnCreateSemaphore = &vkCreateSemaphore;
    if (!g_pfnDestroySemaphore) g_pfnDestroySemaphore = &vkDestroySemaphore;
    if (!g_pfnQueueSubmit) g_pfnQueueSubmit = &vkQueueSubmit;

    if (!g_pfnCreateShaderModule) g_pfnCreateShaderModule = &vkCreateShaderModule;
    if (!g_pfnDestroyShaderModule) g_pfnDestroyShaderModule = &vkDestroyShaderModule;
    if (!g_pfnCreateDescriptorSetLayout) g_pfnCreateDescriptorSetLayout = &vkCreateDescriptorSetLayout;
    if (!g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout = &vkDestroyDescriptorSetLayout;
    if (!g_pfnCreatePipelineLayout) g_pfnCreatePipelineLayout = &vkCreatePipelineLayout;
    if (!g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout = &vkDestroyPipelineLayout;
    if (!g_pfnCreateComputePipelines) g_pfnCreateComputePipelines = &vkCreateComputePipelines;
    if (!g_pfnDestroyPipeline) g_pfnDestroyPipeline = &vkDestroyPipeline;
    if (!g_pfnCreateDescriptorPool) g_pfnCreateDescriptorPool = &vkCreateDescriptorPool;
    if (!g_pfnDestroyDescriptorPool) g_pfnDestroyDescriptorPool = &vkDestroyDescriptorPool;
    if (!g_pfnAllocateDescriptorSets) g_pfnAllocateDescriptorSets = &vkAllocateDescriptorSets;
    if (!g_pfnUpdateDescriptorSets) g_pfnUpdateDescriptorSets = &vkUpdateDescriptorSets;
    if (!g_pfnCreateImageView) g_pfnCreateImageView = &vkCreateImageView;
    if (!g_pfnDestroyImageView) g_pfnDestroyImageView = &vkDestroyImageView;
    if (!g_pfnCmdBindPipeline) g_pfnCmdBindPipeline = &vkCmdBindPipeline;
    if (!g_pfnCmdBindDescriptorSets) g_pfnCmdBindDescriptorSets = &vkCmdBindDescriptorSets;
    if (!g_pfnCmdPushConstants) g_pfnCmdPushConstants = &vkCmdPushConstants;
    if (!g_pfnCmdDispatch) g_pfnCmdDispatch = &vkCmdDispatch;

    if (!g_pfnCreateImage) g_pfnCreateImage = &vkCreateImage;
    if (!g_pfnDestroyImage) g_pfnDestroyImage = &vkDestroyImage;
    if (!g_pfnGetImageMemoryRequirements) g_pfnGetImageMemoryRequirements = &vkGetImageMemoryRequirements;
    if (!g_pfnAllocateMemory) g_pfnAllocateMemory = &vkAllocateMemory;
    if (!g_pfnFreeMemory) g_pfnFreeMemory = &vkFreeMemory;
    if (!g_pfnBindImageMemory) g_pfnBindImageMemory = &vkBindImageMemory;

    Log("Vulkan Device created successfully. Dispatch table initialized.");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    Log("Hook_vkDestroyDevice called");
    if (g_nextDestroyDevice) {
        g_nextDestroyDevice(device, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain
) {
    if (!g_pfnCreateSwapchainKHR) {
        Log("ERROR: g_pfnCreateSwapchainKHR is null!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkSwapchainCreateInfoKHR modifiedCi = *pCreateInfo;
    if (modifiedCi.minImageCount < 4) {
        modifiedCi.minImageCount = 4;
    }
    modifiedCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    VkResult res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) {
        Log("Swapchain creation with STORAGE_BIT failed (%d), retrying without STORAGE_BIT", res);
        modifiedCi.imageUsage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (modifiedCi.minImageCount < 4) modifiedCi.minImageCount = 4;
        res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
    }
    if (res != VK_SUCCESS || !pSwapchain) return res;

    auto ctx = std::make_shared<SwapchainContext>();
    ctx->swapchain = *pSwapchain;
    ctx->device = device;
    ctx->physDevice = g_physicalDevice;
    ctx->format = modifiedCi.imageFormat;
    ctx->extent = modifiedCi.imageExtent;

    uint32_t imgCount = 0;
    if (g_pfnGetSwapchainImagesKHR && g_pfnGetSwapchainImagesKHR(device, *pSwapchain, &imgCount, nullptr) == VK_SUCCESS && imgCount > 0) {
        ctx->images.resize(imgCount);
        g_pfnGetSwapchainImagesKHR(device, *pSwapchain, &imgCount, ctx->images.data());
    }

    if (g_pfnCreateCommandPool) {
        VkCommandPoolCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO;
        cpci.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        cpci.queueFamilyIndex = g_graphicsQueueFamily;
        if (g_pfnCreateCommandPool(device, &cpci, nullptr, &ctx->cmdPool) == VK_SUCCESS && g_pfnAllocateCommandBuffers) {
            VkCommandBuffer rawCmds[RING_SIZE];
            VkCommandBufferAllocateInfo cbai{};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = ctx->cmdPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = RING_SIZE;
            if (g_pfnAllocateCommandBuffers(device, &cbai, rawCmds) == VK_SUCCESS) {
                VkSemaphoreCreateInfo sci{};
                sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                for (size_t i = 0; i < RING_SIZE; ++i) {
                    ctx->slots[i].cmdBuffer = rawCmds[i];
                    g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->slots[i].acqSemaphore);
                    g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->slots[i].interDoneSemaphore);
                    g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->slots[i].finalDoneSemaphore);
                }
            }
        }
    }

    InitPipelines(ctx.get());

    ctx->isInitialized = true;
    ctx->workerRunning = true;
    ctx->presentWorkerThread = std::thread(PresentWorkerLoop, ctx);

    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        g_swapchains[*pSwapchain] = ctx;
    }

    ReloadConfig();
    Log("Swapchain created: %ux%u, format=%d, imageCount=%zu, swapchain=%p",
        ctx->extent.width, ctx->extent.height, ctx->format, ctx->images.size(), (void*)*pSwapchain);
    return res;
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
    Log("Swapchain destroyed: %p", (void*)swapchain);
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            auto ctx = it->second;
            ctx->workerRunning = false;
            ctx->presentCv.notify_all();
            if (ctx->presentWorkerThread.joinable()) {
                ctx->presentWorkerThread.join();
            }

            if (ctx->blendPipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->blendPipeline, nullptr);
            if (ctx->blendPipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->blendPipelineLayout, nullptr);
            if (ctx->blendDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->blendDescLayout, nullptr);
            if (ctx->blendModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->blendModule, nullptr);

            if (ctx->refinePipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->refinePipeline, nullptr);
            if (ctx->refinePipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->refinePipelineLayout, nullptr);
            if (ctx->refineDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->refineDescLayout, nullptr);
            if (ctx->refineModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->refineModule, nullptr);

            if (ctx->coarsePipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->coarsePipeline, nullptr);
            if (ctx->coarsePipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->coarsePipelineLayout, nullptr);
            if (ctx->coarseDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->coarseDescLayout, nullptr);
            if (ctx->coarseModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->coarseModule, nullptr);

            if (ctx->downPipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->downPipeline, nullptr);
            if (ctx->downPipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->downPipelineLayout, nullptr);
            if (ctx->downDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->downDescLayout, nullptr);
            if (ctx->downModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->downModule, nullptr);

            if (ctx->pipelineDescPool && g_pfnDestroyDescriptorPool) g_pfnDestroyDescriptorPool(device, ctx->pipelineDescPool, nullptr);

            for (auto iv : ctx->imageViews) {
                if (iv && g_pfnDestroyImageView) g_pfnDestroyImageView(device, iv, nullptr);
            }

            for (size_t i = 0; i < RING_SIZE; ++i) {
                DestroyStorageImage2D(device, ctx->slots[i].lumaPyr0, ctx->slots[i].lumaPyr0Memory, ctx->slots[i].lumaPyr0View);
                DestroyStorageImage2D(device, ctx->slots[i].lumaPyr1, ctx->slots[i].lumaPyr1Memory, ctx->slots[i].lumaPyr1View);
                DestroyStorageImage2D(device, ctx->slots[i].coarseFlow, ctx->slots[i].coarseFlowMemory, ctx->slots[i].coarseFlowView);
                DestroyStorageImage2D(device, ctx->slots[i].denseFlow, ctx->slots[i].denseFlowMemory, ctx->slots[i].denseFlowView);

                if (ctx->slots[i].acqSemaphore && g_pfnDestroySemaphore) g_pfnDestroySemaphore(device, ctx->slots[i].acqSemaphore, nullptr);
                if (ctx->slots[i].interDoneSemaphore && g_pfnDestroySemaphore) g_pfnDestroySemaphore(device, ctx->slots[i].interDoneSemaphore, nullptr);
                if (ctx->slots[i].finalDoneSemaphore && g_pfnDestroySemaphore) g_pfnDestroySemaphore(device, ctx->slots[i].finalDoneSemaphore, nullptr);
            }
            if (ctx->cmdPool && g_pfnDestroyCommandPool) {
                g_pfnDestroyCommandPool(device, ctx->cmdPool, nullptr);
            }
            g_swapchains.erase(it);
        }
    }
    if (g_pfnDestroySwapchainKHR) {
        g_pfnDestroySwapchainKHR(device, swapchain, pAllocator);
    }
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages
) {
    if (!g_pfnGetSwapchainImagesKHR) return VK_ERROR_INITIALIZATION_FAILED;
    VkResult res = g_pfnGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    if (res != VK_SUCCESS || !pSwapchainImages) return res;

    std::lock_guard<std::mutex> lock(g_contextMutex);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
        it->second->images.assign(pSwapchainImages, pSwapchainImages + *pSwapchainImageCount);
        Log("Swapchain images received: count=%u", *pSwapchainImageCount);
        InitPipelines(it->second.get());
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo
) {
    if (!g_pfnQueuePresentKHR) return VK_ERROR_INITIALIZATION_FAILED;

    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    CheckHotReload();

    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    std::shared_ptr<SwapchainContext> ctx;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            ctx = it->second;
        }
    }

    auto& cfg = GetConfig();
    if (!cfg.enabled || !ctx || !ctx->isInitialized || ctx->images.empty()) {
        if (ctx) {
            DrainPendingPresents(ctx.get());
            ctx->pacer.Reset();
            ctx->prevGameIdx = UINT32_MAX;

            static uint64_t s_disabledCount = 0;
            if (s_disabledCount++ % 60 == 1) {
                FILE* fStats = fopen("/tmp/skyframe_stats.json", "w");
                if (fStats) {
                    fprintf(fStats, "{\"base_fps\": 0.0, \"output_fps\": 0.0, \"enabled\": false}\n");
                    fclose(fStats);
                }
            }

            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            return g_pfnQueuePresentKHR(queue, pPresentInfo);
        }
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    // 1. Update frame pacer and timing (Auto VSync Cadence locked to display refresh rate)
    ctx->pacer.SetTargetHz(cfg.target_hz);
    ctx->pacer.PaceBasePresent();
    auto now = std::chrono::steady_clock::now();
    static uint64_t s_presentCount = 0;
    s_presentCount++;

    uint32_t currentGameIdx = pPresentInfo->pImageIndices[0];

    // Pick frame slot from ring buffer
    auto& slot = ctx->slots[ctx->ringIndex % RING_SIZE];
    ctx->ringIndex++;

    uint64_t halfIntervalNs = ctx->pacer.GetTargetPacingDelayNs();

    // Flush any pending presentation from prior frame if game produced a fast burst
    DrainPendingPresents(ctx.get());

    // 0. Base frame initialization: if this is the very first frame, establish baseline directly
    if (ctx->prevGameIdx == UINT32_MAX) {
        ctx->prevGameIdx = currentGameIdx;
        std::lock_guard<std::mutex> qlock(ctx->queueMutex);
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    // 2. FastWarp 2x Frame Generation: Acquire next image for intermediate frame
    uint32_t intermediateIdx = 0;
    VkResult acqRes = VK_NOT_READY;
    if (g_pfnAcquireNextImageKHR && slot.acqSemaphore) {
        acqRes = g_pfnAcquireNextImageKHR(
            ctx->device, ctx->swapchain, 50000000ULL, slot.acqSemaphore, VK_NULL_HANDLE, &intermediateIdx
        );
    }

    if (acqRes == VK_SUCCESS && intermediateIdx != currentGameIdx &&
        intermediateIdx < ctx->images.size() && currentGameIdx < ctx->images.size() &&
        slot.cmdBuffer != VK_NULL_HANDLE && g_pfnBeginCommandBuffer &&
        g_pfnCmdPipelineBarrier && g_pfnEndCommandBuffer && g_pfnQueueSubmit) {

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_pfnBeginCommandBuffer(slot.cmdBuffer, &bi);

        bool usedHierarchical = false;
        if (ctx->downPipeline != VK_NULL_HANDLE &&
            ctx->coarsePipeline != VK_NULL_HANDLE &&
            ctx->refinePipeline != VK_NULL_HANDLE &&
            ctx->blendPipeline != VK_NULL_HANDLE &&
            ctx->prevGameIdx != UINT32_MAX && ctx->prevGameIdx < ctx->images.size() &&
            ctx->prevGameIdx != intermediateIdx && ctx->prevGameIdx < ctx->imageViews.size() &&
            currentGameIdx < ctx->imageViews.size() && intermediateIdx < ctx->imageViews.size() &&
            ctx->imageViews[ctx->prevGameIdx] != VK_NULL_HANDLE &&
            ctx->imageViews[currentGameIdx] != VK_NULL_HANDLE &&
            ctx->imageViews[intermediateIdx] != VK_NULL_HANDLE &&
            slot.downDescSet != VK_NULL_HANDLE && slot.coarseDescSet != VK_NULL_HANDLE &&
            slot.refineDescSet != VK_NULL_HANDLE && slot.blendDescSet != VK_NULL_HANDLE &&
            slot.lumaPyr0View != VK_NULL_HANDLE && slot.lumaPyr1View != VK_NULL_HANDLE &&
            slot.coarseFlowView != VK_NULL_HANDLE && slot.denseFlowView != VK_NULL_HANDLE &&
            g_pfnUpdateDescriptorSets && g_pfnCmdBindPipeline &&
            g_pfnCmdBindDescriptorSets && g_pfnCmdPushConstants && g_pfnCmdDispatch) {

            uint32_t fullW = ctx->extent.width;
            uint32_t fullH = ctx->extent.height;
            uint32_t pyrW = (fullW + 3) / 4;
            uint32_t pyrH = (fullH + 3) / 4;
            uint32_t coarseW = (pyrW + 3) / 4;
            uint32_t coarseH = (pyrH + 3) / 4;

            float flowScale = std::clamp(cfg.flow_scale, 0.50f, 1.00f);
            uint32_t flowW = static_cast<uint32_t>(std::round(fullW * flowScale));
            uint32_t flowH = static_cast<uint32_t>(std::round(fullH * flowScale));
            flowW = std::clamp(flowW, 16u, fullW);
            flowH = std::clamp(flowH, 16u, fullH);

            // --- 1. Update Descriptor Sets for All 4 Passes ---
            // Pass 0 (Downsample): Frame0, Frame1, Luma0, Luma1
            VkDescriptorImageInfo downImageInfos[4]{};
            downImageInfos[0].imageView = ctx->imageViews[ctx->prevGameIdx];
            downImageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            downImageInfos[1].imageView = ctx->imageViews[currentGameIdx];
            downImageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            downImageInfos[2].imageView = slot.lumaPyr0View;
            downImageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            downImageInfos[3].imageView = slot.lumaPyr1View;
            downImageInfos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet writes[15]{};
            for (int i = 0; i < 4; ++i) {
                writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[i].dstSet = slot.downDescSet;
                writes[i].dstBinding = i;
                writes[i].descriptorCount = 1;
                writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[i].pImageInfo = &downImageInfos[i];
            }

            // Pass 1 (Coarse Search): Luma0, Luma1, CoarseFlow
            VkDescriptorImageInfo coarseImageInfos[3]{};
            coarseImageInfos[0].imageView = slot.lumaPyr0View;
            coarseImageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            coarseImageInfos[1].imageView = slot.lumaPyr1View;
            coarseImageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            coarseImageInfos[2].imageView = slot.coarseFlowView;
            coarseImageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            for (int i = 0; i < 3; ++i) {
                writes[4 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[4 + i].dstSet = slot.coarseDescSet;
                writes[4 + i].dstBinding = i;
                writes[4 + i].descriptorCount = 1;
                writes[4 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[4 + i].pImageInfo = &coarseImageInfos[i];
            }

            // Pass 2 (Refine): Frame0, Frame1, CoarseFlow, DenseFlow
            VkDescriptorImageInfo refineImageInfos[4]{};
            refineImageInfos[0].imageView = ctx->imageViews[ctx->prevGameIdx];
            refineImageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            refineImageInfos[1].imageView = ctx->imageViews[currentGameIdx];
            refineImageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            refineImageInfos[2].imageView = slot.coarseFlowView;
            refineImageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            refineImageInfos[3].imageView = slot.denseFlowView;
            refineImageInfos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            for (int i = 0; i < 4; ++i) {
                writes[7 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[7 + i].dstSet = slot.refineDescSet;
                writes[7 + i].dstBinding = i;
                writes[7 + i].descriptorCount = 1;
                writes[7 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[7 + i].pImageInfo = &refineImageInfos[i];
            }

            // Pass 3 (Warp & Blend): Frame0, Frame1, Intermediate, DenseFlow
            VkDescriptorImageInfo blendImageInfos[4]{};
            blendImageInfos[0].imageView = ctx->imageViews[ctx->prevGameIdx];
            blendImageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            blendImageInfos[1].imageView = ctx->imageViews[currentGameIdx];
            blendImageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            blendImageInfos[2].imageView = ctx->imageViews[intermediateIdx];
            blendImageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            blendImageInfos[3].imageView = slot.denseFlowView;
            blendImageInfos[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            for (int i = 0; i < 4; ++i) {
                writes[11 + i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                writes[11 + i].dstSet = slot.blendDescSet;
                writes[11 + i].dstBinding = i;
                writes[11 + i].descriptorCount = 1;
                writes[11 + i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                writes[11 + i].pImageInfo = &blendImageInfos[i];
            }

            g_pfnUpdateDescriptorSets(ctx->device, 15, writes, 0, nullptr);

            // --- 2. Transition Frame0, Frame1, Luma0, Luma1 for Pass 0 ---
            VkImageMemoryBarrier prePass0Barriers[4]{};
            prePass0Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            prePass0Barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            prePass0Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            prePass0Barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            prePass0Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            prePass0Barriers[0].image = ctx->images[ctx->prevGameIdx];
            prePass0Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            prePass0Barriers[0].subresourceRange.levelCount = 1;
            prePass0Barriers[0].subresourceRange.layerCount = 1;

            prePass0Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            prePass0Barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            prePass0Barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            prePass0Barriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            prePass0Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            prePass0Barriers[1].image = ctx->images[currentGameIdx];
            prePass0Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            prePass0Barriers[1].subresourceRange.levelCount = 1;
            prePass0Barriers[1].subresourceRange.layerCount = 1;

            prePass0Barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            prePass0Barriers[2].srcAccessMask = 0;
            prePass0Barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            prePass0Barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            prePass0Barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            prePass0Barriers[2].image = slot.lumaPyr0;
            prePass0Barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            prePass0Barriers[2].subresourceRange.levelCount = 1;
            prePass0Barriers[2].subresourceRange.layerCount = 1;

            prePass0Barriers[3].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            prePass0Barriers[3].srcAccessMask = 0;
            prePass0Barriers[3].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            prePass0Barriers[3].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            prePass0Barriers[3].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            prePass0Barriers[3].image = slot.lumaPyr1;
            prePass0Barriers[3].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            prePass0Barriers[3].subresourceRange.levelCount = 1;
            prePass0Barriers[3].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 4, prePass0Barriers);

            // Dispatch Pass 0 (Pyramid Downsampler: 1280x800 -> 320x200)
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->downPipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->downPipelineLayout, 0, 1, &slot.downDescSet, 0, nullptr);

            DownPushConstants downPc{
                static_cast<int>(fullW),
                static_cast<int>(fullH),
                static_cast<int>(pyrW),
                static_cast<int>(pyrH)
            };
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->downPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(downPc), &downPc);
            g_pfnCmdDispatch(slot.cmdBuffer, (pyrW + 7) / 8, (pyrH + 7) / 8, 1);

            // Barrier Pass 0 -> Pass 1:
            // lumaPyr0 & lumaPyr1: write -> read
            // coarseFlow: undefined -> write
            VkImageMemoryBarrier pass0To1Barriers[3]{};
            pass0To1Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass0To1Barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass0To1Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass0To1Barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass0To1Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass0To1Barriers[0].image = slot.lumaPyr0;
            pass0To1Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass0To1Barriers[0].subresourceRange.levelCount = 1;
            pass0To1Barriers[0].subresourceRange.layerCount = 1;

            pass0To1Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass0To1Barriers[1].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass0To1Barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass0To1Barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass0To1Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass0To1Barriers[1].image = slot.lumaPyr1;
            pass0To1Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass0To1Barriers[1].subresourceRange.levelCount = 1;
            pass0To1Barriers[1].subresourceRange.layerCount = 1;

            pass0To1Barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass0To1Barriers[2].srcAccessMask = 0;
            pass0To1Barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass0To1Barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pass0To1Barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass0To1Barriers[2].image = slot.coarseFlow;
            pass0To1Barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass0To1Barriers[2].subresourceRange.levelCount = 1;
            pass0To1Barriers[2].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, pass0To1Barriers);

            // Dispatch Pass 1 (Coarse Motion Search: 80x50 blocks)
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->coarsePipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->coarsePipelineLayout, 0, 1, &slot.coarseDescSet, 0, nullptr);

            CoarsePushConstants coarsePc{
                static_cast<int>(pyrW),
                static_cast<int>(pyrH),
                static_cast<int>(coarseW),
                static_cast<int>(coarseH)
            };
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->coarsePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(coarsePc), &coarsePc);
            g_pfnCmdDispatch(slot.cmdBuffer, coarseW, coarseH, 1);

            // Barrier Pass 1 -> Pass 2:
            // coarseFlow: write -> read
            // denseFlow: undefined -> write
            VkImageMemoryBarrier pass1To2Barriers[2]{};
            pass1To2Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass1To2Barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass1To2Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass1To2Barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass1To2Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass1To2Barriers[0].image = slot.coarseFlow;
            pass1To2Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass1To2Barriers[0].subresourceRange.levelCount = 1;
            pass1To2Barriers[0].subresourceRange.layerCount = 1;

            pass1To2Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass1To2Barriers[1].srcAccessMask = 0;
            pass1To2Barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass1To2Barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pass1To2Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass1To2Barriers[1].image = slot.denseFlow;
            pass1To2Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass1To2Barriers[1].subresourceRange.levelCount = 1;
            pass1To2Barriers[1].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, pass1To2Barriers);

            // Dispatch Pass 2 (Dense Flow Refinement: flowW x flowH grid)
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->refinePipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->refinePipelineLayout, 0, 1, &slot.refineDescSet, 0, nullptr);

            RefinePushConstants refinePc{
                static_cast<int>(fullW),
                static_cast<int>(fullH),
                static_cast<int>(flowW),
                static_cast<int>(flowH),
                static_cast<int>(coarseW),
                static_cast<int>(coarseH)
            };
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->refinePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(refinePc), &refinePc);
            g_pfnCmdDispatch(slot.cmdBuffer, (flowW + 7) / 8, (flowH + 7) / 8, 1);

            // Barrier Pass 2 -> Pass 3:
            // denseFlow: write -> read
            // intermediateIdx: undefined -> write
            VkImageMemoryBarrier pass2To3Barriers[2]{};
            pass2To3Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass2To3Barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass2To3Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass2To3Barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass2To3Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass2To3Barriers[0].image = slot.denseFlow;
            pass2To3Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass2To3Barriers[0].subresourceRange.levelCount = 1;
            pass2To3Barriers[0].subresourceRange.layerCount = 1;

            pass2To3Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass2To3Barriers[1].srcAccessMask = 0;
            pass2To3Barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass2To3Barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pass2To3Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass2To3Barriers[1].image = ctx->images[intermediateIdx];
            pass2To3Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass2To3Barriers[1].subresourceRange.levelCount = 1;
            pass2To3Barriers[1].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, pass2To3Barriers);

            // Dispatch Pass 3 (Warp & Bilinear Occlusion Blend: full resolution 1280x800)
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->blendPipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->blendPipelineLayout, 0, 1, &slot.blendDescSet, 0, nullptr);

            BlendPushConstants blendPc{
                0.5f,
                static_cast<int>(fullW),
                static_cast<int>(fullH),
                cfg.show_hud ? 1 : 0,
                cfg.hud_protection ? 1 : 0,
                cfg.mode,
                static_cast<int>(flowW),
                static_cast<int>(flowH)
            };
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->blendPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(blendPc), &blendPc);
            g_pfnCmdDispatch(slot.cmdBuffer, (fullW + 7) / 8, (fullH + 7) / 8, 1);

            // Final transitions back to PRESENT_SRC_KHR
            VkImageMemoryBarrier postBarriers[3]{};
            postBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            postBarriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            postBarriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            postBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            postBarriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            postBarriers[0].image = ctx->images[ctx->prevGameIdx];
            postBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            postBarriers[0].subresourceRange.levelCount = 1;
            postBarriers[0].subresourceRange.layerCount = 1;

            postBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            postBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            postBarriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            postBarriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            postBarriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            postBarriers[1].image = ctx->images[currentGameIdx];
            postBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            postBarriers[1].subresourceRange.levelCount = 1;
            postBarriers[1].subresourceRange.layerCount = 1;

            postBarriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            postBarriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            postBarriers[2].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            postBarriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            postBarriers[2].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            postBarriers[2].image = ctx->images[intermediateIdx];
            postBarriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            postBarriers[2].subresourceRange.levelCount = 1;
            postBarriers[2].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 3, postBarriers);

            usedHierarchical = true;
        } else if (g_pfnCmdCopyImage) {
            // Direct copy fallback
            VkImageMemoryBarrier barriers[2]{};
            barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].image = ctx->images[currentGameIdx];
            barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[0].subresourceRange.levelCount = 1;
            barriers[0].subresourceRange.layerCount = 1;

            barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            barriers[1].srcAccessMask = 0;
            barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].image = ctx->images[intermediateIdx];
            barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            barriers[1].subresourceRange.levelCount = 1;
            barriers[1].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

            VkImageCopy copyRegion{};
            copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.srcSubresource.layerCount = 1;
            copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            copyRegion.dstSubresource.layerCount = 1;
            copyRegion.extent.width = ctx->extent.width;
            copyRegion.extent.height = ctx->extent.height;
            copyRegion.extent.depth = 1;

            g_pfnCmdCopyImage(slot.cmdBuffer, ctx->images[currentGameIdx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, ctx->images[intermediateIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &copyRegion);

            barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
            barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
            barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
            barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
            barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
        }

        g_pfnEndCommandBuffer(slot.cmdBuffer);

        std::vector<VkSemaphore> waitSems;
        for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; ++i) {
            waitSems.push_back(pPresentInfo->pWaitSemaphores[i]);
        }
        waitSems.push_back(slot.acqSemaphore);
        std::vector<VkPipelineStageFlags> waitStages(waitSems.size(), VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);

        VkSemaphore signalSems[2] = { slot.finalDoneSemaphore, slot.interDoneSemaphore };

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
        si.pWaitSemaphores = waitSems.data();
        si.pWaitDstStageMask = waitStages.data();
        si.commandBufferCount = 1;
        si.pCommandBuffers = &slot.cmdBuffer;
        si.signalSemaphoreCount = 2;
        si.pSignalSemaphores = signalSems;

        {
            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            g_pfnQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);
        }

        // A. Present Intermediate Frame (F_{N-0.5}) IMMEDIATELY at t
        VkPresentInfoKHR interPresent = *pPresentInfo;
        interPresent.waitSemaphoreCount = 1;
        interPresent.pWaitSemaphores = &slot.interDoneSemaphore;
        interPresent.swapchainCount = 1;
        interPresent.pSwapchains = &ctx->swapchain;
        interPresent.pImageIndices = &intermediateIdx;

        VkResult res = VK_SUCCESS;
        {
            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            res = g_pfnQueuePresentKHR(queue, &interPresent);
        }

        // B. Queue Real Game Frame (F_N) for presentation at t + halfInterval on worker thread
        PendingPresent gameJob;
        gameJob.queue = queue;
        gameJob.swapchain = ctx->swapchain;
        gameJob.imageIndex = currentGameIdx;
        gameJob.waitSemaphore = slot.finalDoneSemaphore;
        gameJob.targetTime = now + std::chrono::nanoseconds(halfIntervalNs);

        {
            std::lock_guard<std::mutex> lock(ctx->presentMutex);
            ctx->pendingPresents.push_back(gameJob);
        }
        ctx->presentCv.notify_one();

        ctx->prevGameIdx = currentGameIdx;

        if (s_presentCount % 120 == 1) {
            Log("FrameGen ACTIVE: 2x presents paced! Base: %.1f FPS -> Output: %.1f FPS (Display: %d Hz | %s, step: %.1f ms / avg: %.1f ms)",
                ctx->pacer.GetBaseFps(), ctx->pacer.GetOutputFps(),
                ctx->pacer.GetTargetHz(),
                usedHierarchical ? "Pyramidal Coarse-to-Fine Optical Flow" : "Fallback Copy",
                static_cast<float>(halfIntervalNs) / 1e6f,
                static_cast<float>(ctx->pacer.GetAverageFrameTimeNs()) / 1e6f);
        }

        if (s_presentCount % 60 == 1) {
            FILE* fStats = fopen("/tmp/skyframe_stats.json", "w");
            if (fStats) {
                fprintf(fStats, "{\"base_fps\": %.1f, \"output_fps\": %.1f, \"target_hz\": %d, \"enabled\": true}\n",
                        ctx->pacer.GetBaseFps(), ctx->pacer.GetOutputFps(), ctx->pacer.GetTargetHz());
                fclose(fStats);
            }
        }
        return res;
    }

    // Fallback: If intermediate frame couldn't be acquired, present original frame cleanly
    ctx->prevGameIdx = currentGameIdx;
    VkResult res = VK_SUCCESS;
    {
        std::lock_guard<std::mutex> qlock(ctx->queueMutex);
        res = g_pfnQueuePresentKHR(queue, pPresentInfo);
    }
    return res;
}

} // namespace skyframe

__attribute__((constructor)) void skyframe_init() {
    char cmdline[512] = {0};
    FILE* fcmd = fopen("/proc/self/cmdline", "r");
    if (fcmd) {
        size_t n = fread(cmdline, 1, sizeof(cmdline) - 1, fcmd);
        fclose(fcmd);
        for (size_t i = 0; i < n; ++i) {
            if (cmdline[i] == '\0' && i + 1 < n) cmdline[i] = ' ';
        }
    }

    skyframe::Log("========================================");
    skyframe::Log("SkyFrame Native Vulkan Layer loaded!");
    skyframe::Log("PID: %d, Arch: %d-bit, Process: %s",
                  getpid(), (int)(sizeof(void*) * 8), cmdline[0] ? cmdline : "unknown");
    skyframe::ReloadConfig();
    auto& cfg = skyframe::GetConfig();
    skyframe::Log("Config status: enabled=%d, mode=%d, hud=%d", cfg.enabled, cfg.mode, cfg.hud_protection);
    skyframe::Log("========================================");
}

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct) {
    skyframe::Log("vkNegotiateLoaderLayerInterfaceVersion called! Loader interface version: %d",
                  pVersionStruct ? pVersionStruct->loaderLayerInterfaceVersion : -1);
    if (!pVersionStruct) return VK_ERROR_INITIALIZATION_FAILED;

    if (pVersionStruct->loaderLayerInterfaceVersion < 2) {
        skyframe::Log("Loader interface version < 2 not supported!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    pVersionStruct->loaderLayerInterfaceVersion = 2;
    pVersionStruct->pfnGetInstanceProcAddr = skyframe_GetInstanceProcAddr;
    pVersionStruct->pfnGetDeviceProcAddr = skyframe_GetDeviceProcAddr;
    pVersionStruct->pfnGetPhysicalDeviceProcAddr = nullptr;

    skyframe::Log("vkNegotiateLoaderLayerInterfaceVersion negotiated successfully!");
    return VK_SUCCESS;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL skyframe_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)skyframe_GetInstanceProcAddr;
    if (strcmp(pName, "vkCreateInstance") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkCreateInstance;
    if (strcmp(pName, "vkDestroyInstance") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkDestroyInstance;
    if (strcmp(pName, "vkCreateDevice") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkCreateDevice;

    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)skyframe_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkQueuePresentKHR;

    if (skyframe::g_nextGetInstanceProcAddr && instance) {
        return skyframe::g_nextGetInstanceProcAddr(instance, pName);
    }
    return nullptr;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL skyframe_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)skyframe_GetDeviceProcAddr;
    if (strcmp(pName, "vkDestroyDevice") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkDestroyDevice;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkQueuePresentKHR;

    if (skyframe::g_nextGetDeviceProcAddr && device) {
        return skyframe::g_nextGetDeviceProcAddr(device, pName);
    }
    return nullptr;
}

}
