#include "vulkan_warper.h"
#include <cstring>
#include <iostream>

namespace skyframe {

void InitWarperDispatch(VkDevice device, PFN_vkGetDeviceProcAddr gdpa, WarperDeviceDispatch& d) {
    auto load = [&](const char* name) -> PFN_vkVoidFunction {
        return gdpa ? gdpa(device, name) : nullptr;
    };
    d.CreateSampler = (PFN_vkCreateSampler)load("vkCreateSampler");
    d.DestroySampler = (PFN_vkDestroySampler)load("vkDestroySampler");
    d.CreateDescriptorPool = (PFN_vkCreateDescriptorPool)load("vkCreateDescriptorPool");
    d.DestroyDescriptorPool = (PFN_vkDestroyDescriptorPool)load("vkDestroyDescriptorPool");
    d.CreateDescriptorSetLayout = (PFN_vkCreateDescriptorSetLayout)load("vkCreateDescriptorSetLayout");
    d.DestroyDescriptorSetLayout = (PFN_vkDestroyDescriptorSetLayout)load("vkDestroyDescriptorSetLayout");
    d.CreatePipelineLayout = (PFN_vkCreatePipelineLayout)load("vkCreatePipelineLayout");
    d.DestroyPipelineLayout = (PFN_vkDestroyPipelineLayout)load("vkDestroyPipelineLayout");
    d.CreateComputePipelines = (PFN_vkCreateComputePipelines)load("vkCreateComputePipelines");
    d.DestroyPipeline = (PFN_vkDestroyPipeline)load("vkDestroyPipeline");
    d.CreateShaderModule = (PFN_vkCreateShaderModule)load("vkCreateShaderModule");
    d.DestroyShaderModule = (PFN_vkDestroyShaderModule)load("vkDestroyShaderModule");
    d.AllocateDescriptorSets = (PFN_vkAllocateDescriptorSets)load("vkAllocateDescriptorSets");
    d.UpdateDescriptorSets = (PFN_vkUpdateDescriptorSets)load("vkUpdateDescriptorSets");
    d.CreateImage = (PFN_vkCreateImage)load("vkCreateImage");
    d.DestroyImage = (PFN_vkDestroyImage)load("vkDestroyImage");
    d.GetImageMemoryRequirements = (PFN_vkGetImageMemoryRequirements)load("vkGetImageMemoryRequirements");
    d.AllocateMemory = (PFN_vkAllocateMemory)load("vkAllocateMemory");
    d.FreeMemory = (PFN_vkFreeMemory)load("vkFreeMemory");
    d.BindImageMemory = (PFN_vkBindImageMemory)load("vkBindImageMemory");
    d.CreateImageView = (PFN_vkCreateImageView)load("vkCreateImageView");
    d.DestroyImageView = (PFN_vkDestroyImageView)load("vkDestroyImageView");
    d.CreateBuffer = (PFN_vkCreateBuffer)load("vkCreateBuffer");
    d.DestroyBuffer = (PFN_vkDestroyBuffer)load("vkDestroyBuffer");
    d.GetBufferMemoryRequirements = (PFN_vkGetBufferMemoryRequirements)load("vkGetBufferMemoryRequirements");
    d.BindBufferMemory = (PFN_vkBindBufferMemory)load("vkBindBufferMemory");
    d.MapMemory = (PFN_vkMapMemory)load("vkMapMemory");
    d.UnmapMemory = (PFN_vkUnmapMemory)load("vkUnmapMemory");
    d.CmdPipelineBarrier = (PFN_vkCmdPipelineBarrier)load("vkCmdPipelineBarrier");
    d.CmdBlitImage = (PFN_vkCmdBlitImage)load("vkCmdBlitImage");
    d.CmdCopyImageToBuffer = (PFN_vkCmdCopyImageToBuffer)load("vkCmdCopyImageToBuffer");
    d.CmdCopyBufferToImage = (PFN_vkCmdCopyBufferToImage)load("vkCmdCopyBufferToImage");
    d.CmdBindPipeline = (PFN_vkCmdBindPipeline)load("vkCmdBindPipeline");
    d.CmdBindDescriptorSets = (PFN_vkCmdBindDescriptorSets)load("vkCmdBindDescriptorSets");
    d.CmdPushConstants = (PFN_vkCmdPushConstants)load("vkCmdPushConstants");
    d.CmdDispatch = (PFN_vkCmdDispatch)load("vkCmdDispatch");

    if (!d.CreateSampler) d.CreateSampler = &vkCreateSampler;
    if (!d.DestroySampler) d.DestroySampler = &vkDestroySampler;
    if (!d.CreateDescriptorPool) d.CreateDescriptorPool = &vkCreateDescriptorPool;
    if (!d.DestroyDescriptorPool) d.DestroyDescriptorPool = &vkDestroyDescriptorPool;
    if (!d.CreateDescriptorSetLayout) d.CreateDescriptorSetLayout = &vkCreateDescriptorSetLayout;
    if (!d.DestroyDescriptorSetLayout) d.DestroyDescriptorSetLayout = &vkDestroyDescriptorSetLayout;
    if (!d.CreatePipelineLayout) d.CreatePipelineLayout = &vkCreatePipelineLayout;
    if (!d.DestroyPipelineLayout) d.DestroyPipelineLayout = &vkDestroyPipelineLayout;
    if (!d.CreateComputePipelines) d.CreateComputePipelines = &vkCreateComputePipelines;
    if (!d.DestroyPipeline) d.DestroyPipeline = &vkDestroyPipeline;
    if (!d.CreateShaderModule) d.CreateShaderModule = &vkCreateShaderModule;
    if (!d.DestroyShaderModule) d.DestroyShaderModule = &vkDestroyShaderModule;
    if (!d.AllocateDescriptorSets) d.AllocateDescriptorSets = &vkAllocateDescriptorSets;
    if (!d.UpdateDescriptorSets) d.UpdateDescriptorSets = &vkUpdateDescriptorSets;
    if (!d.CreateImage) d.CreateImage = &vkCreateImage;
    if (!d.DestroyImage) d.DestroyImage = &vkDestroyImage;
    if (!d.GetImageMemoryRequirements) d.GetImageMemoryRequirements = &vkGetImageMemoryRequirements;
    if (!d.AllocateMemory) d.AllocateMemory = &vkAllocateMemory;
    if (!d.FreeMemory) d.FreeMemory = &vkFreeMemory;
    if (!d.BindImageMemory) d.BindImageMemory = &vkBindImageMemory;
    if (!d.CreateImageView) d.CreateImageView = &vkCreateImageView;
    if (!d.DestroyImageView) d.DestroyImageView = &vkDestroyImageView;
    if (!d.CreateBuffer) d.CreateBuffer = &vkCreateBuffer;
    if (!d.DestroyBuffer) d.DestroyBuffer = &vkDestroyBuffer;
    if (!d.GetBufferMemoryRequirements) d.GetBufferMemoryRequirements = &vkGetBufferMemoryRequirements;
    if (!d.BindBufferMemory) d.BindBufferMemory = &vkBindBufferMemory;
    if (!d.MapMemory) d.MapMemory = &vkMapMemory;
    if (!d.UnmapMemory) d.UnmapMemory = &vkUnmapMemory;
    if (!d.CmdPipelineBarrier) d.CmdPipelineBarrier = &vkCmdPipelineBarrier;
    if (!d.CmdBlitImage) d.CmdBlitImage = &vkCmdBlitImage;
    if (!d.CmdCopyImageToBuffer) d.CmdCopyImageToBuffer = &vkCmdCopyImageToBuffer;
    if (!d.CmdCopyBufferToImage) d.CmdCopyBufferToImage = &vkCmdCopyBufferToImage;
    if (!d.CmdBindPipeline) d.CmdBindPipeline = &vkCmdBindPipeline;
    if (!d.CmdBindDescriptorSets) d.CmdBindDescriptorSets = &vkCmdBindDescriptorSets;
    if (!d.CmdPushConstants) d.CmdPushConstants = &vkCmdPushConstants;
    if (!d.CmdDispatch) d.CmdDispatch = &vkCmdDispatch;
}

static VkShaderModule CreateShaderModule(VkDevice device, const WarperDeviceDispatch& disp, const uint32_t* pCode, size_t codeSize) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = codeSize;
    ci.pCode = pCode;
    VkShaderModule mod = VK_NULL_HANDLE;
    if (disp.CreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return mod;
}

VulkanWarper::VulkanWarper(VkDevice device, const VkPhysicalDeviceMemoryProperties& memProperties, PFN_vkGetDeviceProcAddr gdpa, VkQueue queue, uint32_t queueFamilyIndex)
    : m_device(device), m_memProperties(memProperties), m_queue(queue), m_queueFamily(queueFamilyIndex) {
    
    InitWarperDispatch(m_device, gdpa, m_disp);

    // Linear clamp-to-edge sampler for hardware bilinear interpolation
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    m_disp.CreateSampler(m_device, &sci, nullptr, &m_linearSampler);

    // Descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 64 }
    };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 64;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = poolSizes;
    m_disp.CreateDescriptorPool(m_device, &pci, nullptr, &m_descriptorPool);
}

