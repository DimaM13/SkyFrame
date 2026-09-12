#include "flow_ncnn.h"
#include <iostream>
#include <cmath>
#include <cstring>
#include <algorithm>

#if HAVE_NCNN
#if __has_include(<ncnn/net.h>)
#include <ncnn/net.h>
#include <ncnn/gpu.h>
#else
#include <net.h>
#include <gpu.h>
#endif
#include "rife_ops.h"
#endif

namespace skyframe {

class FlowEstimator::Impl {
public:
    bool initialized = false;
    std::string modelPath;

#if HAVE_NCNN
    ncnn::Net flownet;
    ncnn::VulkanDevice* vkdev = nullptr;
#endif
};

FlowEstimator::FlowEstimator() : pImpl(new Impl()) {}

FlowEstimator::~FlowEstimator() {
#if HAVE_NCNN
    if (pImpl->initialized) {
        pImpl->flownet.clear();
    }
#endif
    delete pImpl;
}

bool FlowEstimator::IsLoaded() const {
    return pImpl && pImpl->initialized;
}

bool FlowEstimator::LoadModel(const std::string& modelDir, int gpuDeviceIndex) {
    pImpl->modelPath = modelDir;
#if HAVE_NCNN
    ncnn::create_gpu_instance();
    pImpl->vkdev = ncnn::get_gpu_device(gpuDeviceIndex);
    if (!pImpl->vkdev) {
        std::cerr << "[SkyFrame] FlowEstimator: No Vulkan GPU device found!" << std::endl;
        return false;
    }

    pImpl->flownet.opt.use_vulkan_compute = true;
    pImpl->flownet.opt.use_fp16_packed = true;
    pImpl->flownet.opt.use_fp16_storage = true;
    pImpl->flownet.opt.use_fp16_arithmetic = true;
    pImpl->flownet.opt.use_packing_layout = true;
    pImpl->flownet.set_vulkan_device(pImpl->vkdev);

    pImpl->flownet.register_custom_layer("rife.Warp", RifeWarp_layer_creator);

    std::string paramPath = modelDir + "/flownet.param";
    std::string binPath = modelDir + "/flownet.bin";

    int retParam = pImpl->flownet.load_param(paramPath.c_str());
    if (retParam != 0) {
        std::cerr << "[SkyFrame] FlowEstimator: Failed to load param from " << paramPath << " (ret=" << retParam << ")" << std::endl;
        return false;
    }

    int retBin = pImpl->flownet.load_model(binPath.c_str());
    if (retBin != 0) {
        std::cerr << "[SkyFrame] FlowEstimator: Failed to load bin from " << binPath << " (ret=" << retBin << ")" << std::endl;
        return false;
    }

    pImpl->initialized = true;
    std::cerr << "[SkyFrame] FlowEstimator: RIFE FlowNet successfully loaded with Vulkan FP16 acceleration!" << std::endl;
    return true;
#else
    pImpl->initialized = false;
    std::cerr << "[SkyFrame] FlowEstimator: Compiled without NCNN, RIFE unavailable." << std::endl;
    return false;
#endif
}

int FlowEstimator::GetOptimalFlowWidth(int mode, int screenWidth) {
    // mode: 0 = Lite (180p base), 1 = Balanced (240p base), 2 = Quality (360p base)
    int target_h = 180;
    if (mode == 0) target_h = 180;
    else if (mode == 1) target_h = 240;
    else if (mode == 2) target_h = 360;

    float aspect = 16.0f / 10.0f; // Steam Deck 1280x800 native aspect
    if (screenWidth > 0 && target_h > 0) {
        // preserve actual aspect ratio
        aspect = 16.0f / 10.0f;
    }
    int w = static_cast<int>(std::round(target_h * aspect));
    return ((w + 31) / 32) * 32;
}

int FlowEstimator::GetOptimalFlowHeight(int mode, int screenHeight) {
    int target_h = 180;
    if (mode == 0) target_h = 180;
    else if (mode == 1) target_h = 240;
    else if (mode == 2) target_h = 360;
    return ((target_h + 31) / 32) * 32;
}

bool FlowEstimator::EstimateFlow(
    const unsigned char* frame0Pixels,
    const unsigned char* frame1Pixels,
    int srcWidth,
    int srcHeight,
    int pixelType,
    float* outFlow,
    float* outMask,
    int flowWidth,
    int flowHeight
) {
#if HAVE_NCNN
    if (!pImpl->initialized || !frame0Pixels || !frame1Pixels || !outFlow || !outMask) {
        return false;
    }

    int w = flowWidth;
    int h = flowHeight;
    int w_padded = ((w + 31) / 32) * 32;
    int h_padded = ((h + 31) / 32) * 32;

    int ncnnPixelType = ncnn::Mat::PIXEL_RGBA2RGB;
    if (pixelType == 1) ncnnPixelType = ncnn::Mat::PIXEL_BGRA2RGB;
    else if (pixelType == 2) ncnnPixelType = ncnn::Mat::PIXEL_RGB;

    ncnn::Mat in0_raw;
    ncnn::Mat in1_raw;

    if (srcWidth == w && srcHeight == h) {
        in0_raw = ncnn::Mat::from_pixels(frame0Pixels, ncnnPixelType, w, h);
        in1_raw = ncnn::Mat::from_pixels(frame1Pixels, ncnnPixelType, w, h);
    } else {
        in0_raw = ncnn::Mat::from_pixels_resize(frame0Pixels, ncnnPixelType, srcWidth, srcHeight, w, h);
        in1_raw = ncnn::Mat::from_pixels_resize(frame1Pixels, ncnnPixelType, srcWidth, srcHeight, w, h);
    }

    const float norm_val = 1.0f / 255.0f;

    ncnn::Mat in0_padded(w_padded, h_padded, 3);
    ncnn::Mat in1_padded(w_padded, h_padded, 3);

    for (int q = 0; q < 3; q++) {
        float* dst0 = in0_padded.channel(q);
        float* dst1 = in1_padded.channel(q);
        const float* src0 = in0_raw.channel(q);
        const float* src1 = in1_raw.channel(q);

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                dst0[x] = src0[x] * norm_val;
                dst1[x] = src1[x] * norm_val;
            }
            dst0 += w_padded;
            dst1 += w_padded;
            src0 += w;
            src1 += w;
        }

