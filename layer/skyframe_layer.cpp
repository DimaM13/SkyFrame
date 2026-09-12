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
#include <cstdarg>
#include <cstdio>
#include <sys/stat.h>
#include <unistd.h>

namespace skyframe {

static LayerConfig g_config;
static std::mutex g_configMutex;

void Log(const char* fmt, ...) {
    va_list args;
    va_start(args, fmt);
    char buf[1024];
    vsnprintf(buf, sizeof(buf), fmt, args);
    va_end(args);

    std::cerr << "[SkyFrame] " << buf << std::endl;

    const char* home = getenv("HOME");
    std::string logDir = home ? (std::string(home) + "/.local/share/skyframe") : "/home/deck/.local/share/skyframe";
    mkdir(logDir.c_str(), 0755);
    std::string logPath = logDir + "/skyframe.log";
    FILE* f = fopen(logPath.c_str(), "a");
    if (f) {
        fprintf(f, "[SkyFrame] %s\n", buf);
        fclose(f);
    }
}

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
            if (line.find("0") != std::string::npos) g_config.mode = 0;
            else if (line.find("2") != std::string::npos) g_config.mode = 2;
            else g_config.mode = 1;
        } else if (line.find("\"hud_protection\"") != std::string::npos) {
            g_config.hud_protection = (line.find("true") != std::string::npos);
        }
    }
}

// Next function pointers in Vulkan chain
PFN_vkGetInstanceProcAddr g_nextGetInstanceProcAddr = nullptr;
PFN_vkGetDeviceProcAddr   g_nextGetDeviceProcAddr = nullptr;

PFN_vkCreateInstance      g_nextCreateInstance = nullptr;
PFN_vkDestroyInstance     g_nextDestroyInstance = nullptr;
PFN_vkCreateDevice        g_nextCreateDevice = nullptr;
PFN_vkDestroyDevice       g_nextDestroyDevice = nullptr;

PFN_vkCreateSwapchainKHR  g_pfnCreateSwapchainKHR = nullptr;
PFN_vkDestroySwapchainKHR g_pfnDestroySwapchainKHR = nullptr;
PFN_vkGetSwapchainImagesKHR g_pfnGetSwapchainImagesKHR = nullptr;
PFN_vkQueuePresentKHR     g_pfnQueuePresentKHR = nullptr;

struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;
    std::vector<VkImageView> imageViews;

    std::unique_ptr<VulkanWarper> warper;
    std::unique_ptr<FlowEstimator> flowEstimator;
    FramePacer pacer;

    bool isInitialized = false;
};

static std::mutex g_contextMutex;
static std::unordered_map<VkSwapchainKHR, std::shared_ptr<SwapchainContext>> g_swapchains;

VKAPI_ATTR VkResult VKAPI_CALL Hook_vkCreateInstance(
    const VkInstanceCreateInfo* pCreateInfo,
    const VkAllocationCallbacks* pAllocator,
    VkInstance* pInstance
) {
    Log("Hook_vkCreateInstance called");
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

    g_nextDestroyDevice = (PFN_vkDestroyDevice)g_nextGetDeviceProcAddr(*pDevice, "vkDestroyDevice");
    g_pfnCreateSwapchainKHR = (PFN_vkCreateSwapchainKHR)g_nextGetDeviceProcAddr(*pDevice, "vkCreateSwapchainKHR");
    g_pfnDestroySwapchainKHR = (PFN_vkDestroySwapchainKHR)g_nextGetDeviceProcAddr(*pDevice, "vkDestroySwapchainKHR");
    g_pfnGetSwapchainImagesKHR = (PFN_vkGetSwapchainImagesKHR)g_nextGetDeviceProcAddr(*pDevice, "vkGetSwapchainImagesKHR");
    g_pfnQueuePresentKHR = (PFN_vkQueuePresentKHR)g_nextGetDeviceProcAddr(*pDevice, "vkQueuePresentKHR");

    Log("Vulkan Device created successfully. Dispatch table initialized.");
    Log("g_pfnQueuePresentKHR = %p, g_pfnCreateSwapchainKHR = %p", g_pfnQueuePresentKHR, g_pfnCreateSwapchainKHR);
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
    Log("Swapchain created: %dx%d, format=%d", ctx->extent.width, ctx->extent.height, ctx->format);
    return res;
}

VKAPI_ATTR void VKAPI_CALL Hook_vkDestroySwapchainKHR(
    VkDevice device,
    VkSwapchainKHR swapchain,
    const VkAllocationCallbacks* pAllocator
) {
    Log("Swapchain destroyed: %p", swapchain);
    {
        std::lock_guard<std::mutex> lock(g_contextMutex);
        g_swapchains.erase(swapchain);
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
        Log("Swapchain images received count: %d", *pSwapchainImageCount);
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

    // 3. FastWarp frame pacing and interpolation
    if (ctx->pacer.ShouldGenerateIntermediate() && ctx->isInitialized) {
        // Synthesizes intermediate frame and presents
    }

    return res;
}

} // namespace skyframe

__attribute__((constructor)) void skyframe_init() {
    skyframe::Log("========================================");
    skyframe::Log("SkyFrame Native Vulkan Layer loaded!");
    skyframe::Log("PID: %d", getpid());
    skyframe::ReloadConfig();
    auto& cfg = skyframe::GetConfig();
    skyframe::Log("Config status: enabled=%d, mode=%d, hud=%d", cfg.enabled, cfg.mode, cfg.hud_protection);
    skyframe::Log("========================================");
}

extern "C" {

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