VulkanWarper::~VulkanWarper() {
    DestroyFlowAndMaskTextures();
    DestroyDownsampleStaging();

    if (m_linearSampler) m_disp.DestroySampler(m_device, m_linearSampler, nullptr);
    if (m_descriptorPool) m_disp.DestroyDescriptorPool(m_device, m_descriptorPool, nullptr);

    if (m_warpPipeline) m_disp.DestroyPipeline(m_device, m_warpPipeline, nullptr);
    if (m_warpPipelineLayout) m_disp.DestroyPipelineLayout(m_device, m_warpPipelineLayout, nullptr);
    if (m_warpDescLayout) m_disp.DestroyDescriptorSetLayout(m_device, m_warpDescLayout, nullptr);

    if (m_downsamplePipeline) m_disp.DestroyPipeline(m_device, m_downsamplePipeline, nullptr);
    if (m_downsamplePipelineLayout) m_disp.DestroyPipelineLayout(m_device, m_downsamplePipelineLayout, nullptr);
    if (m_downsampleDescLayout) m_disp.DestroyDescriptorSetLayout(m_device, m_downsampleDescLayout, nullptr);
}

uint32_t VulkanWarper::FindMemoryType(uint32_t typeFilter, VkMemoryPropertyFlags properties) {
    if (m_memProperties.memoryTypeCount > 0) {
        for (uint32_t i = 0; i < m_memProperties.memoryTypeCount; i++) {
            if ((typeFilter & (1 << i)) && (m_memProperties.memoryTypes[i].propertyFlags & properties) == properties) {
                return i;
            }
        }
        for (uint32_t i = 0; i < m_memProperties.memoryTypeCount; i++) {
            if (typeFilter & (1 << i)) {
                if ((m_memProperties.memoryTypes[i].propertyFlags & properties) != 0) {
                    return i;
                }
            }
        }
        for (uint32_t i = 0; i < m_memProperties.memoryTypeCount; i++) {
            if (typeFilter & (1 << i)) {
                return i;
            }
        }
    }
    for (uint32_t i = 0; i < 32; i++) {
        if (typeFilter & (1 << i)) {
            return i;
        }
    }
    return 0;
}

