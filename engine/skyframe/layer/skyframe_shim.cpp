/* SPDX-License-Identifier: GPL-3.0-or-later */
/* SkyFrame present tap — own Vulkan layer.
 *
 * Observes and NEVER modifies: every intercepted call is forwarded
 * unchanged to the next layer/driver. Measures present timestamps per
 * swapchain and publishes /tmp/skyframe_stats.json for the Decky HUD
 * (live output rate + interval jitter). All logic is exception-safe;
 * on any internal failure the game call still goes through.
 */

#include <vulkan/vk_layer.h>
#include <vulkan/vulkan_core.h>

#include <chrono>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <string>
#include <unordered_map>
#include <utility>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct DevFuncs {
    PFN_vkGetDeviceProcAddr nextGetDevice = nullptr;
    PFN_vkCreateSwapchainKHR createSwapchain = nullptr;
    PFN_vkDestroySwapchainKHR destroySwapchain = nullptr;
    PFN_vkQueuePresentKHR queuePresent = nullptr;
};

struct SwapStats {
    uint64_t presents = 0;
    double lastMs = 0.0;
    double sumInterval = 0.0;
    double maxInterval = 0.0;
    uint64_t measured = 0; // intervals accumulated since last flush
    double lastFlushMs = 0.0;
};

struct Globals {
    std::mutex mtx;
    PFN_vkGetInstanceProcAddr nextGetInstance = nullptr;
    PFN_vkCreateInstance nextCreateInstance = nullptr;
    PFN_vkDestroyInstance nextDestroyInstance = nullptr;
    std::unordered_map<VkDevice, DevFuncs> devices;
    std::unordered_map<VkSwapchainKHR, VkDevice> swapDev;
    std::unordered_map<VkSwapchainKHR, SwapStats> stats;
};

Globals& G() {
    static Globals g;
    return g;
}

double nowMs() {
    return std::chrono::duration<double, std::milli>(Clock::now().time_since_epoch()).count();
}

void flushStatsLocked() {
    // Aggregate across swapchains; games normally drive one.
    uint64_t presents = 0;
    double sum = 0.0, mx = 0.0, measured = 0;
    for (const auto& [sc, s] : G().stats) {
        (void)sc;
        presents += s.presents;
        sum += s.sumInterval;
        measured += s.measured;
        if (s.maxInterval > mx) mx = s.maxInterval;
    }
    if (measured < 2) return;
    const double avg = sum / (double)measured;
    const double fps = avg > 0.0 ? 1000.0 / avg : 0.0;

    char buf[512];
    std::snprintf(buf, sizeof(buf),
        "{\"output_fps\":%.1f,\"avg_interval_ms\":%.2f,\"jitter_ms\":%.2f,"
        "\"presents\":%llu,\"live\":true,\"source\":\"skyframe-shim\"}",
        fps, avg, mx - avg,
        (unsigned long long)presents);

    const char* tmp = "/tmp/skyframe_stats.json.tmp";
    const char* dst = "/tmp/skyframe_stats.json";
    if (FILE* f = std::fopen(tmp, "w")) {
        std::fputs(buf, f);
        std::fclose(f);
        std::rename(tmp, dst);
    }
    for (auto& [sc, s] : G().stats) {
        (void)sc;
        s.sumInterval = 0.0;
        s.maxInterval = 0.0;
        s.measured = 0;
    }
}

void recordPresent(VkSwapchainKHR swapchain) {
    const double t = nowMs();
    std::lock_guard<std::mutex> lock(G().mtx);
    SwapStats& s = G().stats[swapchain];
    s.presents++;
    if (s.lastMs > 0.0) {
        const double dt = t - s.lastMs;
        if (dt > 0.0 && dt < 5000.0) {
            s.sumInterval += dt;
            s.measured++;
            if (dt > s.maxInterval) s.maxInterval = dt;
        }
    }
    s.lastMs = t;
    if (t - s.lastFlushMs > 500.0) {
        s.lastFlushMs = t;
        flushStatsLocked();
    }
}

DevFuncs devFuncsFor(VkDevice device) {
    std::lock_guard<std::mutex> lock(G().mtx);
    const auto it = G().devices.find(device);
    if (it != G().devices.end()) return it->second;
    return {};
}

// ---- intercepted entry points (forward-only) ----

