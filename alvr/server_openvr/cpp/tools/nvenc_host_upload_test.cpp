// Standalone hardware check: build with NvencHostInput.cpp and the checkout's
// FFmpeg libraries. Run with a NVIDIA DRM render node; no SteamVR is started.
#include "../platform/linux/NvencHostInput.h"
#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <stdexcept>
#include <vector>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavutil/error.h>
#include <libavutil/opt.h>
}

void InfoToDriver(char const* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    puts("");
}

static void require(bool ok, char const* message) {
    if (!ok)
        throw std::runtime_error(message);
}
static void check(int result) {
    if (result < 0) {
        char error[AV_ERROR_MAX_STRING_SIZE];
        av_strerror(result, error, sizeof(error));
        throw std::runtime_error(error);
    }
}
static AVBufferRef* pool(alvr::NvencHostInput& input, unsigned width, unsigned height) {
    auto* ref = av_hwframe_ctx_alloc(input.device());
    require(ref, "CUDA frame pool allocation failed");
    auto* ctx = reinterpret_cast<AVHWFramesContext*>(ref->data);
    ctx->format = AV_PIX_FMT_CUDA;
    ctx->sw_format = input.format();
    ctx->width = width;
    ctx->height = height;
    check(av_hwframe_ctx_init(ref));
    return ref;
}

static void invalidInputs() {
    uint8_t pixel[4] = {};
    for (int test = 0; test < 5; test++) {
        bool rejected = false;
        try {
            alvr::NvencHostInput input(
                "/missing-node",
                test == 0 ? nullptr : pixel,
                test == 1 ? 3 : 4,
                test == 2 ? VK_FORMAT_R16_SFLOAT : VK_FORMAT_R8G8B8A8_UNORM,
                test == 3 ? 0 : 1,
                1
            );
        } catch (std::exception const&) {
            rejected = true;
        }
        require(rejected, "Invalid input accepted");
    }
}

static void
roundtrip(char const* node, VkFormat format, unsigned width, unsigned height, unsigned padding) {
    unsigned pitch = width * 4 + padding;
    std::vector<uint8_t> pixels(size_t(pitch) * height);
    alvr::NvencHostInput input(node, pixels.data(), pitch, format, width, height);
    auto* frames = pool(input, width, height);
    auto* gpu = av_frame_alloc();
    auto* cpu = av_frame_alloc();
    for (unsigned generation = 0; generation < 3; generation++) {
        for (size_t i = 0; i < pixels.size(); i++)
            pixels[i] = uint8_t(i * 31 + generation * 73);
        auto expected = pixels;
        check(av_hwframe_get_buffer(frames, gpu, 0));
        input.upload(gpu);
        // The caller may reuse staging as soon as upload returns.
        std::fill(pixels.begin(), pixels.end(), 0xcc);
        check(av_hwframe_transfer_data(cpu, gpu, 0));
        for (unsigned row = 0; row < height; row++) {
            require(
                memcmp(
                    cpu->data[0] + size_t(row) * cpu->linesize[0],
                    expected.data() + size_t(row) * pitch,
                    width * 4
                ) == 0,
                "CUDA roundtrip changed pixels or read reused staging"
            );
        }
        av_frame_unref(cpu);
        av_frame_unref(gpu);
    }
    av_frame_free(&cpu);
    av_frame_free(&gpu);
    av_buffer_unref(&frames);
    printf(
        "PASS exact upload/download: format=%d %ux%u pitch=%u, 3 generations\n",
        format,
        width,
        height,
        pitch
    );
}

