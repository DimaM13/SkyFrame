#ifndef VK_LAYER_EXPORT
#if defined(_WIN32)
#define VK_LAYER_EXPORT __declspec(dllexport)
#else
#define VK_LAYER_EXPORT __attribute__((visibility("default")))
#endif
#endif
#pragma once

#include <vulkan/vulkan.h>
#include <vulkan/vk_layer.h>
#include <mutex>
#include <unordered_map>
#include <memory>
#include <string>

namespace skyframe {

struct LayerConfig {
    bool enabled = true;
    int mode = 1;               // 0 = Lite (180p), 1 = Balanced (240p), 2 = Quality (360p)
    int multiplier = 2;         // 2x frame generation
    bool hud_protection = true; // Protect static HUD
    float hud_threshold = 0.08f;
    int target_fps = 0;         // 0 = unconstrained / match refresh rate
    bool show_hud = false;      // Visual debug indicator on generated frames
};

// Global config singleton loaded from /home/deck/.config/skyframe/config.json
LayerConfig& GetConfig();
void ReloadConfig();

} // namespace skyframe

// Exported Vulkan Layer entry points
extern "C" {
    VK_LAYER_EXPORT VKAPI_ATTR VkResult VKAPI_CALL vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface *pVersionStruct);
    VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL skyframe_GetInstanceProcAddr(VkInstance instance, const char* pName);
    VK_LAYER_EXPORT VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL skyframe_GetDeviceProcAddr(VkDevice device, const char* pName);
}