bool VulkanWarper::InitPipelines(const uint32_t* warpSpv, size_t warpSize, const uint32_t* downSpv, size_t downSize) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. Warp Pipeline Descriptor Layout
    VkDescriptorSetLayoutBinding warpBindings[5]{};
    for (int i = 0; i < 4; ++i) {
        warpBindings[i].binding = i;
        warpBindings[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        warpBindings[i].descriptorCount = 1;
        warpBindings[i].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    }
    warpBindings[4].binding = 4;
    warpBindings[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    warpBindings[4].descriptorCount = 1;
    warpBindings[4].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

    VkDescriptorSetLayoutCreateInfo dlci{};
    dlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
    dlci.bindingCount = 5;
    dlci.pBindings = warpBindings;
    if (m_disp.CreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_warpDescLayout) != VK_SUCCESS) {
        return false;
    }

    VkPushConstantRange pcr{};
    pcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
    pcr.offset = 0;
    pcr.size = sizeof(WarpPushConstants);

    VkPipelineLayoutCreateInfo plci{};
    plci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
    plci.setLayoutCount = 1;
    plci.pSetLayouts = &m_warpDescLayout;
    plci.pushConstantRangeCount = 1;
    plci.pPushConstantRanges = &pcr;
    if (m_disp.CreatePipelineLayout(m_device, &plci, nullptr, &m_warpPipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkShaderModule warpModule = CreateShaderModule(m_device, m_disp, warpSpv, warpSize);
    if (!warpModule) return false;

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = warpModule;
    cpci.stage.pName = "main";
    cpci.layout = m_warpPipelineLayout;

    VkResult res = m_disp.CreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_warpPipeline);
    m_disp.DestroyShaderModule(m_device, warpModule, nullptr);
    if (res != VK_SUCCESS) return false;

    // 2. Downsample Pipeline
    if (downSpv && downSize > 0) {
        VkDescriptorSetLayoutBinding downBindings[2]{};
        downBindings[0].binding = 0;
        downBindings[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        downBindings[0].descriptorCount = 1;
        downBindings[0].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
        downBindings[1].binding = 1;
        downBindings[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
        downBindings[1].descriptorCount = 1;
        downBindings[1].stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;

        VkDescriptorSetLayoutCreateInfo ddlci{};
        ddlci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO;
        ddlci.bindingCount = 2;
        ddlci.pBindings = downBindings;
        if (m_disp.CreateDescriptorSetLayout(m_device, &ddlci, nullptr, &m_downsampleDescLayout) == VK_SUCCESS) {
            struct DownPushConstants { int in_w, in_h, out_w, out_h; };
            VkPushConstantRange dpcr{};
            dpcr.stageFlags = VK_SHADER_STAGE_COMPUTE_BIT;
            dpcr.offset = 0;
            dpcr.size = sizeof(DownPushConstants);

            VkPipelineLayoutCreateInfo dplci{};
            dplci.sType = VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO;
            dplci.setLayoutCount = 1;
            dplci.pSetLayouts = &m_downsampleDescLayout;
            dplci.pushConstantRangeCount = 1;
            dplci.pPushConstantRanges = &dpcr;
            if (m_disp.CreatePipelineLayout(m_device, &dplci, nullptr, &m_downsamplePipelineLayout) == VK_SUCCESS) {
                VkShaderModule downModule = CreateShaderModule(m_device, m_disp, downSpv, downSize);
                if (downModule) {
                    VkComputePipelineCreateInfo dcpci{};
                    dcpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
                    dcpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
                    dcpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
                    dcpci.stage.module = downModule;
                    dcpci.stage.pName = "main";
                    dcpci.layout = m_downsamplePipelineLayout;
                    m_disp.CreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &dcpci, nullptr, &m_downsamplePipeline);
                    m_disp.DestroyShaderModule(m_device, downModule, nullptr);
                }
            }
        }
    }

    return true;
}

bool VulkanWarper::CreateFlowAndMaskTextures(int flowWidth, int flowHeight) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_flowImage && m_flowW == flowWidth && m_flowH == flowHeight) {
        return true;
    }

    DestroyFlowAndMaskTextures();
    m_flowW = flowWidth;
    m_flowH = flowHeight;

    // 1. Create Flow Image (VK_FORMAT_R32G32_SFLOAT)
    VkImageCreateInfo iciFlow{};
    iciFlow.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iciFlow.imageType = VK_IMAGE_TYPE_2D;
    iciFlow.format = VK_FORMAT_R32G32_SFLOAT;
    iciFlow.extent = { (uint32_t)flowWidth, (uint32_t)flowHeight, 1 };
    iciFlow.mipLevels = 1;
    iciFlow.arrayLayers = 1;
    iciFlow.samples = VK_SAMPLE_COUNT_1_BIT;
    iciFlow.tiling = VK_IMAGE_TILING_OPTIMAL;
    iciFlow.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    iciFlow.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_disp.CreateImage(m_device, &iciFlow, nullptr, &m_flowImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqFlow;
    m_disp.GetImageMemoryRequirements(m_device, m_flowImage, &memReqFlow);
    VkMemoryAllocateInfo maiFlow{};
    maiFlow.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    maiFlow.allocationSize = memReqFlow.size;
    maiFlow.memoryTypeIndex = FindMemoryType(memReqFlow.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (m_disp.AllocateMemory(m_device, &maiFlow, nullptr, &m_flowMem) != VK_SUCCESS) return false;
    m_disp.BindImageMemory(m_device, m_flowImage, m_flowMem, 0);

    VkImageViewCreateInfo ivciFlow{};
    ivciFlow.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivciFlow.image = m_flowImage;
    ivciFlow.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivciFlow.format = VK_FORMAT_R32G32_SFLOAT;
    ivciFlow.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (m_disp.CreateImageView(m_device, &ivciFlow, nullptr, &m_flowView) != VK_SUCCESS) return false;

    // 2. Create Mask Image (VK_FORMAT_R32_SFLOAT)
    VkImageCreateInfo iciMask{};
    iciMask.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    iciMask.imageType = VK_IMAGE_TYPE_2D;
    iciMask.format = VK_FORMAT_R32_SFLOAT;
    iciMask.extent = { (uint32_t)flowWidth, (uint32_t)flowHeight, 1 };
    iciMask.mipLevels = 1;
    iciMask.arrayLayers = 1;
    iciMask.samples = VK_SAMPLE_COUNT_1_BIT;
    iciMask.tiling = VK_IMAGE_TILING_OPTIMAL;
    iciMask.usage = VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
    iciMask.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_disp.CreateImage(m_device, &iciMask, nullptr, &m_maskImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqMask;
    m_disp.GetImageMemoryRequirements(m_device, m_maskImage, &memReqMask);
    VkMemoryAllocateInfo maiMask{};
    maiMask.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    maiMask.allocationSize = memReqMask.size;
    maiMask.memoryTypeIndex = FindMemoryType(memReqMask.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (m_disp.AllocateMemory(m_device, &maiMask, nullptr, &m_maskMem) != VK_SUCCESS) return false;
    m_disp.BindImageMemory(m_device, m_maskImage, m_maskMem, 0);

    VkImageViewCreateInfo ivciMask{};
    ivciMask.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivciMask.image = m_maskImage;
    ivciMask.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivciMask.format = VK_FORMAT_R32_SFLOAT;
    ivciMask.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (m_disp.CreateImageView(m_device, &ivciMask, nullptr, &m_maskView) != VK_SUCCESS) return false;

    // 3. Staging Buffer for Flow (RG32F) + Mask (R32F)
    size_t flowBytes = (size_t)flowWidth * flowHeight * 2 * sizeof(float);
    size_t maskBytes = (size_t)flowWidth * flowHeight * 1 * sizeof(float);
    VkDeviceSize stagingSize = flowBytes + maskBytes;

    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = stagingSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_disp.CreateBuffer(m_device, &bci, nullptr, &m_flowStagingBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqBuf;
    m_disp.GetBufferMemoryRequirements(m_device, m_flowStagingBuffer, &memReqBuf);
    VkMemoryAllocateInfo maiBuf{};
    maiBuf.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    maiBuf.allocationSize = memReqBuf.size;
    maiBuf.memoryTypeIndex = FindMemoryType(memReqBuf.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (m_disp.AllocateMemory(m_device, &maiBuf, nullptr, &m_flowStagingMem) != VK_SUCCESS) return false;
    m_disp.BindBufferMemory(m_device, m_flowStagingBuffer, m_flowStagingMem, 0);

    m_disp.MapMemory(m_device, m_flowStagingMem, 0, stagingSize, 0, &m_flowStagingPtr);
    return true;
}

void VulkanWarper::DestroyFlowAndMaskTextures() {
    if (m_flowStagingPtr) {
        m_disp.UnmapMemory(m_device, m_flowStagingMem);
        m_flowStagingPtr = nullptr;
    }
    if (m_flowStagingBuffer) {
        m_disp.DestroyBuffer(m_device, m_flowStagingBuffer, nullptr);
        m_flowStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_flowStagingMem) {
        m_disp.FreeMemory(m_device, m_flowStagingMem, nullptr);
        m_flowStagingMem = VK_NULL_HANDLE;
    }

    if (m_flowView) { m_disp.DestroyImageView(m_device, m_flowView, nullptr); m_flowView = VK_NULL_HANDLE; }
    if (m_flowImage) { m_disp.DestroyImage(m_device, m_flowImage, nullptr); m_flowImage = VK_NULL_HANDLE; }
    if (m_flowMem) { m_disp.FreeMemory(m_device, m_flowMem, nullptr); m_flowMem = VK_NULL_HANDLE; }

    if (m_maskView) { m_disp.DestroyImageView(m_device, m_maskView, nullptr); m_maskView = VK_NULL_HANDLE; }
    if (m_maskImage) { m_disp.DestroyImage(m_device, m_maskImage, nullptr); m_maskImage = VK_NULL_HANDLE; }
    if (m_maskMem) { m_disp.FreeMemory(m_device, m_maskMem, nullptr); m_maskMem = VK_NULL_HANDLE; }
}

bool VulkanWarper::CreateDownsampleStaging(int flowWidth, int flowHeight, VkFormat format) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (m_downImage && m_flowW == flowWidth && m_flowH == flowHeight) {
        return true;
    }

    DestroyDownsampleStaging();

    // 1. Downsample Image matching swapchain format for zero-overhead hardware blit
    VkImageCreateInfo ici{};
    ici.sType = VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO;
    ici.imageType = VK_IMAGE_TYPE_2D;
    ici.format = format;
    ici.extent = { (uint32_t)flowWidth, (uint32_t)flowHeight, 1 };
    ici.mipLevels = 1;
    ici.arrayLayers = 1;
    ici.samples = VK_SAMPLE_COUNT_1_BIT;
    ici.tiling = VK_IMAGE_TILING_OPTIMAL;
    ici.usage = VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
    ici.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    if (m_disp.CreateImage(m_device, &ici, nullptr, &m_downImage) != VK_SUCCESS) return false;

    VkMemoryRequirements memReq;
    m_disp.GetImageMemoryRequirements(m_device, m_downImage, &memReq);
    VkMemoryAllocateInfo mai{};
    mai.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    mai.allocationSize = memReq.size;
    mai.memoryTypeIndex = FindMemoryType(memReq.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
    if (m_disp.AllocateMemory(m_device, &mai, nullptr, &m_downMem) != VK_SUCCESS) return false;
    m_disp.BindImageMemory(m_device, m_downImage, m_downMem, 0);

    VkImageViewCreateInfo ivci{};
    ivci.sType = VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO;
    ivci.image = m_downImage;
    ivci.viewType = VK_IMAGE_VIEW_TYPE_2D;
    ivci.format = format;
    ivci.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
    if (m_disp.CreateImageView(m_device, &ivci, nullptr, &m_downView) != VK_SUCCESS) return false;

    // 2. Readback staging buffer
    VkDeviceSize bufSize = (size_t)flowWidth * flowHeight * 4;
    VkBufferCreateInfo bci{};
    bci.sType = VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO;
    bci.size = bufSize;
    bci.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
    bci.sharingMode = VK_SHARING_MODE_EXCLUSIVE;
    if (m_disp.CreateBuffer(m_device, &bci, nullptr, &m_downStagingBuffer) != VK_SUCCESS) return false;

    VkMemoryRequirements memReqBuf;
    m_disp.GetBufferMemoryRequirements(m_device, m_downStagingBuffer, &memReqBuf);
    VkMemoryAllocateInfo maiBuf{};
    maiBuf.sType = VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO;
    maiBuf.allocationSize = memReqBuf.size;
    maiBuf.memoryTypeIndex = FindMemoryType(memReqBuf.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
    if (m_disp.AllocateMemory(m_device, &maiBuf, nullptr, &m_downStagingMem) != VK_SUCCESS) return false;
    m_disp.BindBufferMemory(m_device, m_downStagingBuffer, m_downStagingMem, 0);

    m_disp.MapMemory(m_device, m_downStagingMem, 0, bufSize, 0, &m_downStagingPtr);
    return true;
}

void VulkanWarper::DestroyDownsampleStaging() {
    if (m_downStagingPtr) {
        m_disp.UnmapMemory(m_device, m_downStagingMem);
        m_downStagingPtr = nullptr;
    }
    if (m_downStagingBuffer) {
        m_disp.DestroyBuffer(m_device, m_downStagingBuffer, nullptr);
        m_downStagingBuffer = VK_NULL_HANDLE;
    }
    if (m_downStagingMem) {
        m_disp.FreeMemory(m_device, m_downStagingMem, nullptr);
        m_downStagingMem = VK_NULL_HANDLE;
    }

    if (m_downView) { m_disp.DestroyImageView(m_device, m_downView, nullptr); m_downView = VK_NULL_HANDLE; }
    if (m_downImage) { m_disp.DestroyImage(m_device, m_downImage, nullptr); m_downImage = VK_NULL_HANDLE; }
    if (m_downMem) { m_disp.FreeMemory(m_device, m_downMem, nullptr); m_downMem = VK_NULL_HANDLE; }
}

bool VulkanWarper::ReadbackDownsample(
    VkCommandBuffer cmd,
    VkImage srcImage,
    int srcW, int srcH,
    int dstW, int dstH
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_downImage || !m_downStagingBuffer) return false;

    // Transition srcImage to TRANSFER_SRC_OPTIMAL and m_downImage to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].image = srcImage;
    barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = m_downImage;
    barriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    m_disp.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    // Blit from srcImage (1280x800) to m_downImage (288x180) with hardware linear downsample
    VkImageBlit blit{};
    blit.srcSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.srcOffsets[0] = { 0, 0, 0 };
    blit.srcOffsets[1] = { srcW, srcH, 1 };
    blit.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    blit.dstOffsets[0] = { 0, 0, 0 };
    blit.dstOffsets[1] = { dstW, dstH, 1 };

    m_disp.CmdBlitImage(cmd, srcImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                        m_downImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                        1, &blit, VK_FILTER_LINEAR);

    // Transition m_downImage to TRANSFER_SRC_OPTIMAL and restore srcImage to PRESENT_SRC_KHR
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;

    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;

    m_disp.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT | VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    // Copy m_downImage to host-visible staging buffer
    VkBufferImageCopy copyRegion{};
    copyRegion.bufferOffset = 0;
    copyRegion.bufferRowLength = dstW;
    copyRegion.bufferImageHeight = dstH;
    copyRegion.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    copyRegion.imageExtent = { (uint32_t)dstW, (uint32_t)dstH, 1 };

    m_disp.CmdCopyImageToBuffer(cmd, m_downImage, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, m_downStagingBuffer, 1, &copyRegion);
    return true;
}

bool VulkanWarper::UpdateFlowAndMask(
    VkCommandBuffer cmd,
    const float* flowData,
    const float* maskData,
    int width,
    int height
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_flowImage || !m_maskImage || !m_flowStagingPtr) return false;

    size_t flowBytes = (size_t)width * height * 2 * sizeof(float);
    size_t maskBytes = (size_t)width * height * 1 * sizeof(float);

    uint8_t* ptr = static_cast<uint8_t*>(m_flowStagingPtr);
    memcpy(ptr, flowData, flowBytes);
    memcpy(ptr + flowBytes, maskData, maskBytes);

    // Transition images to TRANSFER_DST_OPTIMAL
    VkImageMemoryBarrier barriers[2]{};
    barriers[0].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[0].srcAccessMask = 0;
    barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].image = m_flowImage;
    barriers[0].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    barriers[1].sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
    barriers[1].srcAccessMask = 0;
    barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_UNDEFINED;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].image = m_maskImage;
    barriers[1].subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };

    m_disp.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);

    // Copy buffer to flow image (RG32F)
    VkBufferImageCopy flowCopy{};
    flowCopy.bufferOffset = 0;
    flowCopy.bufferRowLength = width;
    flowCopy.bufferImageHeight = height;
    flowCopy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    flowCopy.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
    m_disp.CmdCopyBufferToImage(cmd, m_flowStagingBuffer, m_flowImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &flowCopy);

    // Copy buffer to mask image (R32F)
    VkBufferImageCopy maskCopy{};
    maskCopy.bufferOffset = flowBytes;
    maskCopy.bufferRowLength = width;
    maskCopy.bufferImageHeight = height;
    maskCopy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
    maskCopy.imageExtent = { (uint32_t)width, (uint32_t)height, 1 };
    m_disp.CmdCopyBufferToImage(cmd, m_flowStagingBuffer, m_maskImage, VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &maskCopy);

    // Transition images to SHADER_READ_ONLY_OPTIMAL for sampler2D in warp_rgba
    barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[0].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[0].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[0].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    barriers[1].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
    barriers[1].dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
    barriers[1].oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
    barriers[1].newLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    m_disp.CmdPipelineBarrier(cmd, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 0, nullptr, 0, nullptr, 2, barriers);
    return true;
}

VkDescriptorSet VulkanWarper::AllocateWarpDescriptorSet() {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_warpPipeline || !m_descriptorPool) return VK_NULL_HANDLE;

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_warpDescLayout;
    if (m_disp.AllocateDescriptorSets(m_device, &ai, &descSet) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return descSet;
}

// Warps frame0 and frame1 using optical flow and mask into outImage
bool VulkanWarper::WarpFrame(
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
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_warpPipeline || descSet == VK_NULL_HANDLE) return false;

    VkDescriptorImageInfo imageInfos[4]{};
    VkImageView views[4] = { frame0View, frame1View, flowView, maskView };
    for (int i = 0; i < 4; ++i) {
        imageInfos[i].sampler = m_linearSampler;
        imageInfos[i].imageView = views[i];
        imageInfos[i].imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;
    }

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView = outImageView;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[5]{};
    for (int i = 0; i < 4; ++i) {
        writes[i].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
        writes[i].dstSet = descSet;
        writes[i].dstBinding = i;
        writes[i].descriptorCount = 1;
        writes[i].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
        writes[i].pImageInfo = &imageInfos[i];
    }
    writes[4].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[4].dstSet = descSet;
    writes[4].dstBinding = 4;
    writes[4].descriptorCount = 1;
    writes[4].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[4].pImageInfo = &outInfo;

    m_disp.UpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    m_disp.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_warpPipeline);
    m_disp.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_warpPipelineLayout, 0, 1, &descSet, 0, nullptr);

    WarpPushConstants pc{};
    pc.time_step = timeStep;
    pc.hud_protection = hudProtection ? 1 : 0;
    pc.hud_threshold = hudThreshold;
    pc.width = width;
    pc.height = height;
    pc.show_hud = showHud ? 1 : 0;
    m_disp.CmdPushConstants(cmd, m_warpPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WarpPushConstants), &pc);

    uint32_t groupX = (width + 15) / 16;
    uint32_t groupY = (height + 15) / 16;
    m_disp.CmdDispatch(cmd, groupX, groupY, 1);

    return true;
}

