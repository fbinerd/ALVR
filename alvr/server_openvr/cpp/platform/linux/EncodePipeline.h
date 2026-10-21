#pragma once
#include "alvr_server/bindings.h"
#include <cstdint>
#include <memory>
#include <string>

#include "VkContext.hpp"

extern "C" struct AVCodecContext;
extern "C" struct AVPacket;

class Renderer;

namespace alvr {

class VkFrame;
class VkFrameCtx;
class VkContext;

struct FramePacket {
    uint8_t* data;
    int size;
    uint64_t pts;
    bool isIDR;
};

// Encoding technology, as chosen by the "Encoder technology" setting
// (video.encoder_config.linux_gpu.encoder_backend). Values match the Rust
// EncoderBackend enum.
enum class EncoderBackend : unsigned {
    Automatic = 0,
    Vaapi = 1,
    Nvenc = 2,
    VulkanVideo = 3,
    Software = 4,
};

char const* encoderBackendName(EncoderBackend backend);

// Where and how frames are encoded, resolved once per encoder build from the
// settings and the render GPU (see Encoder::resolveTarget).
struct EncoderTarget {
    // Technology to use. Never Automatic once resolved.
    EncoderBackend backend = EncoderBackend::Vaapi;
    // DRM render node of the encoder GPU (VAAPI device path).
    std::string devicePath;
    // Vendor of the encoder GPU.
    Vendor vendor = Vendor::Intel;
    // The encoder GPU is not the render GPU: the renderer copies each frame
    // into host memory (hostPixels) and the encoder uploads it.
    bool crossDevice = false;
    // Set by the Encoder after the renderer exists (crossDevice only).
    void const* hostPixels = nullptr;
    unsigned hostRowPitch = 0;
};

class EncodePipeline {
public:
    struct Timestamp {
        uint64_t gpu = 0;
        uint64_t cpu = 0;
    };

    virtual ~EncodePipeline();

    virtual void PushFrame(uint64_t targetTimestampNs, bool idr) = 0;
    virtual bool GetEncoded(FramePacket& data);
    virtual Timestamp GetTimestamp() { return timestamp; }
    virtual int GetCodec();

    virtual void SetParams(FfiDynamicEncoderParams params);
    static std::unique_ptr<EncodePipeline> Create(
        alvr::VkContext& vk_ctx,
        EncoderTarget const& target,
        VkFrame& input_frame,
        /* VkFrameCtx &vk_frame_ctx,  */ uint32_t width,
        uint32_t height
    );

protected:
    AVCodecContext* encoder_ctx = nullptr; // shall be initialized by child class
    AVPacket* encoder_packet = NULL;
    Timestamp timestamp = {};
};

}
