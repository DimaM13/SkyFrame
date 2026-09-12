#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <mutex>

namespace skyframe {

struct WarpPushConstants {
    float time_step;
    int hud_protection;
    float hud_threshold;
    int width;
    int height;
};

class VulkanWarper {
public:
    VulkanWarper(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, uint32_t queueFamilyIndex);
    ~VulkanWarper();

    bool InitPipelines(const std::vector<uint32_t>& warpSpirv, const std::vector<uint32_t>& downsampleSpirv);
    
    // Warps frame0 and frame1 using optical flow and mask into outImage
    bool WarpFrame(
        VkCommandBuffer cmd,
        VkImageView frame0View,
        VkImageView frame1View,
        VkImageView flowView,
        VkImageView maskView,
        VkImageView outImageView,
        int width,
        int height,
        float timeStep,
        bool hudProtection,
        float hudThreshold
    );

    // Downsamples high-res frame to low-res for RIFE
    bool Downsample(
        VkCommandBuffer cmd,
        VkImageView inView,
        VkImageView outLowResView,
        int inWidth,
        int inHeight,
        int outWidth,
        int outHeight
    );

private:
    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDevice m_physDevice = VK_NULL_HANDLE;
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;

    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    
    // Warp pipeline
    VkDescriptorSetLayout m_warpDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_warpPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_warpPipeline = VK_NULL_HANDLE;

    // Downsample pipeline
    VkDescriptorSetLayout m_downsampleDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_downsamplePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_downsamplePipeline = VK_NULL_HANDLE;

    std::mutex m_mutex;
};

} // namespace skyframe
