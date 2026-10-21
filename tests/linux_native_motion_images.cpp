// Offscreen GPU integration test. Build separately; never linked into ALVR.
// Produces stereo PPM fixtures and an objective comparison, without SteamVR.
#include "alvr_server/Settings.h"
#include "native_motion_fixtures.hpp"
#include "platform/linux/motion/NativeMotion.hpp"
#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iomanip>
#include <iostream>
#include <limits>

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
constexpr uint64_t First = 1'000'000'000, Tick = 13'888'889;
using fixture::Pixels;
using fixture::Scene;
using fixture::Size;
void writePpm(std::filesystem::path const& path, Pixels const& pixels, Size size) {
    std::ofstream file(path, std::ios::binary);
    file << "P6\n" << size.width() << ' ' << size.height << "\n255\n";
    for (size_t i = 0; i < pixels.size(); i += 4)
        file.write(reinterpret_cast<char const*>(&pixels[i]), 3);
    if (!file)
        throw std::runtime_error("Cannot write PPM fixture");
}
struct Images {
    alvr::VkContext& ctx;
    Size size;
    vk::Image image {};
    vk::ImageView view {};
    vk::DeviceMemory memory {}, stagingMemory {};
    vk::Buffer staging {};
    vk::CommandPool pool {};
    vk::CommandBuffer command {};
    vk::Fence fence {};
    vk::ImageLayout layout = vk::ImageLayout::eUndefined;
    void* mapped = nullptr;
    explicit Images(alvr::VkContext& c, Size dimensions)
        : ctx(c)
        , size(dimensions) { }
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
            .extent = { size.width(), size.height, 1 },
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
            .size = size.bytes(),
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
            .imageExtent = { size.width(), size.height, 1 },
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
        Pixels result(size.bytes());
        if (!input)
            std::memcpy(result.data(), mapped, result.size());
        return result;
    }
};

