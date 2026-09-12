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
#include <sys/stat.h>
#include <unistd.h>

#if defined(__x86_64__) || defined(_M_X64) || defined(__i386__) || defined(_M_IX86)
#include <immintrin.h>
#endif

#if __has_include("warp_blend_comp_spv.h")
#include "warp_blend_comp_spv.h"
#define HAVE_WARP_BLEND 1
#endif

#if __has_include("warp_rgba_comp_spv.h")
#include "warp_rgba_comp_spv.h"
#define HAVE_WARP_RGBA 1
#endif

#if __has_include("downsample_comp_spv.h")
#include "downsample_comp_spv.h"
#define HAVE_DOWNSAMPLE 1
#endif

namespace skyframe {

static LayerConfig g_config;
static std::mutex g_configMutex;
static uint32_t g_graphicsQueueFamily = 0;
static VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;

static std::atomic<bool> g_isInternalNcnnCall{false};
static std::mutex g_deviceMapMutex;
static std::unordered_map<VkDevice, VkPhysicalDevice> g_deviceToPhysicalDevice;
static std::unordered_map<VkDevice, PFN_vkGetDeviceProcAddr> g_deviceToGetDeviceProcAddr;

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
        Log("Hot-reload applied: enabled=%d, mode=%d, show_hud=%d, hud_protection=%d, target_hz=%d",
            cfg.enabled, cfg.mode, cfg.show_hud, cfg.hud_protection, cfg.target_hz);
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

PFN_vkCreateFence            g_pfnCreateFence = nullptr;
PFN_vkDestroyFence           g_pfnDestroyFence = nullptr;
PFN_vkResetFences            g_pfnResetFences = nullptr;
PFN_vkWaitForFences          g_pfnWaitForFences = nullptr;

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

constexpr size_t RING_SIZE = 4;

struct FrameSlot {
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    VkCommandBuffer readbackCmdBuffer = VK_NULL_HANDLE;
    VkSemaphore acqSemaphore = VK_NULL_HANDLE;
    VkSemaphore interDoneSemaphore = VK_NULL_HANDLE;
    VkSemaphore finalDoneSemaphore = VK_NULL_HANDLE;
    VkFence readbackFence = VK_NULL_HANDLE;
    VkDescriptorSet blendDescSet = VK_NULL_HANDLE;
    VkDescriptorSet warpDescSet = VK_NULL_HANDLE;
};

struct PendingPresent {
    VkQueue queue = VK_NULL_HANDLE;
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    uint32_t imageIndex = 0;
    VkSemaphore waitSemaphore = VK_NULL_HANDLE;
    std::chrono::steady_clock::time_point targetTime;
};

struct BlendPushConstants {
    float alpha;
    int   width;
    int   height;
    int   show_hud;
    int   hud_protection;
    int   mode;
};

struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkPhysicalDevice physicalDevice = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    FrameSlot slots[RING_SIZE];
    size_t ringIndex = 0;

    std::unique_ptr<VulkanWarper> warper;
    std::unique_ptr<FlowEstimator> flowEstimator;
    FramePacer pacer;

    // FastWarp RIFE neural flow buffers
    int flowWidth = 288;
    int flowHeight = 180;
    std::vector<float> flowBuffer;
    std::vector<float> maskBuffer;
    std::vector<uint8_t> prevDownsample;
    bool hasPrevDownsample = false;
    bool rifeEnabled = false;

    // Asynchronous Frame Pacer Worker
    std::thread presentWorkerThread;
    std::atomic<bool> workerRunning{false};
    std::mutex presentMutex;
    std::condition_variable presentCv;
    std::mutex queueMutex;
    std::deque<PendingPresent> pendingPresents;

    // Motion Smoothing Compute Pipeline
    uint32_t prevGameIdx = UINT32_MAX;
    VkShaderModule blendModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout blendDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout blendPipelineLayout = VK_NULL_HANDLE;
    VkPipeline blendPipeline = VK_NULL_HANDLE;
    VkDescriptorPool blendDescPool = VK_NULL_HANDLE;
    std::vector<VkImageView> imageViews;

