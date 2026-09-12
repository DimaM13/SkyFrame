#pragma once

#include <vulkan/vulkan.h>
#include <vector>
#include <memory>
#include <mutex>
#include <cstdint>

namespace skyframe {

struct WarpPushConstants {
    float time_step;
    int   hud_protection;
    float hud_threshold;
    int   width;
    int   height;
    int   show_hud;
};

struct WarperDeviceDispatch {
    PFN_vkCreateSampler CreateSampler = nullptr;
    PFN_vkDestroySampler DestroySampler = nullptr;
    PFN_vkCreateDescriptorPool CreateDescriptorPool = nullptr;
    PFN_vkDestroyDescriptorPool DestroyDescriptorPool = nullptr;
    PFN_vkCreateDescriptorSetLayout CreateDescriptorSetLayout = nullptr;
    PFN_vkDestroyDescriptorSetLayout DestroyDescriptorSetLayout = nullptr;
    PFN_vkCreatePipelineLayout CreatePipelineLayout = nullptr;
    PFN_vkDestroyPipelineLayout DestroyPipelineLayout = nullptr;
    PFN_vkCreateComputePipelines CreateComputePipelines = nullptr;
    PFN_vkDestroyPipeline DestroyPipeline = nullptr;
    PFN_vkCreateShaderModule CreateShaderModule = nullptr;
    PFN_vkDestroyShaderModule DestroyShaderModule = nullptr;
    PFN_vkAllocateDescriptorSets AllocateDescriptorSets = nullptr;
    PFN_vkUpdateDescriptorSets UpdateDescriptorSets = nullptr;
    PFN_vkCreateImage CreateImage = nullptr;
    PFN_vkDestroyImage DestroyImage = nullptr;
    PFN_vkGetImageMemoryRequirements GetImageMemoryRequirements = nullptr;
    PFN_vkAllocateMemory AllocateMemory = nullptr;
    PFN_vkFreeMemory FreeMemory = nullptr;
    PFN_vkBindImageMemory BindImageMemory = nullptr;
    PFN_vkCreateImageView CreateImageView = nullptr;
    PFN_vkDestroyImageView DestroyImageView = nullptr;
    PFN_vkCreateBuffer CreateBuffer = nullptr;
    PFN_vkDestroyBuffer DestroyBuffer = nullptr;
    PFN_vkGetBufferMemoryRequirements GetBufferMemoryRequirements = nullptr;
    PFN_vkBindBufferMemory BindBufferMemory = nullptr;
    PFN_vkMapMemory MapMemory = nullptr;
    PFN_vkUnmapMemory UnmapMemory = nullptr;
    PFN_vkCmdPipelineBarrier CmdPipelineBarrier = nullptr;
    PFN_vkCmdBlitImage CmdBlitImage = nullptr;
    PFN_vkCmdCopyImageToBuffer CmdCopyImageToBuffer = nullptr;
    PFN_vkCmdCopyBufferToImage CmdCopyBufferToImage = nullptr;
    PFN_vkCmdBindPipeline CmdBindPipeline = nullptr;
    PFN_vkCmdBindDescriptorSets CmdBindDescriptorSets = nullptr;
    PFN_vkCmdPushConstants CmdPushConstants = nullptr;
    PFN_vkCmdDispatch CmdDispatch = nullptr;
};

class VulkanWarper {
public:
    VulkanWarper(VkDevice device, const VkPhysicalDeviceMemoryProperties& memProperties, PFN_vkGetDeviceProcAddr gdpa, VkQueue queue, uint32_t queueFamilyIndex);
    ~VulkanWarper();

    bool InitPipelines(const uint32_t* warpSpv, size_t warpSize, const uint32_t* downSpv, size_t downSize);
    
    bool CreateFlowAndMaskTextures(int flowWidth, int flowHeight);
    void DestroyFlowAndMaskTextures();

    bool CreateDownsampleStaging(int flowWidth, int flowHeight, VkFormat format = VK_FORMAT_R8G8B8A8_UNORM);
    void DestroyDownsampleStaging();

    VkDescriptorSet AllocateWarpDescriptorSet();

    bool UpdateFlowAndMask(
        VkCommandBuffer cmd,
        const float* flowData,
        const float* maskData,
        int width,
        int height
    );

    bool ReadbackDownsample(
        VkCommandBuffer cmd,
        VkImage srcImage,
        int srcW, int srcH,
        int dstW, int dstH
    );

    const unsigned char* GetDownsamplePixels() const {
        return static_cast<const unsigned char*>(m_downStagingPtr);
    }

    VkImageView GetFlowImageView() const { return m_flowView; }
    VkImageView GetMaskImageView() const { return m_maskView; }
    bool HasTextures() const { return m_flowImage != VK_NULL_HANDLE; }

    // Warps frame0 and frame1 using optical flow and mask into outImage
    bool WarpFrame(
        VkCommandBuffer cmd,
        VkDescriptorSet descSet,
        VkImageView frame0View,
        VkImageView frame1View,
        VkImageView flowView,
        VkImageView maskView,
        VkImageView outImageView,
        int width,
        int height,
        float timeStep,
        bool hudProtection,
        float hudThreshold,
        bool showHud
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
    uint32_t FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties);

    VkDevice m_device = VK_NULL_HANDLE;
    VkPhysicalDeviceMemoryProperties m_memProperties{};
    WarperDeviceDispatch m_disp{};
    VkQueue m_queue = VK_NULL_HANDLE;
    uint32_t m_queueFamily = 0;

    VkSampler m_linearSampler = VK_NULL_HANDLE;
    VkDescriptorPool m_descriptorPool = VK_NULL_HANDLE;
    
    // Warp pipeline (warp_rgba.comp)
    VkDescriptorSetLayout m_warpDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_warpPipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_warpPipeline = VK_NULL_HANDLE;

    // Downsample pipeline (downsample.comp)
    VkDescriptorSetLayout m_downsampleDescLayout = VK_NULL_HANDLE;
    VkPipelineLayout m_downsamplePipelineLayout = VK_NULL_HANDLE;
    VkPipeline m_downsamplePipeline = VK_NULL_HANDLE;

    // Flow texture (RG32F)
    int m_flowW = 0;
    int m_flowH = 0;
    VkImage m_flowImage = VK_NULL_HANDLE;
    VkDeviceMemory m_flowMem = VK_NULL_HANDLE;
    VkImageView m_flowView = VK_NULL_HANDLE;

    // Mask texture (R32F)
    VkImage m_maskImage = VK_NULL_HANDLE;
    VkDeviceMemory m_maskMem = VK_NULL_HANDLE;
    VkImageView m_maskView = VK_NULL_HANDLE;

    // Staging buffer for Flow + Mask upload
    VkBuffer m_flowStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_flowStagingMem = VK_NULL_HANDLE;
    void* m_flowStagingPtr = nullptr;

    // Downsample image + staging buffer for readback
    VkImage m_downImage = VK_NULL_HANDLE;
    VkDeviceMemory m_downMem = VK_NULL_HANDLE;
    VkImageView m_downView = VK_NULL_HANDLE;
    VkBuffer m_downStagingBuffer = VK_NULL_HANDLE;
    VkDeviceMemory m_downStagingMem = VK_NULL_HANDLE;
    void* m_downStagingPtr = nullptr;

    std::mutex m_mutex;
};

} // namespace skyframe
