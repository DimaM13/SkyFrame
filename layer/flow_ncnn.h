#pragma once

#include <string>
#include <vector>

namespace skyframe {

class FlowEstimator {
public:
    FlowEstimator();
    ~FlowEstimator();

    bool LoadModel(const std::string& modelDir, int gpuDeviceIndex = 0);
    bool IsLoaded() const;
    bool Warmup(int width, int height);

    // Estimates optical flow and mask between frame0 and frame1
    // frame0Pixels, frame1Pixels: pointers to frame data
    // pixelType: 0 = RGBA, 1 = BGRA, 2 = RGB
    // outFlow: float array of size (flowWidth * flowHeight * 2) containing normalized (dx, dy)
    // outMask: float array of size (flowWidth * flowHeight) containing occlusion mask [0, 1]
    bool EstimateFlow(
        const unsigned char* frame0Pixels,
        const unsigned char* frame1Pixels,
        int srcWidth,
        int srcHeight,
        int pixelType,
        float* outFlow,
        float* outMask,
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