    bool isInitialized = false;
};

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

            // High precision pacing until targetTime (system sleep to eliminate 100% CPU usage)
            if (job.targetTime > std::chrono::steady_clock::now()) {
                ctx->presentCv.wait_until(lock, job.targetTime, [&]() {
                    return !ctx->workerRunning.load() || ctx->pendingPresents.empty();
                });
                if (!ctx->workerRunning.load() || ctx->pendingPresents.empty()) continue;
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

            job = ctx->pendingPresents.front();
            ctx->pendingPresents.pop_front();
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

static bool InitBlendPipeline(SwapchainContext* ctx) {
#if HAVE_WARP_BLEND
    if (!g_pfnCreateShaderModule || !g_pfnCreateDescriptorSetLayout ||
        !g_pfnCreatePipelineLayout || !g_pfnCreateComputePipelines ||
        !g_pfnCreateDescriptorPool || !g_pfnAllocateDescriptorSets ||
        !g_pfnCreateImageView) {
        Log("Blend pipeline: required Vulkan function pointers missing");
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
                Log("Blend pipeline: failed to create image view for image %zu", i);
                return false;
            }
        }
    }

    if (ctx->blendPipeline != VK_NULL_HANDLE) {
        return true;
    }

    // 2. Create Shader Module
    VkShaderModuleCreateInfo smci{};
    smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    smci.codeSize = warp_blend_comp_spv_size;
    smci.pCode = warp_blend_comp_spv;

    if (g_pfnCreateShaderModule(ctx->device, &smci, nullptr, &ctx->blendModule) != VK_SUCCESS) {
        Log("Blend pipeline: failed to create shader module");
        return false;
    }

    // 3. Descriptor Set Layout (3 storage images: Frame0, Frame1, OutImage)
    VkDescriptorSetLayoutBinding bindings[3]{};
    for (int i = 0; i < 3; ++i) {
        bindings[i].binding = i;
        bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        bindings[i].descriptorCount = 1;
        bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }

    VkDescriptorSetLayoutCreateInfo dslci{};
    dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dslci.bindingCount = 3;
    dslci.pBindings = bindings;

    if (g_pfnCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &ctx->blendDescLayout) != VK_SUCCESS) {
        Log("Blend pipeline: failed to create descriptor set layout");
        return false;
    }

    // 4. Pipeline Layout with Push Constants
    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(BlendPushConstants);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &ctx->blendDescLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;

    if (g_pfnCreatePipelineLayout(ctx->device, &plci, nullptr, &ctx->blendPipelineLayout) != VK_SUCCESS) {
        Log("Blend pipeline: failed to create pipeline layout");
        return false;
    }

    // 5. Compute Pipeline
    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = ctx->blendModule;
    cpci.stage.pName = "main";
    cpci.layout = ctx->blendPipelineLayout;

    if (g_pfnCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &ctx->blendPipeline) != VK_SUCCESS) {
        Log("Blend pipeline: failed to create compute pipeline");
        return false;
    }

    // 6. Descriptor Pool & Sets for Ring Buffer
    VkDescriptorPoolSize poolSize{};
    poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    poolSize.descriptorCount = static_cast<uint32_t>(RING_SIZE * 3);

    VkDescriptorPoolCreateInfo dpci{};
    dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    dpci.maxSets = static_cast<uint32_t>(RING_SIZE);
    dpci.poolSizeCount = 1;
    dpci.pPoolSizes = &poolSize;

    if (g_pfnCreateDescriptorPool(ctx->device, &dpci, nullptr, &ctx->blendDescPool) != VK_SUCCESS) {
        Log("Blend pipeline: failed to create descriptor pool");
        return false;
    }

    VkDescriptorSetLayout layouts[RING_SIZE];
    for (size_t i = 0; i < RING_SIZE; ++i) layouts[i] = ctx->blendDescLayout;

    VkDescriptorSetAllocateInfo dsai{};
    dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    dsai.descriptorPool = ctx->blendDescPool;
    dsai.descriptorSetCount = static_cast<uint32_t>(RING_SIZE);
    dsai.pSetLayouts = layouts;

    VkDescriptorSet sets[RING_SIZE];
    if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, sets) != VK_SUCCESS) {
        Log("Blend pipeline: failed to allocate descriptor sets");
        return false;
    }

    for (size_t i = 0; i < RING_SIZE; ++i) {
        ctx->slots[i].blendDescSet = sets[i];
    }

    Log("Blend pipeline: successfully initialized compute shader for motion smoothing!");
    return true;
#else
    Log("Blend pipeline: warp_blend_comp_spv.h not available");
    return false;
#endif
}

