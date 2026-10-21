// Offscreen GPU integration test. Build separately; never linked into ALVR.
// Produces stereo PPM fixtures and an objective comparison, without SteamVR.
#include "alvr_server/Settings.h"
#include "platform/linux/motion/NativeMotion.hpp"
#include <array>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

const Settings* Settings_Instance() {
    static Settings settings = [] {
        Settings value {};
        std::strcpy(value.m_renderGpu, "NVIDIA");
        return value;
    }();
    return &settings;
}
void Info(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vprintf(format, args);
    va_end(args);
    puts("");
}
void Warn(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
}
void Error(const char* format, ...) {
    va_list args;
    va_start(args, format);
    vfprintf(stderr, format, args);
    va_end(args);
    fputs("\n", stderr);
}

namespace {
constexpr uint32_t Width = 1024, Height = 384, EyeWidth = Width / 2;
constexpr uint64_t First = 1'000'000'000, Tick = 13'888'889;
using Pixels = std::vector<uint8_t>;
uint32_t hash(int x, int y, unsigned eye) {
    uint32_t n = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + eye * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
}
Pixels fixture(int displacement) {
    Pixels pixels(Width * Height * 4);
    for (unsigned y = 0; y < Height; ++y)
        for (unsigned x = 0; x < Width; ++x) {
            unsigned eye = x / EyeWidth;
            float sx = float(int(x % EyeWidth) - (eye ? -displacement : displacement)) / 8.f;
            float sy = float(y) / 8.f;
            int ix = int(std::floor(sx)), iy = int(std::floor(sy));
            float fx = sx - ix, fy = sy - iy;
            auto value
                = [&](int dx, int dy) { return float(32 + hash(ix + dx, iy + dy, eye) % 192); };
            float top = value(0, 0) * (1 - fx) + value(1, 0) * fx;
            float bottom = value(0, 1) * (1 - fx) + value(1, 1) * fx;
            uint8_t luma = uint8_t(top * (1 - fy) + bottom * fy);
            size_t at = (size_t(y) * Width + x) * 4;
            pixels[at] = eye ? luma / 2 : luma;
            pixels[at + 1] = luma;
            pixels[at + 2] = eye ? luma : luma / 2;
            pixels[at + 3] = 255;
        }
    return pixels;
}
void writePpm(std::filesystem::path const& path, Pixels const& pixels) {
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << Width << ' ' << Height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
        file.write(reinterpret_cast<char const*>(&pixels[i]), 3);
    if (!file)
        throw std::runtime_error("Cannot write PPM fixture");
}
struct Images {
    alvr::VkContext& ctx;
    vk::Image image {};
    vk::ImageView view {};
    vk::DeviceMemory memory {}, stagingMemory {};
    vk::Buffer staging {};
    vk::CommandPool pool {};
    vk::CommandBuffer command {};
    vk::Fence fence {};
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    void* mapped = nullptr;
    explicit Images(alvr::VkContext& c)
        : ctx(c) { }
    ~Images() {
        try {
            ctx.dev.waitIdle();
        } catch (...) {
            return;
        }
        if (mapped)
            ctx.dev.unmapMemory(stagingMemory);
        ctx.dev.destroy(view);
        ctx.dev.destroy(image);
        ctx.dev.free(memory);
        ctx.dev.destroy(staging);
        ctx.dev.free(stagingMemory);
        ctx.dev.destroy(fence);
        ctx.dev.destroy(pool);
    }
    uint32_t memoryType(uint32_t bits, vk::MemoryPropertyFlags flags) {
        auto properties = ctx.physDev.getMemoryProperties();
        for (uint32_t i = 0; i < properties.memoryTypeCount; ++i)
            if ((bits & (1u << i)) && (properties.memoryTypes[i].propertyFlags & flags) == flags)
                return i;
        throw std::runtime_error("No fixture memory type");
    }
    void initialize() {
        image = ctx.dev.createImage(vk::ImageCreateInfo {
            .imageType = vk::ImageType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .extent = { Width, Height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = vk::ImageUsageFlagBits::eTransferDst | vk::ImageUsageFlagBits::eTransferSrc
                | vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eStorage,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        auto req = ctx.dev.getImageMemoryRequirements(image);
        memory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
            .memoryTypeIndex
            = memoryType(req.memoryTypeBits, vk::MemoryPropertyFlagBits::eDeviceLocal),
        });
        ctx.dev.bindImageMemory(image, memory, 0);
        view = ctx.dev.createImageView(vk::ImageViewCreateInfo {
            .image = image,
            .viewType = vk::ImageViewType::e2D,
            .format = vk::Format::eR8G8B8A8Unorm,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        });
        staging = ctx.dev.createBuffer(vk::BufferCreateInfo {
            .size = Width * Height * 4,
            .usage = vk::BufferUsageFlagBits::eTransferSrc | vk::BufferUsageFlagBits::eTransferDst,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        req = ctx.dev.getBufferMemoryRequirements(staging);
        stagingMemory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
            .memoryTypeIndex = memoryType(
                req.memoryTypeBits,
                vk::MemoryPropertyFlagBits::eHostVisible | vk::MemoryPropertyFlagBits::eHostCoherent
            ),
        });
        ctx.dev.bindBufferMemory(staging, stagingMemory, 0);
        mapped = ctx.dev.mapMemory(stagingMemory, 0, VK_WHOLE_SIZE);
        pool = ctx.dev.createCommandPool(vk::CommandPoolCreateInfo {
            .flags = vk::CommandPoolCreateFlagBits::eResetCommandBuffer,
            .queueFamilyIndex = ctx.meta.queueFamily,
        });
        command = ctx.dev.allocateCommandBuffers(vk::CommandBufferAllocateInfo {
            .commandPool = pool,
            .level = vk::CommandBufferLevel::ePrimary,
            .commandBufferCount = 1,
        })[0];
        fence = ctx.dev.createFence({});
    }
    Pixels transfer(Pixels const* input) {
        if (input)
            std::memcpy(mapped, input->data(), input->size());
        command.reset();
        command.begin(vk::CommandBufferBeginInfo {});
        auto transition = [&](vk::ImageLayout after) {
            vk::ImageMemoryBarrier barrier {
                .srcAccessMask = layout == vk::ImageLayout::eUndefined
                    ? vk::AccessFlags {}
                    : vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
                .oldLayout = layout,
                .newLayout = after,
                .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
                .image = image,
                .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
            };
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eAllCommands,
                vk::PipelineStageFlagBits::eAllCommands,
                {},
                {},
                {},
                barrier
            );
            layout = after;
        };
        transition(
            input ? vk::ImageLayout::eTransferDstOptimal : vk::ImageLayout::eTransferSrcOptimal
        );
        vk::BufferImageCopy region {
            .imageSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .imageExtent = { Width, Height, 1 },
        };
        if (input)
            command.copyBufferToImage(staging, image, layout, region);
        else
            command.copyImageToBuffer(image, layout, staging, region);
        transition(vk::ImageLayout::eGeneral);
        if (!input) {
            vk::MemoryBarrier barrier {
                .srcAccessMask = vk::AccessFlagBits::eTransferWrite,
                .dstAccessMask = vk::AccessFlagBits::eHostRead,
            };
            command.pipelineBarrier(
                vk::PipelineStageFlagBits::eTransfer,
                vk::PipelineStageFlagBits::eHost,
                {},
                barrier,
                {},
                {}
            );
        }
        command.end();
        ctx.dev.resetFences(fence);
        vk::SubmitInfo submit { .commandBufferCount = 1, .pCommandBuffers = &command };
        ctx.useQueue([&](auto& queue) { queue.submit(submit, fence); });
        if (ctx.dev.waitForFences(fence, true, 1'000'000'000) != vk::Result::eSuccess)
            throw std::runtime_error("Fixture transfer timeout");
        Pixels result(Width * Height * 4);
        if (!input)
            std::memcpy(result.data(), mapped, result.size());
        return result;
    }
};
double error(Pixels const& got, Pixels const& expected, unsigned eye) {
    double sum = 0;
    size_t count = 0;
    for (unsigned y = 32; y + 32 < Height; ++y)
        for (unsigned x = 32; x + 32 < EyeWidth; ++x)
            for (unsigned channel = 0; channel < 3; ++channel) {
                size_t i = (size_t(y) * Width + eye * EyeWidth + x) * 4 + channel;
                sum += std::abs(int(got[i]) - int(expected[i]));
                ++count;
            }
    return sum / count;
}
}
int main(int argc, char** argv) {
    try {
        if (argc != 2)
            throw std::runtime_error(
                "Usage: ALVR_NATIVE_MOTION=1 linux_native_motion_images <output-directory>"
            );
        std::filesystem::path output(argv[1]);
        std::filesystem::create_directories(output);
        alvr::VkContext ctx(std::vector<uint8_t> {});
        if (!ctx.meta.opticalFlowEnabled || ctx.meta.vendor != alvr::Vendor::Nvidia)
            throw std::runtime_error(
                "Fixture requires NVIDIA optical-flow context; check environment"
            );
        bool passed = true;
        {
            Images gpu(ctx);
            gpu.initialize();
            alvr::motion::NativeMotion motion(
                ctx, vk::Extent2D { Width, Height }, vk::Format::eR8G8B8A8Unorm
            );
            auto first = fixture(0), latest = fixture(8), expected = fixture(12);
            gpu.transfer(&first);
            if (motion.predict(gpu.image, gpu.view, gpu.layout, First))
                throw std::runtime_error("Prediction before history");
            motion.capture(gpu.image, gpu.layout, First);
            if (motion.predict(gpu.image, gpu.view, gpu.layout, First + Tick))
                throw std::runtime_error("Prediction before second real frame");
            motion.capture(gpu.image, gpu.layout, First + Tick);
            gpu.transfer(&latest);
            if (motion.predict(gpu.image, gpu.view, gpu.layout, First + 2 * Tick))
                throw std::runtime_error("Prediction on capture tick");
            motion.capture(gpu.image, gpu.layout, First + 2 * Tick);
            if (!motion.predict(gpu.image, gpu.view, gpu.layout, First + 3 * Tick))
                throw std::runtime_error("Missing half-interval prediction");
            auto predicted = gpu.transfer(nullptr);
            std::ofstream metrics(output / "metrics.txt");
            for (unsigned eye = 0; eye < 2; ++eye) {
                double baseline = error(latest, expected, eye),
                       synthesis = error(predicted, expected, eye);
                bool improved = baseline > 2.5 && synthesis < baseline * 0.85;
                metrics << "eye=" << eye << " repeated_mae=" << baseline
                        << " predicted_mae=" << synthesis << " improved=" << improved << '\n';
                passed &= improved;
            }
            metrics << "passed=" << passed << '\n';
            writePpm(output / "previous.ppm", first);
            writePpm(output / "latest.ppm", latest);
            writePpm(output / "predicted.ppm", predicted);
            writePpm(output / "expected.ppm", expected);
        }
        ctx.destroy();
        std::cout << (passed ? "PASS\n" : "FAIL (prediction did not improve both eyes)\n");
        return passed ? 0 : 1;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
