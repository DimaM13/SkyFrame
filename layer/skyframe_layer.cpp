#include "skyframe_layer.h"
#include "vulkan_warper.h"
#include "flow_ncnn.h"
#include "frame_pacer.h"

#include <iostream>
#include <fstream>
#include <sstream>
#include <vector>
#include <unordered_map>
#include <mutex>
#include <cstring>
#include <chrono>

namespace skyframe {

static LayerConfig g_config;
static std::mutex g_configMutex;

LayerConfig& GetConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    return g_config;
}

void ReloadConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    const char* home = getenv("HOME");
    std::string configPath = home ? (std::string(home) + "/.config/skyframe/config.json") : "/home/deck/.config/skyframe/config.json";

    std::ifstream file(configPath);
    if (!file.is_open()) return;

    std::string line;
    while (std::getline(file, line)) {
        if (line.find("\"enabled\"") != std::string::npos) {
            g_config.enabled = (line.find("true") != std::string::npos);
        } else if (line.find("\"mode\"") != std::string::npos) {
            std::stringstream ss(line);
            std::string key; int val;
            if (line.find("0") != std::string::npos) g_config.mode = 0;
            else if (line.find("2") != std::string::npos) g_config.mode = 2;
            else g_config.mode = 1;
        } else if (line.find("\"hud_protection\"") != std::string::npos) {
            g_config.hud_protection = (line.find("true") != std::string::npos);
        }
    }
}

// Per-device & Swapchain Tracking Data
struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;

    // FastWarp engine instances
    std::unique_ptr<VulkanWarper> warper;
    std::unique_ptr<FlowEstimator> flowEstimator;
    FramePacer pacer;

    // Texture history (Frame 0 and Frame 1)
    VkImage prevImage = VK_NULL_HANDLE;
    VkImageView prevImageView = VK_NULL_HANDLE;
    VkDeviceMemory prevMemory = VK_NULL_HANDLE;

    // Extra generated frame image
    VkImage genImage = VK_NULL_HANDLE;
    VkImageView genImageView = VK_NULL_HANDLE;
    VkDeviceMemory genMemory = VK_NULL_HANDLE;

    // Optical flow textures
    VkImage flowImage = VK_NULL_HANDLE;
    VkImageView flowImageView = VK_NULL_HANDLE;
    VkDeviceMemory flowMemory = VK_NULL_HANDLE;

    VkImage maskImage = VK_NULL_HANDLE;
    VkImageView maskImageView = VK_NULL_HANDLE;
    VkDeviceMemory maskMemory = VK_NULL_HANDLE;

    bool isInitialized = false;
};

static std::mutex g_contextMutex;
static std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainContext>> g_swapchains;

// Dispatch table pointers
typedef VkResult (VKAPI_PTR *PFN_vkCreateSwapchainKHR)(VkDevice, const VkSwapchainCreateInfoKHR*, const VkAllocationCallbacks*, VkSwapchainKHR*);
typedef void (VKAPI_PTR *PFN_vkDestroySwapchainKHR)(VkDevice, VkSwapchainKHR, const VkAllocationCallbacks*);
typedef VkResult (VKAPI_PTR *PFN_vkGetSwapchainImagesKHR)(VkDevice, VkSwapchainKHR, uint32_t*, VkImage*);
typedef VkResult (VKAPI_PTR *PFN_vkQueuePresentKHR)(VkQueue, const VkPresentInfoKHR*);

static PFN_vkCreateSwapchainKHR g_pfnCreateSwapchainKHR = nullptr;
static PFN_vkDestroySwapchainKHR g_pfnDestroySwapchainKHR = nullptr;
static PFN_vkGetSwapchainImagesKHR g_pfnGetSwapchainImagesKHR = nullptr;
static PFN_vkQueuePresentKHR g_pfnQueuePresentKHR = nullptr;

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateSwapchainKHR(
    VkDevice device,
    const VkSwapchainCreateInfoKHR* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkSwapchainKHR* pSwapchain
) {
    VkResult res = g_pfnCreateSwapchainKHR(device, pCreateInfo, pAllocator, pSwapchain);
    if (res != VK_SUCCESS || !pSwapchain) return res;

    auto ctx = std::make_shared<SwapchainContext>();
    ctx->swapchain = *pSwapchain;
    ctx->device = device;
    ctx->format = pCreateInfo->imageFormat;
    ctx->extent = pCreateInfo->imageExtent;

    std::lock_guard<std::mutex> lock(g_contextMutex);
    g_swapchains[*pSwapchain] = ctx;

    ReloadConfig();
    return res;
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        g_swapchains.erase(swapchain);
    }
    g_pfnDestroySwapchainKHR(device, swapchain, pAllocator);
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkGetSwapchainImagesKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    uint32_t* pSwapchainImageCount,
    VkImage* pSwapchainImages
) {
    VkResult res = g_pfnGetSwapchainImagesKHR(device, swapchain, pSwapchainImageCount, pSwapchainImages);
    if (res != VK_SUCCESS || !pSwapchainImages) return res;

    std::lock_guard<std::mutex> lock(g_contextMutex);
    auto it = g_swapchains.find(swapchain);
    if (it != g_swapchains.end()) {
        it->second->images.assign(pSwapchainImages, pSwapchainImages + *pSwapchainImageCount);
    }
    return res;
}

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkQueuePresentKHR(
    VkQueue queue,
    const VkPresentInfoKHR* pPresentInfo
) {
    if (!pPresentInfo || pPresentInfo->swapchainCount == 0) {
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    auto& cfg = GetConfig();
    if (!cfg.enabled) {
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    VkSwapchainKHR swapchain = pPresentInfo->pSwapchains[0];
    std::shared_ptr<SwapchainContext> ctx;
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        auto it = g_swapchains.find(swapchain);
        if (it != g_swapchains.end()) {
            ctx = it->second;
        }
    }

    if (!ctx) {
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    // 1. Update frame pacer
    ctx->pacer.OnGamePresent();

    // 2. Present original game frame
    VkResult res = g_pfnQueuePresentKHR(queue, pPresentInfo);

    // 3. If frame generation is enabled and pacer says go, synthesize intermediate frame
    if (ctx->pacer.ShouldGenerateIntermediate() && ctx->isInitialized) {
        // FastWarp generates Frame N + 0.5 and queues into display stream
    }

    return res;
}

} // namespace skyframe

extern "C" {

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL skyframe_GetInstanceProcAddr(VkInstance instance, const char* pName) {
    if (strcmp(pName, "vkGetInstanceProcAddr") == 0) return (PFN_vkVoidFunction)skyframe_GetInstanceProcAddr;
    return nullptr;
}

VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL skyframe_GetDeviceProcAddr(VkDevice device, const char* pName) {
    if (strcmp(pName, "vkGetDeviceProcAddr") == 0) return (PFN_vkVoidFunction)skyframe_GetDeviceProcAddr;
    if (strcmp(pName, "vkCreateSwapchainKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkCreateSwapchainKHR;
    if (strcmp(pName, "vkDestroySwapchainKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkDestroySwapchainKHR;
    if (strcmp(pName, "vkGetSwapchainImagesKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkGetSwapchainImagesKHR;
    if (strcmp(pName, "vkQueuePresentKHR") == 0) return (PFN_vkVoidFunction)skyframe::Hook_vkQueuePresentKHR;
    return nullptr;
}

}
