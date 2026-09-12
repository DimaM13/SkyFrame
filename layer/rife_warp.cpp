#include "rife_ops.h"

#if HAVE_NCNN

#if __has_include(<ncnn/command.h>)
#include <ncnn/command.h>
#include <ncnn/gpu.h>
#else
#include <command.h>
#include <gpu.h>
#endif
#include <cmath>
#include <algorithm>

namespace skyframe {

static const char warp_comp_data[] = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif

layout (binding = 0) readonly buffer image_blob { sfp image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfp top_blob_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;

void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;

    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);

    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;

    // bilinear interpolate
    afp v;
    {
        int x0 = int(floor(sample_x));
        int y0 = int(floor(sample_y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, p.w - 1);
        y0 = clamp(y0, 0, p.h - 1);
        x1 = clamp(x1, 0, p.w - 1);
        y1 = clamp(y1, 0, p.h - 1);

        afp alpha = sample_x - afp(x0);
        afp beta = sample_y - afp(y0);

        afp v0 = buffer_ld1(image_blob_data, gz * p.cstep + y0 * p.w + x0);
        afp v1 = buffer_ld1(image_blob_data, gz * p.cstep + y0 * p.w + x1);
        afp v2 = buffer_ld1(image_blob_data, gz * p.cstep + y1 * p.w + x0);
        afp v3 = buffer_ld1(image_blob_data, gz * p.cstep + y1 * p.w + x1);

        afp v4 = v0 * (afp(1.f) - alpha) + v1 * alpha;
        afp v5 = v2 * (afp(1.f) - alpha) + v3 * alpha;

        v = v4 * (afp(1.f) - beta) + v5 * beta;
    }

    const int gi = gz * p.cstep + gy * p.w + gx;

    buffer_st1(top_blob_data, gi, v);
}
)";

static const char warp_pack4_comp_data[] = R"(
#version 450

#if NCNN_fp16_storage
#extension GL_EXT_shader_16bit_storage: require
#endif
#if NCNN_fp16_arithmetic
#extension GL_EXT_shader_explicit_arithmetic_types_float16: require
#endif

layout (binding = 0) readonly buffer image_blob { sfpvec4 image_blob_data[]; };
layout (binding = 1) readonly buffer flow_blob { sfp flow_blob_data[]; };
layout (binding = 2) writeonly buffer top_blob { sfpvec4 top_blob_data[]; };

layout (push_constant) uniform parameter
{
    int w;
    int h;
    int c;
    int cstep;
} p;

void main()
{
    int gx = int(gl_GlobalInvocationID.x);
    int gy = int(gl_GlobalInvocationID.y);
    int gz = int(gl_GlobalInvocationID.z);

    if (gx >= p.w || gy >= p.h || gz >= p.c)
        return;

    afp flow_x = buffer_ld1(flow_blob_data, gy * p.w + gx);
    afp flow_y = buffer_ld1(flow_blob_data, p.cstep + gy * p.w + gx);

    afp sample_x = afp(gx) + flow_x;
    afp sample_y = afp(gy) + flow_y;

    // bilinear interpolate
    afpvec4 v;
    {
        int x0 = int(floor(sample_x));
        int y0 = int(floor(sample_y));
        int x1 = x0 + 1;
        int y1 = y0 + 1;

        x0 = clamp(x0, 0, p.w - 1);
        y0 = clamp(y0, 0, p.h - 1);
        x1 = clamp(x1, 0, p.w - 1);
        y1 = clamp(y1, 0, p.h - 1);

        afp alpha = sample_x - afp(x0);
        afp beta = sample_y - afp(y0);

        afpvec4 v0 = buffer_ld4(image_blob_data, gz * p.cstep + y0 * p.w + x0);
        afpvec4 v1 = buffer_ld4(image_blob_data, gz * p.cstep + y0 * p.w + x1);
        afpvec4 v2 = buffer_ld4(image_blob_data, gz * p.cstep + y1 * p.w + x0);
        afpvec4 v3 = buffer_ld4(image_blob_data, gz * p.cstep + y1 * p.w + x1);

        afpvec4 v4 = v0 * (afp(1.f) - alpha) + v1 * alpha;
        afpvec4 v5 = v2 * (afp(1.f) - alpha) + v3 * alpha;

        v = v4 * (afp(1.f) - beta) + v5 * beta;
    }

    const int gi = gz * p.cstep + gy * p.w + gx;

    buffer_st4(top_blob_data, gi, v);
}
)";

RifeWarp::RifeWarp() {
    support_vulkan = true;
    support_packing = false;
    pipeline_warp = nullptr;
    pipeline_warp_pack4 = nullptr;
}

