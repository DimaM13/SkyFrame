#pragma once

#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace skyframe {

class FlowEstimator {
public:
    FlowEstimator();
    ~FlowEstimator();

    bool LoadModel(const std::string& modelDir, int gpuDeviceIndex = 0);

    // Estimates optical flow between frame0 and frame1 at target flow resolution
    // Outputs flow (RG32F) and mask (R32F) textures
    bool EstimateFlow(
        VkCommandBuffer cmd,
        VkImageView frame0View,
        VkImageView frame1View,
        VkImageView outFlowView,
        VkImageView outMaskView,
        int flowWidth,
        int flowHeight
    );

    int GetOptimalFlowWidth(int mode, int screenWidth);
    int GetOptimalFlowHeight(int mode, int screenHeight);

private:
    class Impl;
    Impl* pImpl = nullptr;
};

} // namespace skyframe
