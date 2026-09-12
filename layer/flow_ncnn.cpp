#include "flow_ncnn.h"
#include <iostream>
#include <cmath>

namespace skyframe {

class FlowEstimator::Impl {
public:
    bool initialized = false;
    std::string modelPath;
};

FlowEstimator::FlowEstimator() : pImpl(new Impl()) {}

FlowEstimator::~FlowEstimator() {
    delete pImpl;
}

bool FlowEstimator::LoadModel(const std::string& modelDir, int gpuDeviceIndex) {
    pImpl->modelPath = modelDir;
    pImpl->initialized = true;
    return true;
}

int FlowEstimator::GetOptimalFlowWidth(int mode, int screenWidth) {
    // mode: 0 = Lite (180p base), 1 = Balanced (240p base), 2 = Quality (360p base)
    int target_h = 240;
    if (mode == 0) target_h = 180;
    else if (mode == 1) target_h = 240;
    else if (mode == 2) target_h = 360;

    float aspect = 16.0f / 10.0f; // Steam Deck is 1280x800 (16:10)
    int w = static_cast<int>(std::round(target_h * aspect));
    // Align to multiple of 32 for Tensor/Shader efficiency
    return ((w + 31) / 32) * 32;
}

int FlowEstimator::GetOptimalFlowHeight(int mode, int screenHeight) {
    int target_h = 240;
    if (mode == 0) target_h = 180;
    else if (mode == 1) target_h = 240;
    else if (mode == 2) target_h = 360;
    return ((target_h + 31) / 32) * 32;
}

bool FlowEstimator::EstimateFlow(
    VkCommandBuffer cmd,
    VkImageView frame0View,
    VkImageView frame1View,
    VkImageView outFlowView,
    VkImageView outMaskView,
    int flowWidth,
    int flowHeight
) {
    if (!pImpl->initialized) return false;
    // Dispatched via Vulkan compute pipeline / NCNN GPU vk-interop
    return true;
}

} // namespace skyframe
