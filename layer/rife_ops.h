#pragma once

#if HAVE_NCNN

#include <vector>
#if __has_include(<ncnn/layer.h>)
#include <ncnn/layer.h>
#include <ncnn/pipeline.h>
#include <ncnn/mat.h>
#else
#include <layer.h>
#include <pipeline.h>
#include <mat.h>
#endif

namespace skyframe {

class RifeWarp : public ncnn::Layer {
public:
    RifeWarp();
    virtual int create_pipeline(const ncnn::Option& opt);
    virtual int destroy_pipeline(const ncnn::Option& opt);
    virtual int forward(const std::vector<ncnn::Mat>& bottom_blobs, std::vector<ncnn::Mat>& top_blobs, const ncnn::Option& opt) const;
    virtual int forward(const std::vector<ncnn::VkMat>& bottom_blobs, std::vector<ncnn::VkMat>& top_blobs, ncnn::VkCompute& cmd, const ncnn::Option& opt) const;

private:
    ncnn::Pipeline* pipeline_warp = nullptr;
    ncnn::Pipeline* pipeline_warp_pack4 = nullptr;
    ncnn::Pipeline* pipeline_warp_pack8 = nullptr;
};

ncnn::Layer* RifeWarp_layer_creator(void* userdata);

} // namespace skyframe

#endif // HAVE_NCNN
