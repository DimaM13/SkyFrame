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

#if __has_include("motion_field_comp_spv.h")
#include "motion_field_comp_spv.h"
#define HAVE_MOTION_FIELD 1
#endif

#if __has_include("flow_filter_comp_spv.h")
#include "flow_filter_comp_spv.h"
#define HAVE_FLOW_FILTER 1
#endif

#if __has_include("warp_blend_comp_spv.h")
#include "warp_blend_comp_spv.h"
#define HAVE_WARP_BLEND 1
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

PFN_vkCreateImage               g_pfnCreateImage = nullptr;
PFN_vkDestroyImage              g_pfnDestroyImage = nullptr;
PFN_vkGetImageMemoryRequirements g_pfnGetImageMemoryRequirements = nullptr;
PFN_vkGetPhysicalDeviceMemoryProperties g_pfnGetPhysicalDeviceMemoryProperties = nullptr;
PFN_vkAllocateMemory            g_pfnAllocateMemory = nullptr;
PFN_vkFreeMemory                g_pfnFreeMemory = nullptr;
PFN_vkBindImageMemory           g_pfnBindImageMemory = nullptr;

static VkPhysicalDevice g_physicalDevice = VK_NULL_HANDLE;

constexpr size_t RING_SIZE = 8;

struct FrameSlot {
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    VkSemaphore acqSemaphore = VK_NULL_HANDLE;
    VkSemaphore interDoneSemaphore = VK_NULL_HANDLE;
    VkSemaphore finalDoneSemaphore = VK_NULL_HANDLE;
    VkDescriptorSet motionDescSet = VK_NULL_HANDLE;
    VkDescriptorSet filterDescSet = VK_NULL_HANDLE;
    VkDescriptorSet blendDescSet = VK_NULL_HANDLE;
};

struct MotionPushConstants {
    int width;
    int height;
};

struct FilterPushConstants {
    int width;
    int height;
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
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    FrameSlot slots[RING_SIZE];
    size_t ringIndex = 0;

    FramePacer pacer;
    std::mutex queueMutex;

    // Pass 1: 360-Degree Motion Field (R16G16_SFLOAT, 160x100 for 1280x800)
    VkImage rawMotionImage = VK_NULL_HANDLE;
    VkDeviceMemory rawMotionMemory = VK_NULL_HANDLE;
    VkImageView rawMotionImageView = VK_NULL_HANDLE;

    // Pass 2: 3x3 Spatial Vector Median Regularized Field (R16G16_SFLOAT)
    VkImage filteredMotionImage = VK_NULL_HANDLE;
    VkDeviceMemory filteredMotionMemory = VK_NULL_HANDLE;
    VkImageView filteredMotionImageView = VK_NULL_HANDLE;

    // Pass 1 Pipeline: Coarse Motion Estimation
    VkShaderModule motionModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout motionDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout motionPipelineLayout = VK_NULL_HANDLE;
    VkPipeline motionPipeline = VK_NULL_HANDLE;
    VkDescriptorPool motionDescPool = VK_NULL_HANDLE;

    // Pass 2 Pipeline: 3x3 Vector Median Regularization
    VkShaderModule filterModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout filterDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout filterPipelineLayout = VK_NULL_HANDLE;
    VkPipeline filterPipeline = VK_NULL_HANDLE;
    VkDescriptorPool filterDescPool = VK_NULL_HANDLE;

    // Pass 3 Pipeline: Dense Bilinear Warp & Soft Temporal Blend
    uint32_t prevGameIdx = UINT32_MAX;
    VkShaderModule blendModule = VK_NULL_HANDLE;
    VkDescriptorSetLayout blendDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout blendPipelineLayout = VK_NULL_HANDLE;
    VkPipeline blendPipeline = VK_NULL_HANDLE;
    VkDescriptorPool blendDescPool = VK_NULL_HANDLE;
    std::vector<VkImageView> imageViews;

    bool isInitialized = false;
};

