#pragma once

#include "EncodePipeline.h"
#include "NvencHostInput.h"
#include "VkContext.hpp"
#include "ffmpeg_helper.h"
#include <functional>
#include <memory>

extern "C" struct AVBufferRef;
extern "C" struct AVCodecContext;
extern "C" struct AVFrame;

namespace alvr {

class EncodePipelineNvEnc : public EncodePipeline {
public:
    ~EncodePipelineNvEnc();
    EncodePipelineNvEnc(
        HWContext& vk_ctx, VkContext& v_ctx, VkFrame& input_frame, uint32_t width, uint32_t height
    );
    EncodePipelineNvEnc(EncoderTarget const& target, uint32_t width, uint32_t height);

    void PushFrame(uint64_t targetTimestampNs, bool idr) override;
    void SetParams(FfiDynamicEncoderParams params) override;

private:
    void initEncoder(AVPixelFormat surfaceFormat, uint32_t width, uint32_t height);
    VkContext* v_ctx = nullptr;
    // Declared before vk_frame: make_av_frame takes a reference to this, so
    // it has to outlive the frame built from it.
    std::unique_ptr<VkFrameCtx> frame_ctx;
    AVBufferRef* hw_ctx = nullptr;
    std::unique_ptr<AVFrame, std::function<void(AVFrame*)>> vk_frame;
    AVFrame* hw_frame = nullptr;
    std::unique_ptr<NvencHostInput> hostInput;
};
}
