#pragma once

#include <memory>
#include <string>
#include <vulkan/vulkan_core.h>

extern "C" {
#include <libavutil/hwcontext.h>
}

namespace alvr {

// Uploads completed renderer staging memory to an independently selected
// CUDA device. upload() completes before the renderer may reuse that memory.
class NvencHostInput {
public:
    NvencHostInput(
        std::string const& renderNode,
        void const* pixels,
        unsigned rowPitch,
        VkFormat format,
        unsigned width,
        unsigned height
    );
    ~NvencHostInput();

    AVBufferRef* device() const;
    AVPixelFormat format() const;
    void upload(AVFrame* destination);

private:
    struct Impl;
    std::unique_ptr<Impl> impl;
};

}