static bool InitBlendPipeline(SwapchainContext* ctx) {
#if HAVE_WARP_BLEND && HAVE_MOTION_FIELD
    if (!g_pfnCreateShaderModule || !g_pfnCreateDescriptorSetLayout ||
        !g_pfnCreatePipelineLayout || !g_pfnCreateComputePipelines ||
        !g_pfnCreateDescriptorPool || !g_pfnAllocateDescriptorSets ||
        !g_pfnCreateImageView || !g_pfnCreateImage ||
        !g_pfnGetImageMemoryRequirements || !g_pfnAllocateMemory ||
        !g_pfnBindImageMemory) {
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

    // 2. Helper to create R16G16_SFLOAT Storage Images
    uint32_t mfWidth = (ctx->extent.width + 7) / 8;
    uint32_t mfHeight = (ctx->extent.height + 7) / 8;

    auto createStorageImage = [&](uint32_t width, uint32_t height, VkImage& outImg, VkDeviceMemory& outMem, VkImageView& outView) -> bool {
        if (outImg != VK_NULL_HANDLE || g_physicalDevice == VK_NULL_HANDLE) return true;
        VkImageCreateInfo ici{};
        ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
        ici.imageType = VK_IMAGE_TYPE_2D;
        ici.format = VK_FORMAT_R16G16_SFLOAT;
        ici.extent = { width, height, 1 };
        ici.mipLevels = 1;
        ici.arrayLayers = 1;
        ici.samples = VK_SAMPLE_COUNT_1_BIT;
        ici.tiling = VK_IMAGE_TILING_OPTIMAL;
        ici.usage = VK_IMAGE_USAGE_STORAGE_BIT;
        ici.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
        ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;

        if (g_pfnCreateImage(ctx->device, &ici, nullptr, &outImg) != VK_SUCCESS) return false;

        VkMemoryRequirements memReq{};
        g_pfnGetImageMemoryRequirements(ctx->device, outImg, &memReq);

        VkPhysicalDeviceMemoryProperties memProps{};
        if (g_pfnGetPhysicalDeviceMemoryProperties) {
            g_pfnGetPhysicalDeviceMemoryProperties(g_physicalDevice, &memProps);
        }

        uint32_t memType = 0;
        for (uint32_t i = 0; i < memProps.memoryTypeCount; ++i) {
            if ((memReq.memoryTypeBits & (1 << i)) &&
                (memProps.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT)) {
                memType = i;
                break;
            }
        }

        VkMemoryAllocateInfo mai{};
        mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
        mai.allocationSize = memReq.size;
        mai.memoryTypeIndex = memType;

        if (g_pfnAllocateMemory(ctx->device, &mai, nullptr, &outMem) != VK_SUCCESS) return false;
        g_pfnBindImageMemory(ctx->device, outImg, outMem, 0);

        VkImageViewCreateInfo ivci{};
        ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
        ivci.image = outImg;
        ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
        ivci.format = VK_FORMAT_R16G16_SFLOAT;
        ivci.subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        ivci.subresourceRange.baseMipLevel = 0;
        ivci.subresourceRange.levelCount = 1;
        ivci.subresourceRange.baseArrayLayer = 0;
        ivci.subresourceRange.layerCount = 1;

        return (g_pfnCreateImageView(ctx->device, &ivci, nullptr, &outView) == VK_SUCCESS);
    };

    createStorageImage(mfWidth, mfHeight, ctx->rawMotionImage, ctx->rawMotionMemory, ctx->rawMotionImageView);
    createStorageImage(mfWidth, mfHeight, ctx->filteredMotionImage, ctx->filteredMotionMemory, ctx->filteredMotionImageView);

    if (ctx->blendPipeline != VK_NULL_HANDLE && ctx->motionPipeline != VK_NULL_HANDLE && ctx->filterPipeline != VK_NULL_HANDLE) {
        return true;
    }

    // 3. Pass 1: 360-Degree Motion Estimation Pipeline
    if (ctx->motionPipeline == VK_NULL_HANDLE) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = motion_field_comp_spv_size;
        smci.pCode = motion_field_comp_spv;

        if (g_pfnCreateShaderModule(ctx->device, &smci, nullptr, &ctx->motionModule) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create motion shader module");
            return false;
        }

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
        if (g_pfnCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &ctx->motionDescLayout) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create motion descriptor layout");
            return false;
        }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(MotionPushConstants);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &ctx->motionDescLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (g_pfnCreatePipelineLayout(ctx->device, &plci, nullptr, &ctx->motionPipelineLayout) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create motion pipeline layout");
            return false;
        }

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = ctx->motionModule;
        cpci.stage.pName = "main";
        cpci.layout = ctx->motionPipelineLayout;
        if (g_pfnCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &ctx->motionPipeline) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create motion compute pipeline");
            return false;
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = static_cast<uint32_t>(RING_SIZE * 3);

        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = static_cast<uint32_t>(RING_SIZE);
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        if (g_pfnCreateDescriptorPool(ctx->device, &dpci, nullptr, &ctx->motionDescPool) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create motion descriptor pool");
            return false;
        }

        VkDescriptorSetLayout layouts[RING_SIZE];
        for (size_t i = 0; i < RING_SIZE; ++i) layouts[i] = ctx->motionDescLayout;

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = ctx->motionDescPool;
        dsai.descriptorSetCount = static_cast<uint32_t>(RING_SIZE);
        dsai.pSetLayouts = layouts;

        VkDescriptorSet sets[RING_SIZE];
        if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, sets) != VK_SUCCESS) {
            Log("Blend pipeline: failed to allocate motion descriptor sets");
            return false;
        }

        for (size_t i = 0; i < RING_SIZE; ++i) {
            ctx->slots[i].motionDescSet = sets[i];
        }
    }

    // 4. Pass 2: 3x3 Vector Median Regularization Pipeline
    if (ctx->filterPipeline == VK_NULL_HANDLE) {
#if HAVE_FLOW_FILTER
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = flow_filter_comp_spv_size;
        smci.pCode = flow_filter_comp_spv;

        if (g_pfnCreateShaderModule(ctx->device, &smci, nullptr, &ctx->filterModule) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create filter shader module");
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[2]{};
        for (int i = 0; i < 2; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 2;
        dslci.pBindings = bindings;
        if (g_pfnCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &ctx->filterDescLayout) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create filter descriptor layout");
            return false;
        }

        VkPushConstantRange pcr{};
        pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        pcr.offset = 0;
        pcr.size = sizeof(FilterPushConstants);

        VkPipelineLayoutCreateInfo plci{};
        plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
        plci.setLayoutCount = 1;
        plci.pSetLayouts = &ctx->filterDescLayout;
        plci.pushConstantRangeCount = 1;
        plci.pPushConstantRanges = &pcr;
        if (g_pfnCreatePipelineLayout(ctx->device, &plci, nullptr, &ctx->filterPipelineLayout) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create filter pipeline layout");
            return false;
        }

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = ctx->filterModule;
        cpci.stage.pName = "main";
        cpci.layout = ctx->filterPipelineLayout;
        if (g_pfnCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &ctx->filterPipeline) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create filter compute pipeline");
            return false;
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = static_cast<uint32_t>(RING_SIZE * 2);

        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = static_cast<uint32_t>(RING_SIZE);
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        if (g_pfnCreateDescriptorPool(ctx->device, &dpci, nullptr, &ctx->filterDescPool) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create filter descriptor pool");
            return false;
        }

        VkDescriptorSetLayout layouts[RING_SIZE];
        for (size_t i = 0; i < RING_SIZE; ++i) layouts[i] = ctx->filterDescLayout;

        VkDescriptorSetAllocateInfo dsai{};
        dsai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
        dsai.descriptorPool = ctx->filterDescPool;
        dsai.descriptorSetCount = static_cast<uint32_t>(RING_SIZE);
        dsai.pSetLayouts = layouts;

        VkDescriptorSet sets[RING_SIZE];
        if (g_pfnAllocateDescriptorSets(ctx->device, &dsai, sets) != VK_SUCCESS) {
            Log("Blend pipeline: failed to allocate filter descriptor sets");
            return false;
        }

        for (size_t i = 0; i < RING_SIZE; ++i) {
            ctx->slots[i].filterDescSet = sets[i];
        }
#endif
    }

    // 5. Pass 3: Dense Bilinear Warp & Soft Temporal Blend Pipeline
    if (ctx->blendPipeline == VK_NULL_HANDLE) {
        VkShaderModuleCreateInfo smci{};
        smci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
        smci.codeSize = warp_blend_comp_spv_size;
        smci.pCode = warp_blend_comp_spv;

        if (g_pfnCreateShaderModule(ctx->device, &smci, nullptr, &ctx->blendModule) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create blend shader module");
            return false;
        }

        VkDescriptorSetLayoutBinding bindings[4]{};
        for (int i = 0; i < 4; ++i) {
            bindings[i].binding = i;
            bindings[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
            bindings[i].descriptorCount = 1;
            bindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        }

        VkDescriptorSetLayoutCreateInfo dslci{};
        dslci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        dslci.bindingCount = 4;
        dslci.pBindings = bindings;
        if (g_pfnCreateDescriptorSetLayout(ctx->device, &dslci, nullptr, &ctx->blendDescLayout) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create blend descriptor layout");
            return false;
        }

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
            Log("Blend pipeline: failed to create blend pipeline layout");
            return false;
        }

        VkComputePipelineCreateInfo cpci{};
        cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
        cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
        cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
        cpci.stage.module = ctx->blendModule;
        cpci.stage.pName = "main";
        cpci.layout = ctx->blendPipelineLayout;
        if (g_pfnCreateComputePipelines(ctx->device, VK_NULL_HANDLE, 1, &cpci, nullptr, &ctx->blendPipeline) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create blend compute pipeline");
            return false;
        }

        VkDescriptorPoolSize poolSize{};
        poolSize.type = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        poolSize.descriptorCount = static_cast<uint32_t>(RING_SIZE * 4);

        VkDescriptorPoolCreateInfo dpci{};
        dpci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
        dpci.maxSets = static_cast<uint32_t>(RING_SIZE);
        dpci.poolSizeCount = 1;
        dpci.pPoolSizes = &poolSize;
        if (g_pfnCreateDescriptorPool(ctx->device, &dpci, nullptr, &ctx->blendDescPool) != VK_SUCCESS) {
            Log("Blend pipeline: failed to create blend descriptor pool");
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
            Log("Blend pipeline: failed to allocate blend descriptor sets");
            return false;
        }

        for (size_t i = 0; i < RING_SIZE; ++i) {
            ctx->slots[i].blendDescSet = sets[i];
        }
    }

    Log("Blend pipeline: successfully initialized 3-pass Hierarchical Optical Flow & Regularization pipeline!");
    return true;
#else
    Log("Blend pipeline: shaders not available");
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

    g_physicalDevice = physicalDevice;

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
    // Guarantee at least 8 swapchain images (eliminates image starvation in 3D games)
    if (modifiedCi.minImageCount < 8) {
        modifiedCi.minImageCount = 8;
    }
    modifiedCi.presentMode = VK_PRESENT_MODE_FIFO_KHR;
    modifiedCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_STORAGE_BIT;

    VkResult res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
    if (res != VK_SUCCESS) {
        Log("Swapchain creation with STORAGE_BIT failed (%d), retrying without STORAGE_BIT", res);
        modifiedCi.imageUsage = pCreateInfo->imageUsage | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
        if (modifiedCi.minImageCount < 8) modifiedCi.minImageCount = 8;
        modifiedCi.presentMode = VK_PRESENT_MODE_FIFO_KHR;
        res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
    }
    if (res != VK_SUCCESS || !pSwapchain) return res;

    auto ctx = std::make_shared<SwapchainContext>();
    ctx->swapchain = *pSwapchain;
    ctx->device = device;
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

    InitBlendPipeline(ctx.get());

    ctx->isInitialized = true;

    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        g_swapchains[*pSwapchain] = ctx;
    }

    ReloadConfig();
    Log("Swapchain created: %ux%u, format=%d, imageCount=%zu, swapchain=%p (FIFO Mode)",
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

            if (ctx->motionPipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->motionPipeline, nullptr);
            if (ctx->motionPipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->motionPipelineLayout, nullptr);
            if (ctx->motionDescPool && g_pfnDestroyDescriptorPool) g_pfnDestroyDescriptorPool(device, ctx->motionDescPool, nullptr);
            if (ctx->motionDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->motionDescLayout, nullptr);
            if (ctx->motionModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->motionModule, nullptr);

            if (ctx->filterPipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->filterPipeline, nullptr);
            if (ctx->filterPipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->filterPipelineLayout, nullptr);
            if (ctx->filterDescPool && g_pfnDestroyDescriptorPool) g_pfnDestroyDescriptorPool(device, ctx->filterDescPool, nullptr);
            if (ctx->filterDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->filterDescLayout, nullptr);
            if (ctx->filterModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->filterModule, nullptr);

            if (ctx->blendPipeline && g_pfnDestroyPipeline) g_pfnDestroyPipeline(device, ctx->blendPipeline, nullptr);
            if (ctx->blendPipelineLayout && g_pfnDestroyPipelineLayout) g_pfnDestroyPipelineLayout(device, ctx->blendPipelineLayout, nullptr);
            if (ctx->blendDescPool && g_pfnDestroyDescriptorPool) g_pfnDestroyDescriptorPool(device, ctx->blendDescPool, nullptr);
            if (ctx->blendDescLayout && g_pfnDestroyDescriptorSetLayout) g_pfnDestroyDescriptorSetLayout(device, ctx->blendDescLayout, nullptr);
            if (ctx->blendModule && g_pfnDestroyShaderModule) g_pfnDestroyShaderModule(device, ctx->blendModule, nullptr);

            if (ctx->rawMotionImageView && g_pfnDestroyImageView) g_pfnDestroyImageView(device, ctx->rawMotionImageView, nullptr);
            if (ctx->rawMotionImage && g_pfnDestroyImage) g_pfnDestroyImage(device, ctx->rawMotionImage, nullptr);
            if (ctx->rawMotionMemory && g_pfnFreeMemory) g_pfnFreeMemory(device, ctx->rawMotionMemory, nullptr);

            if (ctx->filteredMotionImageView && g_pfnDestroyImageView) g_pfnDestroyImageView(device, ctx->filteredMotionImageView, nullptr);
            if (ctx->filteredMotionImage && g_pfnDestroyImage) g_pfnDestroyImage(device, ctx->filteredMotionImage, nullptr);
            if (ctx->filteredMotionMemory && g_pfnFreeMemory) g_pfnFreeMemory(device, ctx->filteredMotionMemory, nullptr);

            for (auto iv : ctx->imageViews) {
                if (iv && g_pfnDestroyImageView) g_pfnDestroyImageView(device, iv, nullptr);
            }

            for (size_t i = 0; i < RING_SIZE; ++i) {
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

    // 1. Auto VSync Cadence: Paces base frames to display interval without stalling game rendering
    ctx->pacer.SetTargetHz(cfg.target_hz);
    ctx->pacer.PaceBasePresent();
    auto now = std::chrono::steady_clock::now();
    static uint64_t s_presentCount = 0;
    s_presentCount++;

    uint32_t currentGameIdx = pPresentInfo->pImageIndices[0];

    // Pick frame slot from ring buffer
    auto& slot = ctx->slots[ctx->ringIndex % RING_SIZE];
    ctx->ringIndex++;

    // Base frame initialization: if this is the very first frame, establish baseline directly
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

    if (acqRes != VK_SUCCESS) {
        static uint64_t s_acqFailCount = 0;
        if (s_acqFailCount++ % 60 == 0) {
            Log("WARN: Acquire intermediate image failed (%d), falling back to native present", acqRes);
        }
        std::lock_guard<std::mutex> qlock(ctx->queueMutex);
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    if (acqRes == VK_SUCCESS && intermediateIdx != currentGameIdx &&
        intermediateIdx < ctx->images.size() && currentGameIdx < ctx->images.size() &&
        slot.cmdBuffer != VK_NULL_HANDLE && g_pfnBeginCommandBuffer &&
        g_pfnCmdPipelineBarrier && g_pfnEndCommandBuffer && g_pfnQueueSubmit) {

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_pfnBeginCommandBuffer(slot.cmdBuffer, &bi);

        bool usedBlend = false;
        if (ctx->blendPipeline != VK_NULL_HANDLE && ctx->motionPipeline != VK_NULL_HANDLE &&
            ctx->filterPipeline != VK_NULL_HANDLE &&
            ctx->rawMotionImageView != VK_NULL_HANDLE && ctx->filteredMotionImageView != VK_NULL_HANDLE &&
            ctx->prevGameIdx != UINT32_MAX &&
            ctx->prevGameIdx < ctx->images.size() && ctx->prevGameIdx != intermediateIdx &&
            ctx->prevGameIdx < ctx->imageViews.size() && currentGameIdx < ctx->imageViews.size() &&
            intermediateIdx < ctx->imageViews.size() &&
            ctx->imageViews[ctx->prevGameIdx] != VK_NULL_HANDLE &&
            ctx->imageViews[currentGameIdx] != VK_NULL_HANDLE &&
            ctx->imageViews[intermediateIdx] != VK_NULL_HANDLE &&
            slot.motionDescSet != VK_NULL_HANDLE && slot.filterDescSet != VK_NULL_HANDLE &&
            slot.blendDescSet != VK_NULL_HANDLE &&
            g_pfnUpdateDescriptorSets && g_pfnCmdBindPipeline &&
            g_pfnCmdBindDescriptorSets && g_pfnCmdPushConstants && g_pfnCmdDispatch) {

            uint32_t mfWidth = (ctx->extent.width + 7) / 8;
            uint32_t mfHeight = (ctx->extent.height + 7) / 8;

            // 1. Update Pass 1 Descriptors: u_Frame0, u_Frame1, u_RawMotion
            VkDescriptorImageInfo motionImages[3]{};
            motionImages[0].imageView = ctx->imageViews[ctx->prevGameIdx];
            motionImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            motionImages[1].imageView = ctx->imageViews[currentGameIdx];
            motionImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            motionImages[2].imageView = ctx->rawMotionImageView;
            motionImages[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet motionWrites[3]{};
            for (int i = 0; i < 3; ++i) {
                motionWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                motionWrites[i].dstSet = slot.motionDescSet;
                motionWrites[i].dstBinding = i;
                motionWrites[i].descriptorCount = 1;
                motionWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                motionWrites[i].pImageInfo = &motionImages[i];
            }
            g_pfnUpdateDescriptorSets(ctx->device, 3, motionWrites, 0, nullptr);

            // 2. Update Pass 2 Descriptors: u_RawMotion, u_FilteredMotion
            VkDescriptorImageInfo filterImages[2]{};
            filterImages[0].imageView = ctx->rawMotionImageView;
            filterImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            filterImages[1].imageView = ctx->filteredMotionImageView;
            filterImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet filterWrites[2]{};
            for (int i = 0; i < 2; ++i) {
                filterWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                filterWrites[i].dstSet = slot.filterDescSet;
                filterWrites[i].dstBinding = i;
                filterWrites[i].descriptorCount = 1;
                filterWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                filterWrites[i].pImageInfo = &filterImages[i];
            }
            g_pfnUpdateDescriptorSets(ctx->device, 2, filterWrites, 0, nullptr);

            // 3. Update Pass 3 Descriptors: u_Frame0, u_Frame1, u_OutImage, u_FilteredMotion
            VkDescriptorImageInfo blendImages[4]{};
            blendImages[0].imageView = ctx->imageViews[ctx->prevGameIdx];
            blendImages[0].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            blendImages[1].imageView = ctx->imageViews[currentGameIdx];
            blendImages[1].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            blendImages[2].imageView = ctx->imageViews[intermediateIdx];
            blendImages[2].imageLayout = VK_IMAGE_LAYOUT_GENERAL;
            blendImages[3].imageView = ctx->filteredMotionImageView;
            blendImages[3].imageLayout = VK_IMAGE_LAYOUT_GENERAL;

            VkWriteDescriptorSet blendWrites[4]{};
            for (int i = 0; i < 4; ++i) {
                blendWrites[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
                blendWrites[i].dstSet = slot.blendDescSet;
                blendWrites[i].dstBinding = i;
                blendWrites[i].descriptorCount = 1;
                blendWrites[i].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
                blendWrites[i].pImageInfo = &blendImages[i];
            }
            g_pfnUpdateDescriptorSets(ctx->device, 4, blendWrites, 0, nullptr);

            // Initial Barriers for Pass 1:
            // Frame0 and Frame1 from PRESENT_SRC to GENERAL (Shader Read)
            // rawMotionImage from UNDEFINED to GENERAL (Shader Write)
            VkImageMemoryBarrier pass1Barriers[3]{};
            pass1Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass1Barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            pass1Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass1Barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            pass1Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass1Barriers[0].image = ctx->images[ctx->prevGameIdx];
            pass1Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass1Barriers[0].subresourceRange.levelCount = 1;
            pass1Barriers[0].subresourceRange.layerCount = 1;

            pass1Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass1Barriers[1].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
            pass1Barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass1Barriers[1].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            pass1Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass1Barriers[1].image = ctx->images[currentGameIdx];
            pass1Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass1Barriers[1].subresourceRange.levelCount = 1;
            pass1Barriers[1].subresourceRange.layerCount = 1;

            pass1Barriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass1Barriers[2].srcAccessMask = 0;
            pass1Barriers[2].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass1Barriers[2].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pass1Barriers[2].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass1Barriers[2].image = ctx->rawMotionImage;
            pass1Barriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass1Barriers[2].subresourceRange.levelCount = 1;
            pass1Barriers[2].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 3, pass1Barriers);

            // Dispatch Pass 1: 360-Degree Motion Field Estimation
            MotionPushConstants mpc{ static_cast<int>(ctx->extent.width), static_cast<int>(ctx->extent.height) };
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->motionPipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->motionPipelineLayout, 0, 1, &slot.motionDescSet, 0, nullptr);
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->motionPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(mpc), &mpc);
            g_pfnCmdDispatch(slot.cmdBuffer, mfWidth, mfHeight, 1);

            // Barrier between Pass 1 and Pass 2:
            // rawMotionImage: SHADER_WRITE -> SHADER_READ
            // filteredMotionImage: UNDEFINED -> GENERAL (SHADER_WRITE)
            VkImageMemoryBarrier pass2Barriers[2]{};
            pass2Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass2Barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass2Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass2Barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass2Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass2Barriers[0].image = ctx->rawMotionImage;
            pass2Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass2Barriers[0].subresourceRange.levelCount = 1;
            pass2Barriers[0].subresourceRange.layerCount = 1;

            pass2Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass2Barriers[1].srcAccessMask = 0;
            pass2Barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass2Barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pass2Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass2Barriers[1].image = ctx->filteredMotionImage;
            pass2Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass2Barriers[1].subresourceRange.levelCount = 1;
            pass2Barriers[1].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, pass2Barriers);

            // Dispatch Pass 2: 3x3 Vector Median Regularization (Eradicates 100% of outlier blocks)
            FilterPushConstants fpc{ static_cast<int>(mfWidth), static_cast<int>(mfHeight) };
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->filterPipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->filterPipelineLayout, 0, 1, &slot.filterDescSet, 0, nullptr);
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->filterPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(fpc), &fpc);
            g_pfnCmdDispatch(slot.cmdBuffer, (mfWidth + 7) / 8, (mfHeight + 7) / 8, 1);

            // Barrier between Pass 2 and Pass 3:
            // filteredMotionImage: SHADER_WRITE -> SHADER_READ
            // intermediateIdx: UNDEFINED -> GENERAL (SHADER_WRITE)
            VkImageMemoryBarrier pass3Barriers[2]{};
            pass3Barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass3Barriers[0].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass3Barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
            pass3Barriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass3Barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass3Barriers[0].image = ctx->filteredMotionImage;
            pass3Barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass3Barriers[0].subresourceRange.levelCount = 1;
            pass3Barriers[0].subresourceRange.layerCount = 1;

            pass3Barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            pass3Barriers[1].srcAccessMask = 0;
            pass3Barriers[1].dstAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            pass3Barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
            pass3Barriers[1].newLayout = VK_IMAGE_LAYOUT_GENERAL;
            pass3Barriers[1].image = ctx->images[intermediateIdx];
            pass3Barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            pass3Barriers[1].subresourceRange.levelCount = 1;
            pass3Barriers[1].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, pass3Barriers);

            // Dispatch Pass 3: Dense Bilinear Warp & Soft Temporal Blend
            BlendPushConstants pc{
                0.5f,
                static_cast<int>(ctx->extent.width),
                static_cast<int>(ctx->extent.height),
                cfg.show_hud ? 1 : 0,
                cfg.hud_protection ? 1 : 0,
                cfg.mode
            };
            g_pfnCmdBindPipeline(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->blendPipeline);
            g_pfnCmdBindDescriptorSets(slot.cmdBuffer, VK_PIPELINE_BIND_POINT_COMPUTE, ctx->blendPipelineLayout, 0, 1, &slot.blendDescSet, 0, nullptr);
            g_pfnCmdPushConstants(slot.cmdBuffer, ctx->blendPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);
            g_pfnCmdDispatch(slot.cmdBuffer, mfWidth, mfHeight, 1);

            // Final Barriers: Return all swapchain images to PRESENT_SRC
            VkImageMemoryBarrier finalBarriers[3]{};
            finalBarriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            finalBarriers[0].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            finalBarriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            finalBarriers[0].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            finalBarriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            finalBarriers[0].image = ctx->images[ctx->prevGameIdx];
            finalBarriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            finalBarriers[0].subresourceRange.levelCount = 1;
            finalBarriers[0].subresourceRange.layerCount = 1;

            finalBarriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            finalBarriers[1].srcAccessMask = VK_ACCESS_SHADER_READ_BIT;
            finalBarriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            finalBarriers[1].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            finalBarriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            finalBarriers[1].image = ctx->images[currentGameIdx];
            finalBarriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            finalBarriers[1].subresourceRange.levelCount = 1;
            finalBarriers[1].subresourceRange.layerCount = 1;

            finalBarriers[2].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
            finalBarriers[2].srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT;
            finalBarriers[2].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
            finalBarriers[2].oldLayout = VK_IMAGE_LAYOUT_GENERAL;
            finalBarriers[2].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
            finalBarriers[2].image = ctx->images[intermediateIdx];
            finalBarriers[2].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
            finalBarriers[2].subresourceRange.levelCount = 1;
            finalBarriers[2].subresourceRange.layerCount = 1;

            g_pfnCmdPipelineBarrier(slot.cmdBuffer, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 0, nullptr, 3, finalBarriers);

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

        // A. Present Intermediate Frame (F_{N-0.5}) directly to Gamescope FIFO
        VkPresentInfoKHR interPresent = *pPresentInfo;
        interPresent.pNext = nullptr;
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

        // B. Present Real Game Frame (F_N) directly to Gamescope FIFO
        VkPresentInfoKHR gamePresent = *pPresentInfo;
        gamePresent.waitSemaphoreCount = 1;
        gamePresent.pWaitSemaphores = &slot.finalDoneSemaphore;
        gamePresent.swapchainCount = 1;
        gamePresent.pSwapchains = &ctx->swapchain;
        gamePresent.pImageIndices = &currentGameIdx;

        {
            std::lock_guard<std::mutex> qlock(ctx->queueMutex);
            res = g_pfnQueuePresentKHR(queue, &gamePresent);
        }

        ctx->prevGameIdx = currentGameIdx;

        if (s_presentCount % 120 == 1) {
            Log("FrameGen ACTIVE: 2x presents FIFO GPU paced! Base: %.1f FPS -> Output: %.1f FPS (Display: %d Hz | %s)",
                ctx->pacer.GetBaseFps(), ctx->pacer.GetOutputFps(),
                ctx->pacer.GetTargetHz(),
                usedBlend ? "3-Pass Hierarchical Flow + Median" : "Fallback Copy");
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