static void encodeColors(char const* node, bool bgra) {
    constexpr int width = 256, height = 64, pitch = width * 4 + 32;
    std::vector<uint8_t> pixels(pitch * height);
    alvr::NvencHostInput input(
        node,
        pixels.data(),
        pitch,
        bgra ? VK_FORMAT_B8G8R8A8_UNORM : VK_FORMAT_R8G8B8A8_UNORM,
        width,
        height
    );
    auto* frames = pool(input, width, height);
    auto* encoder = avcodec_alloc_context3(avcodec_find_encoder_by_name("h264_nvenc"));
    require(encoder, "h264_nvenc is unavailable");
    encoder->width = width;
    encoder->height = height;
    encoder->pix_fmt = AV_PIX_FMT_CUDA;
    encoder->time_base = AVRational { 1, 72 };
    encoder->framerate = AVRational { 72, 1 };
    encoder->bit_rate = 8'000'000;
    encoder->max_b_frames = 0;
    encoder->gop_size = 1;
    encoder->hw_frames_ctx = av_buffer_ref(frames);
    av_opt_set(encoder->priv_data, "preset", "p1", 0);
    av_opt_set(encoder->priv_data, "tune", "ull", 0);
    av_opt_set_int(encoder->priv_data, "delay", 0, 0);
    check(avcodec_open2(encoder, encoder->codec, nullptr));
    auto* decoder = avcodec_alloc_context3(avcodec_find_decoder(AV_CODEC_ID_H264));
    require(decoder, "H264 decoder is unavailable");
    check(avcodec_open2(decoder, decoder->codec, nullptr));
    auto* gpu = av_frame_alloc();
    auto* decoded = av_frame_alloc();
    auto* packet = av_packet_alloc();
    int decodedCount = 0;
    auto drain = [&] {
        int result;
        while ((result = avcodec_receive_packet(encoder, packet)) >= 0) {
            check(avcodec_send_packet(decoder, packet));
            av_packet_unref(packet);
            int received;
            while ((received = avcodec_receive_frame(decoder, decoded)) >= 0) {
                auto fmt = AVPixelFormat(decoded->format);
                bool subsampled = fmt == AV_PIX_FMT_YUV420P || fmt == AV_PIX_FMT_YUVJ420P;
                require(
                    subsampled || fmt == AV_PIX_FMT_YUV444P || fmt == AV_PIX_FMT_YUVJ444P,
                    "Unexpected NVENC decoded pixel format"
                );
                for (int region = 0; region < 3; region++) {
                    int x = (region * 2 + 1) * width / 6;
                    int y = height / 2;
                    int shift = subsampled ? 1 : 0;
                    double luma = decoded->data[0][y * decoded->linesize[0] + x];
                    double u = decoded->data[1][(y >> shift) * decoded->linesize[1] + (x >> shift)]
                        - 128.;
                    double v = decoded->data[2][(y >> shift) * decoded->linesize[2] + (x >> shift)]
                        - 128.;
                    bool full = decoded->color_range == AVCOL_RANGE_JPEG;
                    double base = full ? luma : (luma - 16) * 1.164;
                    double sample[] = { base + (full ? 1.402 : 1.596) * v,
                                        base - (full ? .344 : .392) * u - (full ? .714 : .813) * v,
                                        base + (full ? 1.772 : 2.017) * u };
                    int expected[3] = { 20, 20, 20 };
                    expected[region] = 160 + decodedCount * 30;
                    for (int channel = 0; channel < 3; channel++)
                        require(
                            std::abs(int(sample[channel]) - expected[channel]) < 40,
                            "NVENC decode has wrong colors or frame order"
                        );
                }
                decodedCount++;
                av_frame_unref(decoded);
            }
            require(received == AVERROR(EAGAIN) || received == AVERROR_EOF, "Decode failed");
        }
        require(result == AVERROR(EAGAIN) || result == AVERROR_EOF, "NVENC packet failed");
    };
    for (int generation = 0; generation < 3; generation++) {
        for (int y = 0; y < height; y++) {
            for (int x = 0; x < width; x++) {
                int color[3] = { 20, 20, 20 };
                color[std::min(2, x * 3 / width)] = 160 + generation * 30;
                auto* pixel = &pixels[y * pitch + x * 4];
                pixel[0] = color[bgra ? 2 : 0];
                pixel[1] = color[1];
                pixel[2] = color[bgra ? 0 : 2];
                pixel[3] = 255;
            }
        }
        check(av_hwframe_get_buffer(frames, gpu, 0));
        input.upload(gpu);
        std::fill(pixels.begin(), pixels.end(), 0xcc);
        gpu->pts = generation;
        check(avcodec_send_frame(encoder, gpu));
        av_frame_unref(gpu);
        drain();
    }
    check(avcodec_send_frame(encoder, nullptr));
    drain();
    require(decodedCount == 3, "Wrong decoded frame count");
    av_packet_free(&packet);
    av_frame_free(&gpu);
    av_frame_free(&decoded);
    avcodec_free_context(&decoder);
    avcodec_free_context(&encoder);
    av_buffer_unref(&frames);
    printf("PASS NVENC encode/decode colors and order: %s, 3 frames\n", bgra ? "BGRA" : "RGBA");
}

int main(int argc, char** argv) {
    try {
        invalidInputs();
        puts("PASS invalid pointer, stride, format, dimensions and node rejection");
        if (argc != 2)
            throw std::runtime_error("Usage: nvenc_host_upload_test /dev/dri/renderDxxx");
        for (auto format : { VK_FORMAT_R8G8B8A8_UNORM,
                             VK_FORMAT_B8G8R8A8_UNORM,
                             VK_FORMAT_R8G8B8A8_SRGB,
                             VK_FORMAT_B8G8R8A8_SRGB }) {
            roundtrip(argv[1], format, 257, 33, 0);
            roundtrip(argv[1], format, 257, 33, 60);
        }
        encodeColors(argv[1], false);
        encodeColors(argv[1], true);
        roundtrip(argv[1], VK_FORMAT_B8G8R8A8_UNORM, 4288, 2336, 0);
        puts("PASS all CUDA host upload checks");
        return 0;
    } catch (std::exception const& e) {
        fprintf(stderr, "FAIL: %s\n", e.what());
        return 1;
    }
}