// Downsamples high-res frame to low-res for RIFE
bool VulkanWarper::Downsample(
    VkCommandBuffer cmd,
    VkImageView inView,
    VkImageView outLowResView,
    int inWidth,
    int inHeight,
    int outWidth,
    int outHeight
) {
    std::lock_guard<std::mutex> lock(m_mutex);
    if (!m_downsamplePipeline || !m_descriptorPool) return false;

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_downsampleDescLayout;
    if (m_disp.AllocateDescriptorSets(m_device, &ai, &descSet) != VK_SUCCESS) {
        return false;
    }

    VkDescriptorImageInfo inInfo{};
    inInfo.sampler = m_linearSampler;
    inInfo.imageView = inView;
    inInfo.imageLayout = VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL;

    VkDescriptorImageInfo outInfo{};
    outInfo.imageView = outLowResView;
    outInfo.imageLayout = VK_IMAGE_LAYOUT_GENERAL;

    VkWriteDescriptorSet writes[2]{};
    writes[0].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[0].dstSet = descSet;
    writes[0].dstBinding = 0;
    writes[0].descriptorCount = 1;
    writes[0].descriptorType = VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER;
    writes[0].pImageInfo = &inInfo;

    writes[1].sType = VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET;
    writes[1].dstSet = descSet;
    writes[1].dstBinding = 1;
    writes[1].descriptorCount = 1;
    writes[1].descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
    writes[1].pImageInfo = &outInfo;

    m_disp.UpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    m_disp.CmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downsamplePipeline);
    m_disp.CmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downsamplePipelineLayout, 0, 1, &descSet, 0, nullptr);

    struct DownPushConstants { int in_w, in_h, out_w, out_h; } pc = { inWidth, inHeight, outWidth, outHeight };
    m_disp.CmdPushConstants(cmd, m_downsamplePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t groupX = (outWidth + 15) / 16;
    uint32_t groupY = (outHeight + 15) / 16;
    m_disp.CmdDispatch(cmd, groupX, groupY, 1);

    return true;
}

} // namespace skyframe