static bool InitRifePipeline(SwapchainContext* ctx) {
    if (!ctx) return false;

    ctx->flowEstimator = std::make_unique<FlowEstimator>();

    const char* home = getenv("HOME");
    std::vector<std::string> modelDirs;
    if (home && strlen(home) > 0) {
        modelDirs.push_back(std::string(home) + "/.local/share/skyframe/models");
        modelDirs.push_back(std::string(home) + "/homebrew/plugins/SkyFrame/bin/models");
    }
    modelDirs.push_back("/home/deck/.local/share/skyframe/models");
    modelDirs.push_back("/home/deck/homebrew/plugins/SkyFrame/bin/models");
    modelDirs.push_back("./models");

    bool modelLoaded = false;
    for (const auto& dir : modelDirs) {
        std::string param = dir + "/flownet.param";
        if (access(param.c_str(), R_OK) == 0) {
            g_isInternalNcnnCall.store(true);
            bool ok = ctx->flowEstimator->LoadModel(dir);
            g_isInternalNcnnCall.store(false);
            if (ok) {
                Log("FlowEstimator loaded RIFE model from %s", dir.c_str());
                modelLoaded = true;
                break;
            }
        }
    }

    if (!modelLoaded) {
        Log("RIFE model (flownet.param / flownet.bin) not found or failed to load, falling back to compute blend.");
        return false;
    }

    if (ctx->physicalDevice == VK_NULL_HANDLE) {
        ctx->physicalDevice = g_physicalDevice;
    }
    if (ctx->physicalDevice == VK_NULL_HANDLE) {
        Log("ctx->physicalDevice is null, cannot init VulkanWarper for RIFE");
        return false;
    }

    ctx->warper = std::make_unique<VulkanWarper>(ctx->device, ctx->physicalDevice, (VkQueue)VK_NULL_HANDLE, g_graphicsQueueFamily);

#if HAVE_WARP_RGBA
    size_t downSize = 0;
    const uint32_t* downPtr = nullptr;
#if HAVE_DOWNSAMPLE
    downSize = downsample_comp_spv_size;
    downPtr = downsample_comp_spv;
#endif

    if (!ctx->warper->InitPipelines(warp_rgba_comp_spv, warp_rgba_comp_spv_size, downPtr, downSize)) {
        Log("Failed to initialize VulkanWarper pipelines");
        return false;
    }
#else
    Log("warp_rgba_comp_spv.h not available");
    return false;
#endif

    auto& cfg = GetConfig();
    ctx->flowWidth = ctx->flowEstimator->GetOptimalFlowWidth(cfg.mode, ctx->extent.width);
    ctx->flowHeight = ctx->flowEstimator->GetOptimalFlowHeight(cfg.mode, ctx->extent.height);

    if (!ctx->warper->CreateFlowAndMaskTextures(ctx->flowWidth, ctx->flowHeight) ||
        !ctx->warper->CreateDownsampleStaging(ctx->flowWidth, ctx->flowHeight, ctx->format)) {
        Log("Failed to create Flow/Mask or Downsample textures in VulkanWarper");
        return false;
    }

    for (size_t i = 0; i < RING_SIZE; ++i) {
        ctx->slots[i].warpDescSet = ctx->warper->AllocateWarpDescriptorSet();
    }

    ctx->flowBuffer.resize(ctx->flowWidth * ctx->flowHeight * 2);
    ctx->maskBuffer.resize(ctx->flowWidth * ctx->flowHeight);
    ctx->prevDownsample.resize(ctx->flowWidth * ctx->flowHeight * 4);
    ctx->hasPrevDownsample = false;
    ctx->rifeEnabled = true;

    Log("RIFE FastWarp initialized: %dx%d dense optical flow for %ux%u native output!",
        ctx->flowWidth, ctx->flowHeight, ctx->extent.width, ctx->extent.height);
    return true;
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

    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateInstance nextCreateInstance = (PFN_vkCreateInstance)nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance");
    if (!nextCreateInstance) {
        Log("ERROR: Failed to find next vkCreateInstance!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    VkResult res = nextCreateInstance(pCreateInfo, pAllocator, pInstance);
    if (res != VK_SUCCESS) {
        Log("vkCreateInstance downstream returned error: %d", res);
        return res;
    }

    bool isNcnn = g_isInternalNcnnCall.load();
    if (pCreateInfo && pCreateInfo->pApplicationInfo) {
        if ((pCreateInfo->pApplicationInfo->pApplicationName && strcmp(pCreateInfo->pApplicationInfo->pApplicationName, "ncnn") == 0) ||
            (pCreateInfo->pApplicationInfo->pEngineName && strcmp(pCreateInfo->pApplicationInfo->pEngineName, "ncnn") == 0)) {
            isNcnn = true;
        }
    }

    if (isNcnn) {
        Log("Hook_vkCreateInstance: NCNN Vulkan Instance created, bypassing layer state capture.");
        return VK_SUCCESS;
    }

    g_nextGetInstanceProcAddr = nextGetInstanceProcAddr;
    g_nextCreateInstance = nextCreateInstance;
    g_nextDestroyInstance = (PFN_vkDestroyInstance)g_nextGetInstanceProcAddr(*pInstance, "vkDestroyInstance");
    g_nextCreateDevice = (PFN_vkCreateDevice)g_nextGetInstanceProcAddr(*pInstance, "vkCreateDevice");

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
    Log("Hook_vkCreateDevice called (internalNcnn=%d)", (int)g_isInternalNcnnCall.load());
    VkLayerDeviceCreateInfo* chain_info = (VkLayerDeviceCreateInfo*)pCreateInfo->pNext;
    while (chain_info && (chain_info->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
                          chain_info->function != VK_LAYER_LINK_INFO)) {
        chain_info = (VkLayerDeviceCreateInfo*)chain_info->pNext;
    }

    if (!chain_info) {
        Log("ERROR: No VK_LAYER_LINK_INFO found in vkCreateDevice chain!");
        return VK_ERROR_INITIALIZATION_FAILED;
    }

    PFN_vkGetDeviceProcAddr nextGetDeviceProcAddr = chain_info->u.pLayerInfo->pfnNextGetDeviceProcAddr;
    PFN_vkGetInstanceProcAddr nextGetInstanceProcAddr = chain_info->u.pLayerInfo->pfnNextGetInstanceProcAddr;
    chain_info->u.pLayerInfo = chain_info->u.pLayerInfo->pNext;

    PFN_vkCreateDevice nextCreateDevice = (PFN_vkCreateDevice)nextGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateDevice");
    if (!nextCreateDevice) nextCreateDevice = g_nextCreateDevice;

    VkResult res = nextCreateDevice(physicalDevice, pCreateInfo, pAllocator, pDevice);
    if (res != VK_SUCCESS) {
        Log("vkCreateDevice downstream returned error: %d", res);
        return res;
    }

    if (g_isInternalNcnnCall.load()) {
        Log("Hook_vkCreateDevice: NCNN Vulkan Device created, bypassing layer state capture.");
        return VK_SUCCESS;
    }

    g_nextGetDeviceProcAddr = nextGetDeviceProcAddr;
    g_nextGetInstanceProcAddr = nextGetInstanceProcAddr;
    g_physicalDevice = physicalDevice;

    {
        std::lock_guard<std::mutex> lock(g_deviceMapMutex);
        g_deviceToPhysicalDevice[*pDevice] = physicalDevice;
        g_deviceToGetDeviceProcAddr[*pDevice] = nextGetDeviceProcAddr;
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

    g_pfnCreateFence = (PFN_vkCreateFence)g_nextGetDeviceProcAddr(*pDevice, "vkCreateFence");
    g_pfnDestroyFence = (PFN_vkDestroyFence)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyFence");
    g_pfnResetFences = (PFN_vkResetFences)g_nextGetDeviceProcAddr(*pDevice, "vkResetFences");
    g_pfnWaitForFences = (PFN_vkWaitForFences)g_nextGetDeviceProcAddr(*pDevice, "vkWaitForFences");

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
    if (!g_pfnCreateFence) g_pfnCreateFence = &vkCreateFence;
    if (!g_pfnDestroyFence) g_pfnDestroyFence = &vkDestroyFence;
    if (!g_pfnResetFences) g_pfnResetFences = &vkResetFences;
    if (!g_pfnWaitForFences) g_pfnWaitForFences = &vkWaitForFences;

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

    Log("Vulkan Device created successfully. Dispatch table initialized.");
    return VK_SUCCESS;
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroyDevice(
    VkDevice device,
    const VkAllocationCallbacks* pAllocator
) {
    Log("Hook_vkDestroyDevice called");
    PFN_vkDestroyDevice nextDestroyDevice = nullptr;
    {
        std::lock_guard<std::mutex> lock(g_deviceMapMutex);
        auto it = g_deviceToGetDeviceProcAddr.find(device);
        if (it != g_deviceToGetDeviceProcAddr.end() && it->second) {
            nextDestroyDevice = (PFN_vkDestroyDevice)it->second(device, "vkDestroyDevice");
        }
        g_deviceToPhysicalDevice.erase(device);
        g_deviceToGetDeviceProcAddr.erase(device);
    }
    if (!nextDestroyDevice) nextDestroyDevice = g_nextDestroyDevice;
    if (nextDestroyDevice) {
        nextDestroyDevice(device, pAllocator);
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
    modifiedCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;

    VkResult res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) {
        Log("Swapchain creation with SAMPLED_BIT failed (%d), retrying with STORAGE_BIT", res);
        modifiedCi.imageUsage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;
        if (modifiedCi.minImageCount < 4) modifiedCi.minImageCount = 4;
        res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
        if (res != VK_SUCCESS) {
            Log("Swapchain creation with STORAGE_BIT failed (%d), retrying default", res);
            modifiedCi.imageUsage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
            res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
        }
    }
    if (res != VK_SUCCESS || !pSwapchain) return res;

    auto ctx = std::make_shared<SwapchainContext>();
    ctx->swapchain = *pSwapchain;
    ctx->device = device;
    {
        std::lock_guard<std::mutex> lock(g_deviceMapMutex);
        auto it = g_deviceToPhysicalDevice.find(device);
        if (it != g_deviceToPhysicalDevice.end()) {
            ctx->physicalDevice = it->second;
        } else {
            ctx->physicalDevice = g_physicalDevice;
        }
    }
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
            VkCommandBuffer rawCmds[RING_SIZE * 2];
            VkCommandBufferAllocateInfo cbai{};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = ctx->cmdPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = RING_SIZE * 2;
            if (g_pfnAllocateCommandBuffers(device, &cbai, rawCmds) == VK_SUCCESS) {
                VkSemaphoreCreateInfo sci{};
                sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;

                VkFenceCreateInfo fci{};
                fci.sType = VK_STRUCTURE_TYPE_FENCE_CREATE_INFO;
                fci.flags = VK_FENCE_CREATE_SIGNALED_BIT;

                for (size_t i = 0; i < RING_SIZE; ++i) {
                    ctx->slots[i].cmdBuffer = rawCmds[i];
                    ctx->slots[i].readbackCmdBuffer = rawCmds[RING_SIZE + i];
                    g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->slots[i].acqSemaphore);
                    g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->slots[i].interDoneSemaphore);
                    g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->slots[i].finalDoneSemaphore);
                    if (g_pfnCreateFence) {
                        g_pfnCreateFence(device, &fci, nullptr, &ctx->slots[i].readbackFence);
                    }
                }
            }
        }
    }

    InitBlendPipeline(ctx.get());
    InitRifePipeline(ctx.get());

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
            if (ctx->blendDescPool && g_pfnDestroyDescriptorPool) g_pfnDestroyDescriptorPool(device, ctx->blendDescPool, nullptr);
            if (ctx->blendDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->blendDescLayout, nullptr);
            if (ctx->blendModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->blendModule, nullptr);
            for (auto iv : ctx->imageViews) {
                if (iv && g_pfnDestroyImageView) g_pfnDestroyImageView(device, iv, nullptr);
            }

            for (size_t i = 0; i < RING_SIZE; ++i) {
                if (ctx->slots[i].acqSemaphore && g_pfnDestroySemaphore) g_pfnDestroySemaphore(device, ctx->slots[i].acqSemaphore, nullptr);
                if (ctx->slots[i].interDoneSemaphore && g_pfnDestroySemaphore) g_pfnDestroySemaphore(device, ctx->slots[i].interDoneSemaphore, nullptr);
                if (ctx->slots[i].finalDoneSemaphore && g_pfnDestroySemaphore) g_pfnDestroySemaphore(device, ctx->slots[i].finalDoneSemaphore, nullptr);
                if (ctx->slots[i].readbackFence && g_pfnDestroyFence) g_pfnDestroyFence(device, ctx->slots[i].readbackFence, nullptr);
            }
            if (ctx->cmdPool && g_pfnDestroyCommandPool) {
                g_pfnDestroyCommandPool(device, ctx->cmdPool, nullptr);
            }
            ctx->warper.reset();
            ctx->flowEstimator.reset();
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
        InitBlendPipeline(it->second.get());
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

    // 0. Base frame initialization: if this is the very first frame or prevDownsample is unseeded
    if (ctx->prevGameIdx == UINT32_MAX || !ctx->hasPrevDownsample) {
        if (ctx->rifeEnabled && ctx->warper && ctx->flowEstimator && ctx->flowEstimator->IsLoaded() &&
            slot.readbackCmdBuffer != VK_NULL_HANDLE && slot.readbackFence != VK_NULL_HANDLE &&
            g_pfnBeginCommandBuffer && g_pfnEndCommandBuffer && g_pfnResetFences && g_pfnWaitForFences && g_pfnQueueSubmit) {

            VkCommandBufferBeginInfo rbi{};
            rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            g_pfnBeginCommandBuffer(slot.readbackCmdBuffer, &rbi);

            ctx->warper->ReadbackDownsample(
                slot.readbackCmdBuffer,
                ctx->images[currentGameIdx],
                static_cast<int>(ctx->extent.width), static_cast<int>(ctx->extent.height),
                ctx->flowWidth, ctx->flowHeight
            );

            g_pfnEndCommandBuffer(slot.readbackCmdBuffer);

            g_pfnResetFences(ctx->device, 1, &slot.readbackFence);

            std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            VkSubmitInfo si{};
            si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            si.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
            si.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
            si.pWaitDstStageMask = waitStages.data();
            si.commandBufferCount = 1;
            si.pCommandBuffers = &slot.readbackCmdBuffer;

            {
                std::lock_guard<std::mutex> qlock(ctx->queueMutex);
                g_pfnQueueSubmit(queue, 1, &si, slot.readbackFence);
            }

            if (g_pfnWaitForFences(ctx->device, 1, &slot.readbackFence, VK_TRUE, 50000000ULL) == VK_SUCCESS) {
                const unsigned char* pixels = ctx->warper->GetDownsamplePixels();
                if (pixels) {
                    size_t downBytes = (size_t)ctx->flowWidth * ctx->flowHeight * 4;
                    memcpy(ctx->prevDownsample.data(), pixels, downBytes);
                    ctx->hasPrevDownsample = true;
                }
            }

            ctx->prevGameIdx = currentGameIdx;
            VkPresentInfoKHR initialPresent = *pPresentInfo;
            initialPresent.waitSemaphoreCount = 0;
            initialPresent.pWaitSemaphores = nullptr;
            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            return g_pfnQueuePresentKHR(queue, &initialPresent);
        }

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

        bool usedRife = false;
        bool usedBlend = false;

        // PATH A: RIFE FlowNet (NCNN Vulkan) + Native FastWarp (1280x800)
        if (ctx->rifeEnabled && ctx->warper && ctx->flowEstimator && ctx->flowEstimator->IsLoaded() &&
            ctx->hasPrevDownsample && ctx->prevGameIdx != UINT32_MAX &&
            ctx->prevGameIdx < ctx->images.size() && ctx->prevGameIdx != intermediateIdx &&
            ctx->prevGameIdx != currentGameIdx &&
            ctx->prevGameIdx < ctx->imageViews.size() && currentGameIdx < ctx->imageViews.size() &&
            intermediateIdx < ctx->imageViews.size() &&
            ctx->imageViews[ctx->prevGameIdx] != VK_NULL_HANDLE &&
            ctx->imageViews[currentGameIdx] != VK_NULL_HANDLE &&
            ctx->imageViews[intermediateIdx] != VK_NULL_HANDLE &&
            slot.readbackCmdBuffer != VK_NULL_HANDLE && slot.readbackFence != VK_NULL_HANDLE &&
            slot.warpDescSet != VK_NULL_HANDLE &&
            g_pfnResetFences && g_pfnWaitForFences) {

            // 1. Hardware downsample readback for currentGameIdx
            VkCommandBufferBeginInfo rbi{};
            rbi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            rbi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            g_pfnBeginCommandBuffer(slot.readbackCmdBuffer, &rbi);

            ctx->warper->ReadbackDownsample(
                slot.readbackCmdBuffer,
                ctx->images[currentGameIdx],
                static_cast<int>(ctx->extent.width), static_cast<int>(ctx->extent.height),
                ctx->flowWidth, ctx->flowHeight
            );

            g_pfnEndCommandBuffer(slot.readbackCmdBuffer);

            g_pfnResetFences(ctx->device, 1, &slot.readbackFence);

            std::vector<VkPipelineStageFlags> waitStages(pPresentInfo->waitSemaphoreCount, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT);
            VkSubmitInfo rsi{};
            rsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            rsi.waitSemaphoreCount = pPresentInfo->waitSemaphoreCount;
            rsi.pWaitSemaphores = pPresentInfo->pWaitSemaphores;
            rsi.pWaitDstStageMask = waitStages.data();
            rsi.commandBufferCount = 1;
            rsi.pCommandBuffers = &slot.readbackCmdBuffer;

            {
                std::lock_guard<std::mutex> qlock(ctx->queueMutex);
                g_pfnQueueSubmit(queue, 1, &rsi, slot.readbackFence);
            }

            if (g_pfnWaitForFences(ctx->device, 1, &slot.readbackFence, VK_TRUE, 50000000ULL) == VK_SUCCESS) {
                const unsigned char* currPixels = ctx->warper->GetDownsamplePixels();
                if (currPixels) {
                    int pixelType = (ctx->format == VK_FORMAT_B8G8R8A8_UNORM || ctx->format == VK_FORMAT_B8G8R8A8_SRGB) ? 1 : 0;

                    // 2. RIFE FlowNet optical flow & mask inference
                    bool estOk = ctx->flowEstimator->EstimateFlow(
                        ctx->prevDownsample.data(),
                        currPixels,
                        ctx->flowWidth, ctx->flowHeight,
                        pixelType,
                        ctx->flowBuffer.data(),
                        ctx->maskBuffer.data(),
                        ctx->flowWidth, ctx->flowHeight
                    );

                    if (estOk) {
                        size_t downBytes = (size_t)ctx->flowWidth * ctx->flowHeight * 4;
                        memcpy(ctx->prevDownsample.data(), currPixels, downBytes);

                        // 3. Record FastWarp command buffer at 100% native resolution
                        VkCommandBufferBeginInfo bi{};
                        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
                        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
                        g_pfnBeginCommandBuffer(slot.cmdBuffer, &bi);

                        ctx->warper->UpdateFlowAndMask(
                            slot.cmdBuffer,
                            ctx->flowBuffer.data(),
                            ctx->maskBuffer.data(),
                            ctx->flowWidth, ctx->flowHeight
                        );

                        VkImageMemoryBarrier barriers[3]{};
                        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                        barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                        barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barriers[0].image = ctx->images[ctx->prevGameIdx];
                        barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                        barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                        barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barriers[1].image = ctx->images[currentGameIdx];
                        barriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                        barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                        barriers[2].srcAccessMask = 0;
                        barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                        barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                        barriers[2].image = ctx->images[intermediateIdx];
                        barriers[2].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

                        g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                                                VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);

                        ctx->warper->WarpFrame(
                            slot.cmdBuffer,
                            slot.warpDescSet,
                            ctx->imageViews[ctx->prevGameIdx],
                            ctx->imageViews[currentGameIdx],
                            ctx->warper->GetFlowImageView(),
                            ctx->warper->GetMaskImageView(),
                            ctx->imageViews[intermediateIdx],
                            static_cast<int>(ctx->extent.width),
                            static_cast<int>(ctx->extent.height),
                            0.5f,
                            cfg.hud_protection != 0,
                            0.08f,
                            cfg.show_hud != 0
                        );

                        barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                        barriers[0].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                        barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                        barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                        barriers[1].oldLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
                        barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                        barriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                        barriers[2].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                        barriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                        barriers[2].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                        g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT,
                                                VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);

                        g_pfnEndCommandBuffer(slot.cmdBuffer);

                        usedRife = true;
                    }
                }
            }
        }

        if (usedRife) {
            // Readback already waited on game's pWaitSemaphores, so we only wait on slot.acqSemaphore
            VkPipelineStageFlags waitDstStage = VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT;
            VkSemaphore signalSems[2] = { slot.finalDoneSemaphore, slot.interDoneSemaphore };

            VkSubmitInfo wsi{};
            wsi.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
            wsi.waitSemaphoreCount = 1;
            wsi.pWaitSemaphores = &slot.acqSemaphore;
            wsi.pWaitDstStageMask = &waitDstStage;
            wsi.commandBufferCount = 1;
            wsi.pCommandBuffers = &slot.cmdBuffer;
            wsi.signalSemaphoreCount = 2;
            wsi.pSignalSemaphores = signalSems;

            {
                std::lock_guard<std::mutex> qlock(ctx->queueMutex);
                g_pfnQueueSubmit(queue, 1, &wsi, VK_NULL_HANDLE);
            }
        } else {
            // PATH B: Fallback Compute Blend or Copy
            VkCommandBufferBeginInfo bi{};
            bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
            bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
            g_pfnBeginCommandBuffer(slot.cmdBuffer, &bi);

            if (ctx->blendPipeline != VK_NULL_HANDLE && ctx->prevGameIdx != UINT32_MAX &&
                ctx->prevGameIdx < ctx->images.size() && ctx->prevGameIdx != intermediateIdx &&
                ctx->prevGameIdx < ctx->imageViews.size() && currentGameIdx < ctx->imageViews.size() &&
                intermediateIdx < ctx->imageViews.size() &&
                ctx->imageViews[ctx->prevGameIdx] != VK_NULL_HANDLE &&
                ctx->imageViews[currentGameIdx] != VK_NULL_HANDLE &&
                ctx->imageViews[intermediateIdx] != VK_NULL_HANDLE &&
                slot.blendDescSet != VK_NULL_HANDLE && g_pfnUpdateDescriptorSets &&
                g_pfnCmdBindPipeline && g_pfnCmdBindDescriptorSets && g_pfnCmdPushConstants && g_pfnCmdDispatch) {

                // Motion Smoothing: Blend Frame N-1 and Frame N into intermediateIdx at phase 0.5
                VkDescriptorImageInfo imageInfos[3]{};
                imageInfos[0].imageView = ctx->imageViews[ctx->prevGameIdx];
                imageInfos[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                imageInfos[1].imageView = ctx->imageViews[currentGameIdx];
                imageInfos[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
                imageInfos[2].imageView = ctx->imageViews[intermediateIdx];
                imageInfos[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

                VkWriteDescriptorSet writes[3]{};
                for (int i = 0; i < 3; ++i) {
                    writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                    writes[i].dstSet = slot.blendDescSet;
                    writes[i].dstBinding = i;
                    writes[i].descriptorCount = 1;
                    writes[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                    writes[i].pImageInfo = &imageInfos[i];
                }
                g_pfnUpdateDescriptorSets(ctx->device, 3, writes, 0, nullptr);

                VkImageMemoryBarrier barriers[3]{};
                barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[0].image = ctx->images[ctx->prevGameIdx];
                barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barriers[0].subresourceRange.levelCount = 1;
                barriers[0].subresourceRange.layerCount = 1;

                barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
                barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[1].image = ctx->images[currentGameIdx];
                barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barriers[1].subresourceRange.levelCount = 1;
                barriers[1].subresourceRange.layerCount = 1;

                barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
                barriers[2].srcAccessMask = 0;
                barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
                barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[2].image = ctx->images[intermediateIdx];
                barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
                barriers[2].subresourceRange.levelCount = 1;
                barriers[2].subresourceRange.layerCount = 1;

                g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);

                g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->blendPipeline);
                g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->blendPipelineLayout, 0, 1, &slot.blendDescSet, 0, nullptr);

                BlendPushConstants pc{
                    0.5f,
                    static_cast<int>(ctx->extent.width),
                    static_cast<int>(ctx->extent.height),
                    cfg.show_hud ? 1 : 0,
                    cfg.hud_protection ? 1 : 0,
                    cfg.mode
                };
                g_pfnCmdPushConstants(slot.cmdBuffer, ctx->blendPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

                uint32_t groupX = (ctx->extent.width + 7) / 8;
                uint32_t groupY = (ctx->extent.height + 7) / 8;
                g_pfnCmdDispatch(slot.cmdBuffer, groupX, groupY, 1);

                barriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                barriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
                barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                barriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                barriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
                barriers[2].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
                barriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
                barriers[2].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

                g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 3, barriers);

                usedBlend = true;
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
                usedRife ? "RIFE FlowNet + Native FastWarp (1280x800)" : (usedBlend ? "Compute Motion Blend" : "Fallback Copy"),
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

    PFN_vkGetDeviceProcAddr nextPfn = nullptr;
    if (device) {
        std::lock_guard<std::mutex> lock(skyframe::g_deviceMapMutex);
        auto it = skyframe::g_deviceToGetDeviceProcAddr.find(device);
        if (it != skyframe::g_deviceToGetDeviceProcAddr.end()) {
            nextPfn = it->second;
        }
    }
    if (!nextPfn) nextPfn = skyframe::g_nextGetDeviceProcAddr;

    if (nextPfn && device) {
        return nextPfn(device, pName);
    }
    return nullptr;
}

}
