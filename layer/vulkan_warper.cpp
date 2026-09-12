#include "vulkan_warper.h"
#include <cstring>
#include <iostream>

namespace skyframe {

static VkShaderModule CreateShaderModule(VkDevice device, const std::vector<uint32_t>& spirv) {
    VkShaderModuleCreateInfo ci{};
    ci.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
    ci.codeSize = spirv.size() * sizeof(uint32_t);
    ci.pCode = spirv.data();
    VkShaderModule mod = VK_NULL_HANDLE;
    if (vkCreateShaderModule(device, &ci, nullptr, &mod) != VK_SUCCESS) {
        return VK_NULL_HANDLE;
    }
    return mod;
}

VulkanWarper::VulkanWarper(VkDevice device, VkPhysicalDevice physicalDevice, VkQueue queue, uint32_t queueFamilyIndex)
    : m_device(device), m_physDevice(physicalDevice), m_queue(queue), m_queueFamily(queueFamilyIndex) {
    
    // Linear clamp-to-edge sampler
    VkSamplerCreateInfo sci{};
    sci.sType = VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO;
    sci.magFilter = VK_FILTER_LINEAR;
    sci.minFilter = VK_FILTER_LINEAR;
    sci.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
    sci.addressModeU = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeV = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    sci.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
    vkCreateSampler(m_device, &sci, nullptr, &m_linearSampler);

    // Descriptor pool
    VkDescriptorPoolSize poolSizes[] = {
        { VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 64 },
        { VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 32 }
    };
    VkDescriptorPoolCreateInfo pci{};
    pci.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO;
    pci.maxSets = 32;
    pci.poolSizeCount = 2;
    pci.pPoolSizes = poolSizes;
    vkCreateDescriptorPool(m_device, &pci, nullptr, &m_descriptorPool);
}

VulkanWarper::~VulkanWarper() {
    if (m_linearSampler) vkDestroySampler(m_device, m_linearSampler, nullptr);
    if (m_descriptorPool) vkDestroyDescriptorPool(m_device, m_descriptorPool, nullptr);

    if (m_warpPipeline) vkDestroyPipeline(m_device, m_warpPipeline, nullptr);
    if (m_warpPipelineLayout) vkDestroyPipelineLayout(m_device, m_warpPipelineLayout, nullptr);
    if (m_warpDescLayout) vkDestroyDescriptorSetLayout(m_device, m_warpDescLayout, nullptr);

    if (m_downsamplePipeline) vkDestroyPipeline(m_device, m_downsamplePipeline, nullptr);
    if (m_downsamplePipelineLayout) vkDestroyPipelineLayout(m_device, m_downsamplePipelineLayout, nullptr);
    if (m_downsampleDescLayout) vkDestroyDescriptorSetLayout(m_device, m_downsampleDescLayout, nullptr);
}

bool VulkanWarper::InitPipelines(const std::vector<uint32_t>& warpSpirv, const std::vector<uint32_t>& downsampleSpirv) {
    std::lock_guard<std::mutex> lock(m_mutex);

    // 1. Warp Pipeline Descriptor Layout:
    // Binding 0: Frame0 (sampler2D)
    // Binding 1: Frame1 (sampler2D)
    // Binding 2: Flow (sampler2D)
    // Binding 3: Mask (sampler2D)
    // Binding 4: OutImage (writeonly storage image)
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
    if (vkCreateDescriptorSetLayout(m_device, &dlci, nullptr, &m_warpDescLayout) != VK_SUCCESS) {
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
    if (vkCreatePipelineLayout(m_device, &plci, nullptr, &m_warpPipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkShaderModule warpModule = CreateShaderModule(m_device, warpSpirv);
    if (!warpModule) return false;

    VkComputePipelineCreateInfo cpci{};
    cpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    cpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    cpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    cpci.stage.module = warpModule;
    cpci.stage.pName = "main";
    cpci.layout = m_warpPipelineLayout;

    VkResult res = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &cpci, nullptr, &m_warpPipeline);
    vkDestroyShaderModule(m_device, warpModule, nullptr);
    if (res != VK_SUCCESS) return false;

    // 2. Downsample Pipeline Descriptor Layout:
    // Binding 0: inFrame (sampler2D)
    // Binding 1: outLowRes (storage image)
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
    if (vkCreateDescriptorSetLayout(m_device, &ddlci, nullptr, &m_downsampleDescLayout) != VK_SUCCESS) {
        return false;
    }

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
    if (vkCreatePipelineLayout(m_device, &dplci, nullptr, &m_downsamplePipelineLayout) != VK_SUCCESS) {
        return false;
    }

    VkShaderModule downModule = CreateShaderModule(m_device, downsampleSpirv);
    if (!downModule) return false;

    VkComputePipelineCreateInfo dcpci{};
    dcpci.sType = VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO;
    dcpci.stage.sType = VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO;
    dcpci.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
    dcpci.stage.module = downModule;
    dcpci.stage.pName = "main";
    dcpci.layout = m_downsamplePipelineLayout;

    res = vkCreateComputePipelines(m_device, VK_NULL_HANDLE, 1, &dcpci, nullptr, &m_downsamplePipeline);
    vkDestroyShaderModule(m_device, downModule, nullptr);
    return (res == VK_SUCCESS);
}

bool VulkanWarper::WarpFrame(
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
) {
    std::lock_guard<std::mutex> lock(m_mutex);

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_warpDescLayout;
    if (vkAllocateDescriptorSets(m_device, &ai, &descSet) != VK_SUCCESS) {
        return false;
    }

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

    vkUpdateDescriptorSets(m_device, 5, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_warpPipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_warpPipelineLayout, 0, 1, &descSet, 0, nullptr);

    WarpPushConstants pc{};
    pc.time_step = timeStep;
    pc.hud_protection = hudProtection ? 1 : 0;
    pc.hud_threshold = hudThreshold;
    pc.width = width;
    pc.height = height;
    vkCmdPushConstants(cmd, m_warpPipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(WarpPushConstants), &pc);

    uint32_t groupX = (width + 15) / 16;
    uint32_t groupY = (height + 15) / 16;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    return true;
}

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

    VkDescriptorSet descSet = VK_NULL_HANDLE;
    VkDescriptorSetAllocateInfo ai{};
    ai.sType = VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO;
    ai.descriptorPool = m_descriptorPool;
    ai.descriptorSetCount = 1;
    ai.pSetLayouts = &m_downsampleDescLayout;
    if (vkAllocateDescriptorSets(m_device, &ai, &descSet) != VK_SUCCESS) {
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

    vkUpdateDescriptorSets(m_device, 2, writes, 0, nullptr);

    vkCmdBindPipeline(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downsamplePipeline);
    vkCmdBindDescriptorSets(cmd, VK_PIPELINE_BIND_POINT_COMPUTE, m_downsamplePipelineLayout, 0, 1, &descSet, 0, nullptr);

    struct DownPushConstants { int in_w, in_h, out_w, out_h; } pc = { inWidth, inHeight, outWidth, outHeight };
    vkCmdPushConstants(cmd, m_downsamplePipelineLayout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(pc), &pc);

    uint32_t groupX = (outWidth + 15) / 16;
    uint32_t groupY = (outHeight + 15) / 16;
    vkCmdDispatch(cmd, groupX, groupY, 1);

    return true;
}

} // namespace skyframe