VkResult myvkCreateInstance(const VkInstanceCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkInstance* instance) {
    auto* layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(info->pNext));
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_INSTANCE_CREATE_INFO ||
            layerInfo->function != VK_LAYER_LINK_INFO))
        layerInfo = reinterpret_cast<VkLayerInstanceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
    if (!layerInfo || !layerInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    auto* link = layerInfo->u.pLayerInfo;
    auto nextGet = link->pfnNextGetInstanceProcAddr;
    if (!nextGet) return VK_ERROR_INITIALIZATION_FAILED;
    layerInfo->u.pLayerInfo = link->pNext;

    auto nextCreate = reinterpret_cast<PFN_vkCreateInstance>(nextGet(VK_NULL_HANDLE, "vkCreateInstance"));
    if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;
    {
        std::lock_guard<std::mutex> lock(G().mtx);
        G().nextGetInstance = nextGet;
        G().nextCreateInstance = nextCreate;
        G().nextDestroyInstance = reinterpret_cast<PFN_vkDestroyInstance>(
            nextGet(VK_NULL_HANDLE, "vkDestroyInstance"));
    }
    VkInstanceCreateInfo fwd = *info;
    return nextCreate(&fwd, alloc, instance);
}

void myvkDestroyInstance(VkInstance instance, const VkAllocationCallbacks* alloc) {
    PFN_vkDestroyInstance f = nullptr;
    { std::lock_guard<std::mutex> lock(G().mtx); f = G().nextDestroyInstance; }
    if (f) f(instance, alloc);
}

VkResult myvkCreateDevice(VkPhysicalDevice phys, const VkDeviceCreateInfo* info,
        const VkAllocationCallbacks* alloc, VkDevice* device) {
    auto* layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(info->pNext));
    while (layerInfo && (layerInfo->sType != VK_STRUCTURE_TYPE_LOADER_DEVICE_CREATE_INFO ||
            layerInfo->function != VK_LAYER_LINK_INFO))
        layerInfo = reinterpret_cast<VkLayerDeviceCreateInfo*>(const_cast<void*>(layerInfo->pNext));
    if (!layerInfo || !layerInfo->u.pLayerInfo) return VK_ERROR_INITIALIZATION_FAILED;

    auto* link = layerInfo->u.pLayerInfo;
    auto nextGet = link->pfnNextGetDeviceProcAddr;
    if (!nextGet) return VK_ERROR_INITIALIZATION_FAILED;
    layerInfo->u.pLayerInfo = link->pNext;

    // Find loader callback + instance dispatch for CreateDevice.
    PFN_vkGetInstanceProcAddr instGet = nullptr;
    { std::lock_guard<std::mutex> lock(G().mtx); instGet = G().nextGetInstance; }
    if (!instGet) return VK_ERROR_INITIALIZATION_FAILED;
    auto nextCreate = reinterpret_cast<PFN_vkCreateDevice>(instGet(VK_NULL_HANDLE, "vkCreateDevice"));
    if (!nextCreate) return VK_ERROR_INITIALIZATION_FAILED;

    VkDeviceCreateInfo fwd = *info;
    VkResult res = nextCreate(phys, &fwd, alloc, device);
    if (res != VK_SUCCESS) return res;

    DevFuncs df;
    df.nextGetDevice = nextGet;
    df.createSwapchain = reinterpret_cast<PFN_vkCreateSwapchainKHR>(nextGet(*device, "vkCreateSwapchainKHR"));
    df.destroySwapchain = reinterpret_cast<PFN_vkDestroySwapchainKHR>(nextGet(*device, "vkDestroySwapchainKHR"));
    df.queuePresent = reinterpret_cast<PFN_vkQueuePresentKHR>(nextGet(*device, "vkQueuePresentKHR"));
    std::lock_guard<std::mutex> lock(G().mtx);
    G().devices[*device] = df;
    return VK_SUCCESS;
}

void myvkDestroyDevice(VkDevice device, const VkAllocationCallbacks* alloc) {
    DevFuncs df = devFuncsFor(device);
    {
        std::lock_guard<std::mutex> lock(G().mtx);
        G().devices.erase(device);
    }
    auto f = reinterpret_cast<PFN_vkDestroyDevice>(
        df.nextGetDevice ? df.nextGetDevice(device, "vkDestroyDevice") : nullptr);
    if (f) f(device, alloc);
}

VkResult myvkCreateSwapchainKHR(VkDevice device, const VkSwapchainCreateInfoKHR* info,
        const VkAllocationCallbacks* alloc, VkSwapchainKHR* swapchain) {
    DevFuncs df = devFuncsFor(device);
    if (!df.createSwapchain) return VK_ERROR_INITIALIZATION_FAILED;
    VkSwapchainCreateInfoKHR fwd = *info; // never modified
    VkResult res = df.createSwapchain(device, &fwd, alloc, swapchain);
    if (res == VK_SUCCESS) {
        try {
            std::lock_guard<std::mutex> lock(G().mtx);
            G().swapDev[*swapchain] = device;
            G().stats[*swapchain] = SwapStats{};
        } catch (...) {} // tracking must never break the game
    }
    return res;
}