struct Options {
    std::filesystem::path output;
    std::string suite = "smoke";
    Size size;
    unsigned iterations = 1;
    bool saveImages = false;
    double maxPredictMs = 0;
};
Options parse(int argc, char** argv) {
    if (argc < 2)
        throw std::runtime_error("Usage: image_test OUTPUT [--suite smoke|quality|stream] "
                                 "[--iterations N] [--save-images] [--max-predict-ms N]");
    Options o;
    o.output = argv[1];
    unsigned requestedIterations = 0;
    for (int i = 2; i < argc; ++i) {
        std::string arg = argv[i];
        auto value = [&]() -> std::string {
            if (++i == argc)
                throw std::runtime_error("Missing value for " + arg);
            return argv[i];
        };
        if (arg == "--suite")
            o.suite = value();
        else if (arg == "--iterations") {
            std::string text = value();
            size_t end;
            auto number = std::stoul(text, &end);
            if (end != text.size() || number < 1 || number > 120)
                throw std::runtime_error("Iterations must be an integer in [1,120]");
            requestedIterations = unsigned(number);
        } else if (arg == "--save-images")
            o.saveImages = true;
        else if (arg == "--max-predict-ms") {
            std::string text = value();
            size_t end;
            o.maxPredictMs = std::stod(text, &end);
            if (end != text.size() || !std::isfinite(o.maxPredictMs) || o.maxPredictMs <= 0)
                throw std::runtime_error("Prediction budget must be positive and finite");
        } else
            throw std::runtime_error("Unknown option: " + arg);
    }
    if (o.suite == "smoke") {
        o.iterations = 1;
        o.saveImages = true;
    } else if (o.suite == "quality")
        o.iterations = 6;
    else if (o.suite == "stream") {
        o.size = { 2048, 2144 };
        o.iterations = 12;
    } else
        throw std::runtime_error("Unknown suite: " + o.suite);
    if (requestedIterations)
        o.iterations = requestedIterations;
    // Reject reuse so an earlier result cannot be confused with this run.
    if (std::filesystem::exists(o.output / "summary.txt"))
        throw std::runtime_error(
            "Result directory already contains summary.txt; use a new directory"
        );
    std::filesystem::create_directories(o.output);
    return o;
}
using Clock = std::chrono::steady_clock;
double elapsed(Clock::time_point start) {
    return std::chrono::duration<double, std::milli>(Clock::now() - start).count();
}
double percentile(std::vector<double> values, double fraction) {
    if (values.empty())
        return 0;
    std::sort(values.begin(), values.end());
    return values[size_t(std::ceil(fraction * values.size())) - 1];
}
void require(bool condition, char const* message) {
    if (!condition)
        throw std::runtime_error(message);
}
Pixels prime(alvr::motion::NativeMotion& motion, Images& gpu, Scene scene) {
    auto first = fixture::render(gpu.size, scene, 0);
    auto latest = fixture::render(gpu.size, scene, 2);
    for (unsigned frame = 0; frame < 3; ++frame) {
        auto pixels = frame == 0 ? first
            : frame == 2         ? latest
                                 : fixture::render(gpu.size, scene, 1);
        gpu.transfer(&pixels);
        unsigned releases = 0;
        require(
            !motion.predict(
                gpu.image, gpu.view, gpu.layout, First + frame * Tick, [&] { ++releases; }
            ),
            "Prediction during warm-up/capture"
        );
        require(releases == 0, "Input released on a real presentation");
        motion.capture(gpu.image, gpu.layout, First + frame * Tick);
    }
    return latest;
}
void timestampRecovery(alvr::VkContext& ctx, std::ofstream& summary) {
    // Fixed small images: these are state/lifetime checks, not budget samples.
    Images gpu(ctx, { 512, 384 });
    gpu.initialize();
    std::array<uint64_t, 4> targets {
        First + 2 * Tick, First + 2 * Tick - 1, First + 4 * Tick, First + 1'000'000'000
    };
    for (auto target : targets) {
        alvr::motion::NativeMotion motion(
            ctx, { gpu.size.width(), gpu.size.height }, vk::Format::eR8G8B8A8Unorm
        );
        auto latest = prime(motion, gpu, Scene::Translation);
        unsigned releases = 0;
        require(
            !motion.predict(gpu.image, gpu.view, gpu.layout, target, [&] { ++releases; }),
            "Invalid timestamp predicted"
        );
        require(releases == 0, "Rejected prediction released the current real input");
        require(gpu.transfer(nullptr) == latest, "Rejected prediction modified output");
        motion.capture(gpu.image, gpu.layout, target);
        // Re-anchor on the fallback frame, skip one warm-up presentation, then
        // retain another real frame two ticks later and resume prediction.
        for (unsigned frame = 1; frame <= 2; ++frame) {
            auto pixels = fixture::render(gpu.size, Scene::Translation, double(frame));
            gpu.transfer(&pixels);
            require(
                !motion.predict(gpu.image, gpu.view, gpu.layout, target + frame * Tick),
                "Prediction before replacement history was ready"
            );
            motion.capture(gpu.image, gpu.layout, target + frame * Tick);
        }
        require(
            motion.predict(gpu.image, gpu.view, gpu.layout, target + 3 * Tick, [&] { ++releases; }),
            "Prediction did not recover after rejection"
        );
        require(releases == 1, "Recovered prediction must release input exactly once");
        summary << "timestamp_recovery target=" << target << " passed=1\n";
        summary.flush();
    }
}
bool runCase(
    alvr::VkContext& ctx,
    Options const& o,
    Scene scene,
    std::ofstream& metrics,
    std::ofstream& timings,
    std::ofstream& summary
) {
    Images gpu(ctx, o.size);
    gpu.initialize();
    alvr::motion::NativeMotion motion(
        ctx, { o.size.width(), o.size.height }, vk::Format::eR8G8B8A8Unorm
    );
    auto latest = prime(motion, gpu, scene);
    bool quality = true;
    std::vector<double> predictionMs, captureMs, pairMs, flowMs, warpMs;
    for (unsigned sample = 0; sample < o.iterations; ++sample) {
        unsigned realFrame = 2 + sample * 2;
        double captureDuration = 0;
        if (sample) {
            latest = fixture::render(o.size, scene, double(realFrame));
            gpu.transfer(&latest);
            require(
                !motion.predict(gpu.image, gpu.view, gpu.layout, First + realFrame * Tick),
                "Prediction on a real capture presentation"
            );
            auto start = Clock::now();
            motion.capture(gpu.image, gpu.layout, First + realFrame * Tick);
            captureDuration = elapsed(start);
        }
        // Jitter is deliberately inside the documented half-interval tolerance.
        int64_t jitter = scene == Scene::Jitter ? (int(sample % 3) - 1) * 250'000 : 0;
        uint64_t target = uint64_t(int64_t(First + (realFrame + 1) * Tick) + jitter);
        double targetTime = double(target - First) / double(Tick);
        auto expected = fixture::render(o.size, scene, targetTime);
        unsigned releases = 0;
        auto start = Clock::now();
        bool predicted
            = motion.predict(gpu.image, gpu.view, gpu.layout, target, [&] { ++releases; });
        double predictionDuration = elapsed(start);
        require(predicted, "Missing alternating prediction");
        auto stages = motion.lastPredictionTiming();
        require(releases == 1, "Prediction must release input exactly once");
        auto image = gpu.transfer(nullptr);
        bool sampleQuality = true;
        for (unsigned eye = 0; eye < 2; ++eye) {
            auto result = fixture::score(
                o.size, scene, latest, image, expected, eye, double(realFrame), targetTime
            );
            sampleQuality &= result.improved();
            require(result.validPixels, "Invalid alpha or color leaked between stereo eyes");
            auto row = [&](char const* region, fixture::Error const& error) {
                metrics << fixture::name(scene) << ',' << sample << ',' << eye << ',' << region
                        << ',' << error.pixels << ',' << error.baselineMae() << ','
                        << error.predictionMae() << ',' << result.improved() << ','
                        << result.staticMismatches << '\n';
            };
            row("all", result.all);
            row("changed", result.changed);
            if (scene == Scene::Foreground)
                row("revealed", result.revealed);
        }
        quality &= sampleQuality;
        bool cold = sample == 0;
        timings << fixture::name(scene) << ',' << sample << ',' << cold << ',' << predictionDuration
                << ',' << captureDuration << ',' << predictionDuration + captureDuration << ','
                << sampleQuality << ',' << stages.flowMs << ',' << stages.warpMs << '\n';
        metrics.flush();
        timings.flush();
        // First prediction is cold; do not hide it in a steady-state percentile.
        if (!cold || o.iterations == 1) {
            predictionMs.push_back(predictionDuration);
            flowMs.push_back(stages.flowMs);
            warpMs.push_back(stages.warpMs);
            if (!cold) {
                captureMs.push_back(captureDuration);
                pairMs.push_back(predictionDuration + captureDuration);
            }
        }
        if (sample == 0 && o.saveImages) {
            auto folder = o.output / fixture::name(scene);
            std::filesystem::create_directories(folder);
            writePpm(folder / "previous.ppm", fixture::render(o.size, scene, 0), o.size);
            writePpm(folder / "latest.ppm", latest, o.size);
            writePpm(folder / "expected.ppm", expected, o.size);
            writePpm(folder / "predicted.ppm", image, o.size);
        }
    }
    double worst = *std::max_element(predictionMs.begin(), predictionMs.end());
    bool budget = !o.maxPredictMs || worst <= o.maxPredictMs;
    summary << "case=" << fixture::name(scene) << " quality_pass=" << quality
            << " budget_pass=" << budget << " steady_samples=" << predictionMs.size()
            << " predict_p50_ms=" << percentile(predictionMs, .5)
            << " predict_p95_ms=" << percentile(predictionMs, .95) << " predict_max_ms=" << worst
            << " capture_p95_ms=" << percentile(captureMs, .95)
            << " pair_p95_ms=" << percentile(pairMs, .95)
            << " flow_wall_p95_ms=" << percentile(flowMs, .95)
            << " warp_wall_p95_ms=" << percentile(warpMs, .95) << '\n';
    summary.flush();
    std::cout << fixture::name(scene) << " quality=" << quality << " budget=" << budget
              << " predict_p95_ms=" << percentile(predictionMs, .95) << '\n';
    return quality && budget;
}
}
int main(int argc, char** argv) {
    try {
        auto o = parse(argc, argv);
        std::ofstream summary(o.output / "summary.txt"), metrics(o.output / "metrics.csv"),
            timings(o.output / "timings.csv");
        if (!summary || !metrics || !timings)
            throw std::runtime_error("Cannot create result files");
        summary << std::setprecision(8) << "suite=" << o.suite << " eye_width=" << o.size.eyeWidth
                << " height=" << o.size.height << " iterations=" << o.iterations
                << " max_predict_ms=" << o.maxPredictMs << '\n';
        summary << "timing=CPU_wall_including_GPU_completion_excluding_upload_readback_encoder\n"
                << "first_sample=cold_excluded_from_steady_unless_only_sample\n";
        summary.flush();
        metrics << "case,sample,eye,region,pixels,repeated_mae,predicted_mae,quality_pass,"
                   "static_pixel_mismatches\n";
        timings << "case,sample,cold,predict_ms,capture_ms,pair_ms,quality_pass,flow_wall_ms,"
                   "warp_wall_ms\n";
        alvr::VkContext ctx(std::vector<uint8_t> {});
        if (!ctx.meta.opticalFlowEnabled || ctx.meta.vendor != alvr::Vendor::Nvidia)
            throw std::runtime_error(
                "Fixture requires NVIDIA optical-flow context; check environment"
            );
        bool passed = true;
        std::vector<Scene> scenes { Scene::Translation };
        if (o.suite != "smoke")
            scenes = { Scene::Stationary,   Scene::Translation,  Scene::SlowTranslation,
                       Scene::SlowVertical, Scene::SlowDiagonal, Scene::FineTexture,
                       Scene::Rotation,     Scene::Foreground,   Scene::Jitter };
        for (auto scene : scenes)
            passed &= runCase(ctx, o, scene, metrics, timings, summary);
        if (o.suite != "smoke")
            timestampRecovery(ctx, summary);
        ctx.destroy();
        summary << "passed=" << passed << '\n';
        if (!summary || !metrics || !timings)
            throw std::runtime_error("Result write failed");
        std::cout << (passed ? "PASS\n" : "FAIL (quality or requested budget gate)\n");
        return passed ? 0 : 1;
    } catch (std::exception const& error) {
        std::cerr << error.what() << '\n';
        return 1;
    }
}