        // zero bottom padding
        for (int y = h; y < h_padded; y++) {
            memset(dst0, 0, w_padded * sizeof(float));
            memset(dst1, 0, w_padded * sizeof(float));
            dst0 += w_padded;
            dst1 += w_padded;
        }
    }

    ncnn::Mat timestep_padded(w_padded, h_padded, 1);
    timestep_padded.fill(0.5f);

    ncnn::Extractor ex = pImpl->flownet.create_extractor();
    ex.input("in0", in0_padded);
    ex.input("in1", in1_padded);
    ex.input("in2", timestep_padded);

    ncnn::Mat out0;
    int ret = ex.extract("out0", out0);
    if (ret != 0 || out0.empty()) {
        return false;
    }

    // out0: channel 0 = flow_x (pixels), channel 1 = flow_y (pixels), channel 2 = mask [0, 1]
    float inv_w = 1.0f / static_cast<float>(w);
    float inv_h = 1.0f / static_cast<float>(h);

    for (int y = 0; y < h; y++) {
        const float* fx = out0.channel(0).row(y);
        const float* fy = out0.channel(1).row(y);
        const float* fm = out0.channel(2).row(y);

        for (int x = 0; x < w; x++) {
            outFlow[(y * w + x) * 2 + 0] = fx[x] * inv_w;
            outFlow[(y * w + x) * 2 + 1] = fy[x] * inv_h;
            outMask[y * w + x] = fm[x];
        }
    }

    return true;
#else
    return false;
#endif
}

} // namespace skyframe
