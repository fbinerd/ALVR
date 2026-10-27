#include "EncodePipeline.h"

#include "alvr_server/Logger.h"
#include "alvr_server/bindings.h"
// #include "EncodePipelineSW.h"
#include "EncodePipelineNvEnc.h"
#include "EncodePipelineVAAPI.h"
#include "ffmpeg_helper.h"

extern "C" {
#include <libavcodec/avcodec.h>
}

// TODO: Uninheritance this
void alvr::EncodePipeline::SetParams(FfiDynamicEncoderParams params) {
    if (params.updated) {
        encoder_ctx->bit_rate = params.bitrate_bps / params.framerate * 60.0;
        encoder_ctx->framerate = AVRational { 60, 1 };
        encoder_ctx->rc_buffer_size = encoder_ctx->bit_rate / 60.0 * 1.1;
        encoder_ctx->rc_max_rate = encoder_ctx->bit_rate;
        encoder_ctx->rc_initial_buffer_occupancy = encoder_ctx->rc_buffer_size / 4 * 3;
    }
}

char const* alvr::encoderBackendName(EncoderBackend backend) {
    switch (backend) {
    case EncoderBackend::Automatic:
        return "automatic";
    case EncoderBackend::Vaapi:
        return "VAAPI";
    case EncoderBackend::Nvenc:
        return "NVENC";
    case EncoderBackend::VulkanVideo:
        return "Vulkan Video";
    case EncoderBackend::Software:
        return "software";
    }
    return "unknown";
}

// Builds the encoder for a resolved target. Returns nullptr (after logging
// why) when the technology is not available, which leaves the stream idle
// until the settings change, as before.
std::unique_ptr<alvr::EncodePipeline> alvr::EncodePipeline::Create(
    alvr::VkContext& vk_ctx,
    EncoderTarget const& target,
    VkFrame* input_frame,
    /* VkFrameCtx &vk_frame_ctx,  */ uint32_t width,
    uint32_t height
) {
    switch (target.backend) {
    case EncoderBackend::Nvenc:
        try {
            if (target.crossDevice) {
                return std::make_unique<alvr::EncodePipelineNvEnc>(target, width, height);
            }
            if (!input_frame)
                throw std::runtime_error("NVENC Vulkan input frame is missing");
            alvr::HWContext hwCtx(vk_ctx);
            auto nvenc = std::make_unique<alvr::EncodePipelineNvEnc>(
                hwCtx, vk_ctx, *input_frame, width, height
            );
            Info("Using NvEnc encoder");
            return nvenc;
        } catch (std::exception& e) {
            Error(
                "Failed to create NvEnc encoder: %s\nPlease make sure you have installed CUDA "
                "runtime.",
                e.what()
            );
        }
        return nullptr;
    case EncoderBackend::Vaapi:
        try {
            if (!input_frame)
                throw std::runtime_error("VAAPI Vulkan input frame is missing");
            alvr::HWContext hwCtx(vk_ctx);
            auto vaapi = std::make_unique<alvr::EncodePipelineVAAPI>(
                hwCtx,
                target.devicePath,
                target.vendor,
                *input_frame,
                width,
                height,
                target.hostPixels,
                target.hostRowPitch
            );
            Info("Using VAAPI encoder");
            return vaapi;
        } catch (std::exception& e) {
            Error(
                "Failed to create VAAPI encoder: %s\nPlease make sure you have installed VAAPI "
                "runtime.",
                e.what()
            );
        }
        return nullptr;
    case EncoderBackend::VulkanVideo:
        // Placeholder: FFmpeg is built with Vulkan, but its *_vulkan encoders
        // are not enabled and there is no pipeline for them yet.
        Error("Vulkan Video encoding is not implemented yet");
        return nullptr;
    case EncoderBackend::Software:
        // The x264 pipeline (EncodePipelineSW) is not built on Linux.
        Error("Software encoding is not implemented on Linux yet");
        return nullptr;
    case EncoderBackend::Automatic:
        break;
    }
    Error("Encoder technology was not resolved");
    return nullptr;
}

alvr::EncodePipeline::~EncodePipeline() {
    av_packet_free(&encoder_packet);
    avcodec_free_context(&encoder_ctx);
}

bool alvr::EncodePipeline::GetEncoded(FramePacket& packet) {
    av_packet_free(&encoder_packet);
    encoder_packet = av_packet_alloc();
    int err = avcodec_receive_packet(encoder_ctx, encoder_packet);
    if (err != 0) {
        av_packet_free(&encoder_packet);
        if (err == AVERROR(EAGAIN)) {
            return false;
        }
        throw alvr::AvException("failed to encode", err);
    }
    packet.data = encoder_packet->data;
    packet.size = encoder_packet->size;
    packet.pts = encoder_packet->pts;
    packet.isIDR = (encoder_packet->flags & AV_PKT_FLAG_KEY) != 0;
    return true;
}

int alvr::EncodePipeline::GetCodec() { return Settings_Instance()->m_codec; }