void myvkDestroySwapchainKHR(VkDevice device, VkSwapchainKHR swapchain,
        const VkAllocationCallbacks* alloc) {
    DevFuncs df = devFuncsFor(device);
    {
        std::lock_guard<std::mutex> lock(G().mtx);
        G().swapDev.erase(swapchain);
        G().stats.erase(swapchain);
    }
    if (df.destroySwapchain) df.destroySwapchain(device, swapchain, alloc);
}

VkResult myvkQueuePresentKHR(VkQueue queue, const VkPresentInfoKHR* info) {
    // Present is queue-level; resolve device funcs via tracked swapchains.
    // Fallback to any cached device (untracked handles must still present).
    DevFuncs df{};
    {
        std::lock_guard<std::mutex> lock(G().mtx);
        for (uint32_t i = 0; i < info->swapchainCount; ++i) {
            auto it = G().swapDev.find(info->pSwapchains[i]);
            if (it != G().swapDev.end()) {
                auto dit = G().devices.find(it->second);
                if (dit != G().devices.end()) { df = dit->second; break; }
            }
        }
        if (!df.queuePresent && !G().devices.empty())
            df = G().devices.begin()->second;
    }
    VkResult res = VK_SUCCESS;
    if (df.queuePresent) {
        // Forward the caller's struct untouched.
        res = df.queuePresent(queue, info);
    } else {
        // No dispatch known (should be unreachable): fail loudly instead
        // of silently swallowing the game's frame.
        return VK_ERROR_INITIALIZATION_FAILED;
    }
    if (res == VK_SUCCESS || res == VK_SUBOPTIMAL_KHR) {
        for (uint32_t i = 0; i < info->swapchainCount; ++i) {
            try { recordPresent(info->pSwapchains[i]); } catch (...) {}
        }
    }
    return res;
}

PFN_vkVoidFunction lookup(const char* name) {
    if (!name) return nullptr;
    const std::string n(name);
    if (n == "vkCreateInstance") return reinterpret_cast<PFN_vkVoidFunction>(myvkCreateInstance);
    if (n == "vkDestroyInstance") return reinterpret_cast<PFN_vkVoidFunction>(myvkDestroyInstance);
    if (n == "vkCreateDevice") return reinterpret_cast<PFN_vkVoidFunction>(myvkCreateDevice);
    if (n == "vkDestroyDevice") return reinterpret_cast<PFN_vkVoidFunction>(myvkDestroyDevice);
    if (n == "vkCreateSwapchainKHR") return reinterpret_cast<PFN_vkVoidFunction>(myvkCreateSwapchainKHR);
    if (n == "vkDestroySwapchainKHR") return reinterpret_cast<PFN_vkVoidFunction>(myvkDestroySwapchainKHR);
    if (n == "vkQueuePresentKHR") return reinterpret_cast<PFN_vkVoidFunction>(myvkQueuePresentKHR);
    if (n == "vkGetInstanceProcAddr") return reinterpret_cast<PFN_vkVoidFunction>(+[](VkInstance, const char* p) -> PFN_vkVoidFunction { return lookup(p); });
    if (n == "vkGetDeviceProcAddr") return reinterpret_cast<PFN_vkVoidFunction>(+[](VkDevice, const char* p) -> PFN_vkVoidFunction { return lookup(p); });
    return nullptr;
}

} // namespace

extern "C" {
__attribute__((visibility("default")))
VkResult vkNegotiateLoaderLayerInterfaceVersion(VkNegotiateLayerInterface* v) {
    if (!v || v->sType != LAYER_NEGOTIATE_INTERFACE_STRUCT || v->loaderLayerInterfaceVersion < 2)
        return VK_ERROR_INITIALIZATION_FAILED;
    v->loaderLayerInterfaceVersion = 2;
    v->pfnGetPhysicalDeviceProcAddr = nullptr;
    v->pfnGetDeviceProcAddr = [](VkDevice d, const char* n) -> PFN_vkVoidFunction {
        if (auto f = lookup(n)) return f;
        DevFuncs df = devFuncsFor(d);
        if (df.nextGetDevice) return df.nextGetDevice(d, n);
        return nullptr;
    };
    v->pfnGetInstanceProcAddr = [](VkInstance i, const char* n) -> PFN_vkVoidFunction {
        if (auto f = lookup(n)) return f;
        std::lock_guard<std::mutex> lock(G().mtx);
        if (G().nextGetInstance) return G().nextGetInstance(i, n);
        return nullptr;
    };
    return VK_SUCCESS;
}
} // extern "C"
