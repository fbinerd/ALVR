#include "NvencHostInput.h"
#include "alvr_server/Logger.h"

#include <algorithm>
#include <chrono>
#include <climits>
#include <filesystem>
#include <stdexcept>
#include <sys/stat.h>
#include <sys/sysmacros.h>

// These are the same dynamically loaded CUDA declarations used by FFmpeg.
// No CUDA SDK or link-time dependency on NVIDIA's driver is required.
#include <ffnvcodec/dynlink_loader.h>
extern "C" {
#include <libavutil/error.h>
#include <libavutil/frame.h>
#include <libavutil/hwcontext_cuda.h>
}

namespace {
void checkAv(int result, char const* operation) {
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE] = {};
        av_strerror(result, error, sizeof(error));
        throw std::runtime_error(std::string(operation) + ": " + error);
    }
}

AVPixelFormat surfaceFormat(VkFormat format) {
    switch (format) {
    case VK_FORMAT_R8G8B8A8_UNORM:
    case VK_FORMAT_R8G8B8A8_SRGB:
        return AV_PIX_FMT_RGB0;
    case VK_FORMAT_B8G8R8A8_UNORM:
    case VK_FORMAT_B8G8R8A8_SRGB:
        return AV_PIX_FMT_BGR0;
    default:
        throw std::runtime_error("Unsupported NVENC host input format");
    }
}
}

struct alvr::NvencHostInput::Impl {
    CudaFunctions* cuda = nullptr;
    AVBufferRef* device = nullptr;
    AVFrame* source = nullptr;
    uint64_t uploads = 0;
    double totalMs = 0, maxMs = 0;
    std::chrono::steady_clock::time_point lastReport = std::chrono::steady_clock::now();

    ~Impl() {
        av_frame_free(&source);
        av_buffer_unref(&device);
        cuda_free_functions(&cuda);
    }

    void check(CUresult result, char const* operation) {
        if (result != CUDA_SUCCESS) {
            char const* error = "unknown CUDA error";
            cuda->cuGetErrorName(result, &error);
            throw std::runtime_error(std::string(operation) + ": " + error);
        }
    }
};

alvr::NvencHostInput::NvencHostInput(
    std::string const& renderNode,
    void const* pixels,
    unsigned rowPitch,
    VkFormat pixelFormat,
    unsigned width,
    unsigned height
)
    : impl(std::make_unique<Impl>()) {
    // Validate before loading CUDA, including int-sized FFmpeg line strides.
    auto format = surfaceFormat(pixelFormat);
    if (!pixels || width == 0 || height == 0 || width > INT_MAX / 4 || height > INT_MAX
        || rowPitch < width * 4 || rowPitch > INT_MAX) {
        throw std::runtime_error("Invalid NVENC host input dimensions, pointer or row pitch");
    }
    struct stat node { };
    if (stat(renderNode.c_str(), &node) != 0 || !S_ISCHR(node.st_mode)) {
        throw std::runtime_error("NVENC encoder render node is not a character device");
    }
    auto deviceLink = std::filesystem::path("/sys/dev/char")
        / (std::to_string(major(node.st_rdev)) + ":" + std::to_string(minor(node.st_rdev)))
        / "device";
    auto pci = std::filesystem::canonical(deviceLink).filename().string();

    if (cuda_load_functions(&impl->cuda, nullptr) < 0)
        throw std::runtime_error("Could not load CUDA driver for NVENC host upload");
    impl->check(impl->cuda->cuInit(0), "cuInit");
    CUdevice ordinal;
    impl->check(
        impl->cuda->cuDeviceGetByPCIBusId(&ordinal, pci.c_str()),
        "Selected encoder GPU is not available to CUDA"
    );
    auto selector = std::to_string(ordinal);
    checkAv(
        av_hwdevice_ctx_create(&impl->device, AV_HWDEVICE_TYPE_CUDA, selector.c_str(), nullptr, 0),
        "Creating independent NVIDIA CUDA device"
    );
    impl->source = av_frame_alloc();
    if (!impl->source)
        throw std::runtime_error("Could not allocate NVENC host frame");
    impl->source->format = format;
    impl->source->width = width;
    impl->source->height = height;
    impl->source->data[0] = static_cast<uint8_t*>(const_cast<void*>(pixels));
    impl->source->linesize[0] = rowPitch;
    InfoToDriver(
        "[gpu-diag] NVENC host upload: %s, PCI %s, CUDA device %d, format %d, %ux%u, pitch %u",
        renderNode.c_str(),
        pci.c_str(),
        ordinal,
        (int)format,
        width,
        height,
        rowPitch
    );
}

alvr::NvencHostInput::~NvencHostInput() = default;

AVBufferRef* alvr::NvencHostInput::device() const { return impl->device; }
AVPixelFormat alvr::NvencHostInput::format() const { return AVPixelFormat(impl->source->format); }

void alvr::NvencHostInput::upload(AVFrame* destination) {
    if (!destination || !destination->hw_frames_ctx)
        throw std::runtime_error("NVENC host upload has no CUDA destination");
    auto* frames = reinterpret_cast<AVHWFramesContext*>(destination->hw_frames_ctx->data);
    if (frames->format != AV_PIX_FMT_CUDA || frames->sw_format != format()
        || frames->width != impl->source->width || frames->height != impl->source->height
        || frames->device_ctx != reinterpret_cast<AVHWDeviceContext*>(impl->device->data)) {
        throw std::runtime_error("NVENC host upload destination does not match its source/device");
    }
    auto start = std::chrono::steady_clock::now();
    int result = av_hwframe_transfer_data(destination, impl->source, 0);
    // FFmpeg's host-to-CUDA transfer may be asynchronous. Complete it even
    // after a partial failure, before the next render overwrites staging.
    auto* context = static_cast<AVCUDADeviceContext*>(frames->device_ctx->hwctx);
    impl->check(impl->cuda->cuCtxPushCurrent(context->cuda_ctx), "Pushing CUDA upload context");
    auto synchronized = impl->cuda->cuStreamSynchronize(context->stream);
    CUcontext previous;
    auto popped = impl->cuda->cuCtxPopCurrent(&previous);
    impl->check(synchronized, "Completing NVENC host upload");
    impl->check(popped, "Restoring CUDA context");
    checkAv(result, "Uploading renderer frame to NVENC");

    auto now = std::chrono::steady_clock::now();
    double ms = std::chrono::duration<double, std::milli>(now - start).count();
    impl->uploads++;
    impl->totalMs += ms;
    impl->maxMs = std::max(impl->maxMs, ms);
    if (now - impl->lastReport >= std::chrono::seconds(10)) {
        InfoToDriver(
            "[gpu-diag] NVENC host upload: %llu frames, average %.3f ms, max %.3f ms",
            (unsigned long long)impl->uploads,
            impl->totalMs / impl->uploads,
            impl->maxMs
        );
        impl->uploads = 0;
        impl->totalMs = impl->maxMs = 0;
        impl->lastReport = now;
    }
}