int RifeWarp::create_pipeline(const ncnn::Option& opt) {
    if (!vkdev) return 0;

    std::vector<ncnn::vk_specialization_type> specializations;

    // 1. Pack1
    {
        static std::vector<uint32_t> spirv;
        static ncnn::Mutex lock;
        {
            ncnn::MutexLockGuard guard(lock);
            if (spirv.empty()) {
                ncnn::compile_spirv_module(warp_comp_data, opt, spirv);
            }
        }

        if (!spirv.empty()) {
            pipeline_warp = new ncnn::Pipeline(vkdev);
            pipeline_warp->set_optimal_local_size_xyz();
            pipeline_warp->create(spirv.data(), spirv.size() * 4, specializations);
        }
    }

    // 2. Pack4
    {
        static std::vector<uint32_t> spirv;
        static ncnn::Mutex lock;
        {
            ncnn::MutexLockGuard guard(lock);
            if (spirv.empty()) {
                ncnn::compile_spirv_module(warp_pack4_comp_data, opt, spirv);
            }
        }

        if (!spirv.empty()) {
            pipeline_warp_pack4 = new ncnn::Pipeline(vkdev);
            pipeline_warp_pack4->set_optimal_local_size_xyz();
            pipeline_warp_pack4->create(spirv.data(), spirv.size() * 4, specializations);
        }
    }

    return 0;
}

int RifeWarp::destroy_pipeline(const ncnn::Option& /*opt*/) {
    delete pipeline_warp;
    pipeline_warp = nullptr;

    delete pipeline_warp_pack4;
    pipeline_warp_pack4 = nullptr;

    return 0;
}

int RifeWarp::forward(const std::vector<ncnn::Mat>& bottom_blobs, std::vector<ncnn::Mat>& top_blobs, const ncnn::Option& opt) const {
    const ncnn::Mat& image_blob = bottom_blobs[0];
    const ncnn::Mat& flow_blob = bottom_blobs[1];

    int w = image_blob.w;
    int h = image_blob.h;
    int channels = image_blob.c;

    ncnn::Mat& top_blob = top_blobs[0];
    top_blob.create(w, h, channels);
    if (top_blob.empty()) return -100;

    #pragma omp parallel for num_threads(opt.num_threads)
    for (int q = 0; q < channels; q++) {
        float* outptr = top_blob.channel(q);
        const ncnn::Mat image = image_blob.channel(q);
        const float* fxptr = flow_blob.channel(0);
        const float* fyptr = flow_blob.channel(1);

        for (int y = 0; y < h; y++) {
            for (int x = 0; x < w; x++) {
                float flow_x = fxptr[0];
                float flow_y = fyptr[0];
                float sample_x = x + flow_x;
                float sample_y = y + flow_y;

                int x0 = static_cast<int>(std::floor(sample_x));
                int y0 = static_cast<int>(std::floor(sample_y));
                int x1 = x0 + 1;
                int y1 = y0 + 1;

                x0 = std::min(std::max(x0, 0), w - 1);
                y0 = std::min(std::max(y0, 0), h - 1);
                x1 = std::min(std::max(x1, 0), w - 1);
                y1 = std::min(std::max(y1, 0), h - 1);

                float alpha = sample_x - x0;
                float beta = sample_y - y0;

                float v0 = image.row(y0)[x0];
                float v1 = image.row(y0)[x1];
                float v2 = image.row(y1)[x0];
                float v3 = image.row(y1)[x1];

                float v4 = v0 * (1.0f - alpha) + v1 * alpha;
                float v5 = v2 * (1.0f - alpha) + v3 * alpha;
                outptr[0] = v4 * (1.0f - beta) + v5 * beta;

                outptr += 1;
                fxptr += 1;
                fyptr += 1;
            }
        }
    }

    return 0;
}

int RifeWarp::forward(const std::vector<ncnn::VkMat>& bottom_blobs, std::vector<ncnn::VkMat>& top_blobs, ncnn::VkCompute& cmd, const ncnn::Option& opt) const {
    const ncnn::VkMat& image_blob = bottom_blobs[0];
    const ncnn::VkMat& flow_blob = bottom_blobs[1];

    int w = image_blob.w;
    int h = image_blob.h;
    int channels = image_blob.c;
    size_t elemsize = image_blob.elemsize;
    int elempack = image_blob.elempack;

    ncnn::VkMat& top_blob = top_blobs[0];
    top_blob.create(w, h, channels, elemsize, elempack, opt.blob_vkallocator);
    if (top_blob.empty()) return -100;

    std::vector<ncnn::VkMat> bindings(3);
    bindings[0] = image_blob;
    bindings[1] = flow_blob;
    bindings[2] = top_blob;

    std::vector<ncnn::vk_constant_type> constants(4);
    constants[0].i = top_blob.w;
    constants[1].i = top_blob.h;
    constants[2].i = top_blob.c;
    constants[3].i = top_blob.cstep;

    if (elempack == 4 && pipeline_warp_pack4) {
        cmd.record_pipeline(pipeline_warp_pack4, bindings, constants, top_blob);
    } else if (pipeline_warp) {
        cmd.record_pipeline(pipeline_warp, bindings, constants, top_blob);
    }

    return 0;
}

ncnn::Layer* RifeWarp_layer_creator(void* /*userdata*/) {
    return new RifeWarp();
}

} // namespace skyframe

#endif // HAVE_NCNN
