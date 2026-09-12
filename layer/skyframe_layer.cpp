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

void ReloadConfig() {
    std::lock_guard<std::mutex> lock(g_configMutex);
    const char* home = getenv("HOME");
    std::string configPath = (home && strlen(home) > 0) ? (std::string(home) + "/.config/skyframe/config.json") : "/home/deck/.config/skyframe/config.json";

    std::ifstream file(configPath);
    if (!file.is_open()) {
        if (configPath != "/home/deck/.config/skyframe/config.json") {
            file.open("/home/deck/.config/skyframe/config.json");
        }
    }

    if (file.is_open()) {
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

    // Explicit environment variable overrides
    const char* envEnable = getenv("ENABLE_SKYFRAME");
    if (envEnable && (strcmp(envEnable, "1") == 0 || strcmp(envEnable, "true") == 0)) {
        g_config.enabled = true;
    }
    const char* envDisable = getenv("DISABLE_SKYFRAME");
    if (envDisable && (strcmp(envDisable, "1") == 0 || strcmp(envDisable, "true") == 0)) {
        g_config.enabled = false;
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
PFN_vkCreateSemaphore        g_pfnCreateSemaphore = nullptr;
PFN_vkDestroySemaphore       g_pfnDestroySemaphore = nullptr;
PFN_vkQueueSubmit            g_pfnQueueSubmit = nullptr;

struct SwapchainContext {
    VkSwapchainKHR swapchain = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    VkExtent2D extent{0, 0};
    std::vector<VkImage> images;

    VkCommandPool cmdPool = VK_NULL_HANDLE;
    VkCommandBuffer cmdBuffer = VK_NULL_HANDLE;
    VkSemaphore acqSemaphore = VK_NULL_HANDLE;
    VkSemaphore renderDoneSemaphore = VK_NULL_HANDLE;

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
    g_pfnCreateSemaphore = (PFN_vkCreateSemaphore)g_nextGetDeviceProcAddr(*pDevice, "vkCreateSemaphore");
    g_pfnDestroySemaphore = (PFN_vkDestroySemaphore)g_nextGetDeviceProcAddr(*pDevice, "vkDestroySemaphore");
    g_pfnQueueSubmit = (PFN_vkQueueSubmit)g_nextGetDeviceProcAddr(*pDevice, "vkQueueSubmit");

    // Fallbacks if driver getProcAddr returned null
    if (!g_pfnCreateCommandPool) g_pfnCreateCommandPool = &vkCreateCommandPool;
    if (!g_pfnDestroyCommandPool) g_pfnDestroyCommandPool = &vkDestroyCommandPool;
    if (!g_pfnAllocateCommandBuffers) g_pfnAllocateCommandBuffers = &vkAllocateCommandBuffers;
    if (!g_pfnBeginCommandBuffer) g_pfnBeginCommandBuffer = &vkBeginCommandBuffer;
    if (!g_pfnEndCommandBuffer) g_pfnEndCommandBuffer = &vkEndCommandBuffer;
    if (!g_pfnCmdPipelineBarrier) g_pfnCmdPipelineBarrier = &vkCmdPipelineBarrier;
    if (!g_pfnCmdCopyImage) g_pfnCmdCopyImage = &vkCmdCopyImage;
    if (!g_pfnCreateSemaphore) g_pfnCreateSemaphore = &vkCreateSemaphore;
    if (!g_pfnDestroySemaphore) g_pfnDestroySemaphore = &vkDestroySemaphore;
    if (!g_pfnQueueSubmit) g_pfnQueueSubmit = &vkQueueSubmit;

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
    modifiedCi.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;

    VkResult res = g_pfnCreateSwapchainKHR(device, &modifiedCi, pAllocator, pSwapchain);
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
            VkCommandBufferAllocateInfo cbai{};
            cbai.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO;
            cbai.commandPool = ctx->cmdPool;
            cbai.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
            cbai.commandBufferCount = 1;
            g_pfnAllocateCommandBuffers(device, &cbai, &ctx->cmdBuffer);
        }
    }

    if (g_pfnCreateSemaphore) {
        VkSemaphoreCreateInfo sci{};
        sci.sType = VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO;
        g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->acqSemaphore);
        g_pfnCreateSemaphore(device, &sci, nullptr, &ctx->renderDoneSemaphore);
    }

    ctx->isInitialized = true;

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
            if (ctx->cmdPool && g_pfnDestroyCommandPool) {
                g_pfnDestroyCommandPool(device, ctx->cmdPool, nullptr);
            }
            if (ctx->acqSemaphore && g_pfnDestroySemaphore) {
                g_pfnDestroySemaphore(device, ctx->acqSemaphore, nullptr);
            }
            if (ctx->renderDoneSemaphore && g_pfnDestroySemaphore) {
                g_pfnDestroySemaphore(device, ctx->renderDoneSemaphore, nullptr);
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

    if (!ctx || !ctx->isInitialized || ctx->images.empty()) {
        return g_pfnQueuePresentKHR(queue, pPresentInfo);
    }

    // 1. Update frame pacer
    ctx->pacer.OnGamePresent();
    static uint64_t s_presentCount = 0;
    s_presentCount++;

    uint32_t currentGameIdx = pPresentInfo->pImageIndices[0];

    // 2. FastWarp 2x Frame Generation: Acquire next image and present intermediate frame
    uint32_t intermediateIdx = 0;
    VkResult acqRes = VK_NOT_READY;
    if (g_pfnAcquireNextImageKHR && ctx->acqSemaphore) {
        acqRes = g_pfnAcquireNextImageKHR(
            ctx->device, ctx->swapchain, 0, ctx->acqSemaphore, VK_NULL_HANDLE, &intermediateIdx
        );
    }

    if (acqRes == VK_SUCCESS && intermediateIdx != currentGameIdx &&
        intermediateIdx < ctx->images.size() && currentGameIdx < ctx->images.size() &&
        ctx->cmdBuffer != VK_NULL_HANDLE && g_pfnBeginCommandBuffer && g_pfnCmdCopyImage &&
        g_pfnCmdPipelineBarrier && g_pfnEndCommandBuffer && g_pfnQueueSubmit) {

        VkCommandBufferBeginInfo bi{};
        bi.sType = VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO;
        bi.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        g_pfnBeginCommandBuffer(ctx->cmdBuffer, &bi);

        VkImageMemoryBarrier barriers[2]{};
        // Source image (game frame)
        barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[0].srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].image = ctx->images[currentGameIdx];
        barriers[0].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[0].subresourceRange.levelCount = 1;
        barriers[0].subresourceRange.layerCount = 1;

        // Destination image (intermediate frame)
        barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
        barriers[1].srcAccessMask = 0;
        barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].image = ctx->images[intermediateIdx];
        barriers[1].subresourceRange.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        barriers[1].subresourceRange.levelCount = 1;
        barriers[1].subresourceRange.layerCount = 1;

        g_pfnCmdPipelineBarrier(
            ctx->cmdBuffer,
            VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers
        );

        VkImageCopy copyRegion{};
        copyRegion.srcSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.srcSubresource.layerCount = 1;
        copyRegion.dstSubresource.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
        copyRegion.dstSubresource.layerCount = 1;
        copyRegion.extent.width = ctx->extent.width;
        copyRegion.extent.height = ctx->extent.height;
        copyRegion.extent.depth = 1;

        g_pfnCmdCopyImage(
            ctx->cmdBuffer,
            ctx->images[currentGameIdx], VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
            ctx->images[intermediateIdx], VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
            1, &copyRegion
        );

        barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
        barriers[0].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
        barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
        barriers[1].dstAccessMask = VK_ACCESS_MEMORY_READ_BIT;
        barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
        barriers[1].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

        g_pfnCmdPipelineBarrier(
            ctx->cmdBuffer,
            VK_PIPELINE_STAGE_TRANSFER_BIT,
            VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
            0, 0, nullptr, 0, nullptr, 2, barriers
        );

        g_pfnEndCommandBuffer(ctx->cmdBuffer);

        VkPipelineStageFlags waitStages[2] = {
            VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
            VK_PIPELINE_STAGE_TRANSFER_BIT
        };
        std::vector<VkSemaphore> waitSems;
        for (uint32_t i = 0; i < pPresentInfo->waitSemaphoreCount; ++i) {
            waitSems.push_back(pPresentInfo->pWaitSemaphores[i]);
        }
        waitSems.push_back(ctx->acqSemaphore);

        VkSubmitInfo si{};
        si.sType = VK_STRUCTURE_TYPE_SUBMIT_INFO;
        si.waitSemaphoreCount = static_cast<uint32_t>(waitSems.size());
        si.pWaitSemaphores = waitSems.data();
        si.pWaitDstStageMask = waitStages;
        si.commandBufferCount = 1;
        si.pCommandBuffers = &ctx->cmdBuffer;
        si.signalSemaphoreCount = 1;
        si.pSignalSemaphores = &ctx->renderDoneSemaphore;

        g_pfnQueueSubmit(queue, 1, &si, VK_NULL_HANDLE);

        // Present intermediate frame!
        VkPresentInfoKHR interPresent{};
        interPresent.sType = VK_STRUCTURE_TYPE_PRESENT_INFO_KHR;
        interPresent.waitSemaphoreCount = 1;
        interPresent.pWaitSemaphores = &ctx->renderDoneSemaphore;
        interPresent.swapchainCount = 1;
        interPresent.pSwapchains = &ctx->swapchain;
        interPresent.pImageIndices = &intermediateIdx;

        g_pfnQueuePresentKHR(queue, &interPresent);

        if (s_presentCount % 120 == 1) {
            Log("FrameGen ACTIVE: 2x presents sent! Base: %.1f FPS -> Output: %.1f FPS",
                ctx->pacer.GetBaseFps(), ctx->pacer.GetOutputFps());
        }
    }

    // 3. Present original game frame
    VkResult res = g_pfnQueuePresentKHR(queue, pPresentInfo);
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
