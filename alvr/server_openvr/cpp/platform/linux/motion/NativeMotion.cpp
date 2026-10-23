#include "NativeMotion.hpp"
#include "NativeMotionShaders.hpp"
#include "NativeMotionTiming.hpp"
#include "NativeOpticalFlow.hpp"

#include <array>
#include <chrono>
#include <cmath>
#include <stdexcept>

namespace alvr::motion {

// Real pictures needed after a reset or pause before gap filling starts.
constexpr unsigned GapFillWarmupPictures = 4;

// Interpolation (mode 9): the target lies between the two real pictures;
// returns its fraction from the older (0) to the newest (1) picture.
inline std::optional<float> interpolationTime(uint64_t previous, uint64_t latest, uint64_t target) {
    if (!previous || latest <= previous || latest - previous > 100'000'000 || target <= previous
        || target >= latest)
        return std::nullopt;
    return float(double(target - previous) / double(latest - previous));
}
namespace {
    constexpr uint64_t TimeoutNs = 500'000'000;
    struct Image {
        vk::Image handle {};
        vk::DeviceMemory memory {};
        vk::ImageView view {};
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        vk::Extent2D extent {};
        vk::Format format {};
        vk::ImageUsageFlags usage {};
    };
    struct WarpPush {
        uint32_t eyeWidth, height, lumaWidth, lumaHeight;
        float time;
        uint32_t rotationMode = 0, eye = 0, flags = 0; // flags bit 0: depth
        float previousQuaternion[4] {0,0,0,1}, targetQuaternion[4] {0,0,0,1};
        float leftTans[4] {}, rightTans[4] {};
        float targetTranslation[4] {}; // xyz metres, see Rotation::translation
        float previousTranslation[4] {}; // history translation, older-from-newest
    };
    static_assert(sizeof(WarpPush) == 128); // the guaranteed push constant size
}

struct NativeMotion::State {
    VkContext& ctx;
    vk::Extent2D extent, lumaExtent;
    vk::Format format;
    std::array<Image, 2> colors;
    // Optional per-picture depth (R32F, producer extent), allocated on first use.
    std::array<Image, 2> depths;
    std::array<bool, 2> depthValid {};
    std::array<std::array<Image, 2>, 2> lumas; // retained frame / eye
    std::array<std::array<Image, 2>, 2> flows; // eye / forward, backward
    std::array<Image, 2> costs, backwardCosts;
    std::array<std::unique_ptr<NativeOpticalFlow>, 2> estimators;
    vk::CommandPool pool {};
    vk::CommandBuffer command {};
    vk::Fence fence {};
    vk::QueryPool warpQueries {};
    uint32_t warpTimestampBits = 0;
    double timestampPeriod = 0;
    vk::Sampler linearSampler {}, nearestSampler {};
    vk::DescriptorSetLayout lumaDescriptors {}, warpDescriptors {};
    vk::PipelineLayout lumaLayout {}, warpLayout {};
    vk::ShaderModule lumaShader {}, warpShader {};
    vk::Pipeline lumaPipeline {}, warpPipeline {};
    std::array<uint64_t, 2> timestamps {};
    std::array<uint64_t, 2> clocks {};
    uint64_t presentations = 0, predictions = 0, captures = 0, rejected = 0;
    unsigned retained = 0, latest = 0;
    bool captureThisFrame = true, flowDirty = false, idle = true, repeatReference = false,
         gapFill = false;
    uint64_t gapRejected = 0, gapNoHistory = 0, historyResets = 0, gapWarmup = 0;
    // Real pictures captured since the last history reset or pause; gap
    // filling waits for a few of them so its first predictions do not use a
    // cadence that has not settled yet (distorted pictures right after
    // connecting).
    unsigned warmCaptures = 0;
    double aheadFlowMs = -1, captureMsSum = 0, captureMsMax = 0;
    bool allowExactFlowExit, bidirectionalFlow;
    PredictionTiming timing;
    double cachedOpticalGpuMs = -1;
    Rotation historyRotation;
    bool rotationMode = false, confidenceFallback = false, stableResidual = false,
         interpolate = false;
    uint32_t diagnosticFlags = 0;
    bool depthOcclusion = false;

    State(
        VkContext& c,
        vk::Extent2D size,
        vk::Format f,
        uint32_t maxLumaDimension,
        bool exactFlowExit,
        bool bothDirections
    )
        : ctx(c)
        , extent(size)
        , format(f)
        , allowExactFlowExit(exactFlowExit)
        , bidirectionalFlow(bothDirections) {
        lumaExtent = vk::Extent2D { (size.width / 4) & ~3u, (size.height / 2) & ~3u };
        if (maxLumaDimension && (maxLumaDimension < 64 || maxLumaDimension % 4))
            throw std::runtime_error(
                "Native motion luma cap must be zero or a multiple of four >=64"
            );
        uint32_t largest = std::max(lumaExtent.width, lumaExtent.height);
        if (maxLumaDimension && largest > maxLumaDimension) {
            lumaExtent.width
                = uint32_t(uint64_t(lumaExtent.width) * maxLumaDimension / largest) & ~3u;
            lumaExtent.height
                = uint32_t(uint64_t(lumaExtent.height) * maxLumaDimension / largest) & ~3u;
        }
    }

    ~State() {
        // A timeout is fatal to this stream. The caller drains the device before
        // teardown; if that cannot be established, retain resources until device
        // destruction instead of freeing images referenced by submitted work.
        bool completed = idle;
        if (!completed) {
            try {
                completed = ctx.dev.waitForFences(fence, true, TimeoutNs) == vk::Result::eSuccess;
            } catch (...) {
                completed = false;
            }
        }
        for (auto const& estimator : estimators) {
            if (!estimator)
                continue;
            std::string error;
            if (!estimator->quiesce(error))
                completed = false;
        }
        if (!completed) {
            Warn("Native motion: pending submission; retaining resources until device teardown");
            for (auto& estimator : estimators)
                (void)estimator.release();
            return;
        }
        for (auto& estimator : estimators)
            estimator.reset();
        auto destroy = [&](Image& image) {
            ctx.dev.destroy(image.view);
            ctx.dev.destroy(image.handle);
            ctx.dev.free(image.memory);
        };
        for (auto& image : colors)
            destroy(image);
        for (auto& image : depths)
            destroy(image);
        for (auto& pair : lumas)
            for (auto& image : pair)
                destroy(image);
        for (auto& pair : flows)
            for (auto& image : pair)
                destroy(image);
        for (auto& image : costs)
            destroy(image);
        for (auto& image : backwardCosts)
            destroy(image);
        ctx.dev.destroy(lumaPipeline);
        ctx.dev.destroy(warpPipeline);
        ctx.dev.destroy(lumaLayout);
        ctx.dev.destroy(warpLayout);
        ctx.dev.destroy(lumaDescriptors);
        ctx.dev.destroy(warpDescriptors);
        ctx.dev.destroy(lumaShader);
        ctx.dev.destroy(warpShader);
        ctx.dev.destroy(linearSampler);
        ctx.dev.destroy(nearestSampler);
        ctx.dev.destroy(fence);
        ctx.dev.destroy(warpQueries);
        ctx.dev.destroy(pool);
    }

    void makeImage(Image& image, vk::Extent2D size, vk::Format f, vk::ImageUsageFlags usage) {
        auto limits = ctx.physDev.getImageFormatProperties(
            f, vk::ImageType::e2D, vk::ImageTiling::eOptimal, usage, {}
        );
        if (size.width > limits.maxExtent.width || size.height > limits.maxExtent.height)
            throw std::runtime_error("Native motion image dimensions exceed format limits");
        image.extent = size;
        image.format = f;
        image.usage = usage;
        image.handle = ctx.dev.createImage(vk::ImageCreateInfo {
            .imageType = vk::ImageType::e2D,
            .format = f,
            .extent = { size.width, size.height, 1 },
            .mipLevels = 1,
            .arrayLayers = 1,
            .samples = vk::SampleCountFlagBits::e1,
            .tiling = vk::ImageTiling::eOptimal,
            .usage = usage,
            .sharingMode = vk::SharingMode::eExclusive,
        });
        auto requirements = ctx.dev.getImageMemoryRequirements(image.handle);
        auto memory = ctx.physDev.getMemoryProperties();
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i) {
            if ((requirements.memoryTypeBits & (1u << i))
                && (memory.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal
                )) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX)
            throw std::runtime_error("No device-local native motion memory");
        image.memory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = requirements.size,
            .memoryTypeIndex = type,
        });
        ctx.dev.bindImageMemory(image.handle, image.memory, 0);
        image.view = ctx.dev.createImageView(vk::ImageViewCreateInfo {
            .image = image.handle,
            .viewType = vk::ImageViewType::e2D,
            .format = f,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        });
    }

    void makePipeline(
        const uint32_t* words,
        size_t bytes,
        unsigned bindings,
        vk::DescriptorSetLayout& descriptors,
        vk::PipelineLayout& layout,
        vk::ShaderModule& shader,
        vk::Pipeline& pipeline,
        const vk::SpecializationInfo* specialization = nullptr
    ) {
        std::vector<vk::DescriptorSetLayoutBinding> entries;
        for (unsigned i = 0; i < bindings; ++i) {
            entries.push_back(vk::DescriptorSetLayoutBinding {
                .binding = i,
                .descriptorType = i + 1 == bindings ? vk::DescriptorType::eStorageImage
                                                    : vk::DescriptorType::eCombinedImageSampler,
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            });
        }
        descriptors = ctx.dev.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo {
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = bindings,
            .pBindings = entries.data(),
        });
        vk::PushConstantRange push { vk::ShaderStageFlagBits::eCompute, 0, sizeof(WarpPush) };
        layout = ctx.dev.createPipelineLayout(vk::PipelineLayoutCreateInfo {
            .setLayoutCount = 1,
            .pSetLayouts = &descriptors,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        });
        shader = ctx.dev.createShaderModule(vk::ShaderModuleCreateInfo { .codeSize = bytes,
                                                                         .pCode = words });
        auto result = ctx.dev.createComputePipeline(
            {},
            vk::ComputePipelineCreateInfo {
                .stage = { .stage = vk::ShaderStageFlagBits::eCompute,
                           .module = shader,
                           .pName = "main",
                           .pSpecializationInfo = specialization },
                .layout = layout,
            }
        );
        if (result.result != vk::Result::eSuccess)
            throw std::runtime_error("Native motion pipeline creation failed");
        pipeline = result.value;
    }

    void initialize() {
        if (!ctx.meta.opticalFlowEnabled || !extent.width || extent.width % 2 || !extent.height
            || !lumaExtent.width || !lumaExtent.height
            || (format != vk::Format::eR8G8B8A8Unorm && format != vk::Format::eB8G8R8A8Unorm)) {
            throw std::runtime_error("Unsupported native motion device, dimensions or color format"
            );
        }
        vk::FormatProperties3 detailed {};
        vk::FormatProperties2 properties { .pNext = &detailed };
        ctx.physDev.getFormatProperties2(format, &properties);
        if (!(detailed.optimalTilingFeatures
              & vk::FormatFeatureFlagBits2::eStorageWriteWithoutFormat))
            throw std::runtime_error(
                "Native motion output format lacks storage writes without format"
            );
        auto sampled = vk::ImageUsageFlagBits::eSampled;
        auto transferDst = vk::ImageUsageFlagBits::eTransferDst;
        for (auto& image : colors)
            makeImage(image, extent, format, sampled | transferDst);
        for (auto& pair : lumas)
            for (auto& image : pair) {
                makeImage(
                    image,
                    lumaExtent,
                    vk::Format::eR8Unorm,
                    sampled | vk::ImageUsageFlagBits::eStorage
                        | vk::ImageUsageFlagBits::eTransferSrc
                );
            }
        vk::Extent2D flowSize { lumaExtent.width / 4, lumaExtent.height / 4 };
        auto flowUsage = sampled | transferDst;
#ifdef ALVR_NATIVE_MOTION_TESTING
        flowUsage |= vk::ImageUsageFlagBits::eTransferSrc;
#endif
        for (auto& pair : flows)
            for (auto& image : pair)
                makeImage(image, flowSize, vk::Format::eR16G16Sint, flowUsage);
        for (auto& image : costs)
            makeImage(image, flowSize, vk::Format::eR16G16Sint, sampled | transferDst);
        if (bidirectionalFlow)
            for (auto& image : backwardCosts)
                makeImage(image, flowSize, vk::Format::eR16G16Sint, sampled | transferDst);
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
        auto families = ctx.physDev.getQueueFamilyProperties();
        warpTimestampBits = families.at(ctx.meta.queueFamily).timestampValidBits;
        timestampPeriod = ctx.physDev.getProperties().limits.timestampPeriod;
        if (warpTimestampBits && timestampPeriod > 0)
            warpQueries = ctx.dev.createQueryPool(vk::QueryPoolCreateInfo {
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = 2,
            });
        vk::SamplerCreateInfo sampler {
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        };
        linearSampler = ctx.dev.createSampler(sampler);
        sampler.magFilter = sampler.minFilter = vk::Filter::eNearest;
        nearestSampler = ctx.dev.createSampler(sampler);
        makePipeline(
            shader::luma,
            sizeof(shader::luma),
            3,
            lumaDescriptors,
            lumaLayout,
            lumaShader,
            lumaPipeline
        );
        uint32_t exactFlowExit = allowExactFlowExit ? VK_TRUE : VK_FALSE;
        vk::SpecializationMapEntry exitEntry { 0, 0, sizeof(exactFlowExit) };
        vk::SpecializationInfo exitSpecialization {
            .mapEntryCount = 1,
            .pMapEntries = &exitEntry,
            .dataSize = sizeof(exactFlowExit),
            .pData = &exactFlowExit,
        };
        makePipeline(
            shader::warp,
            sizeof(shader::warp),
            9,
            warpDescriptors,
            warpLayout,
            warpShader,
            warpPipeline,
            &exitSpecialization
        );

        Context native {};
        native.physical = ctx.physDev;
        native.device = ctx.dev;
        ctx.useQueue([&](auto queue) { native.render_queue = queue; });
        native.optical_queue = ctx.getOpticalFlowQueue();
        native.render_family = ctx.meta.queueFamily;
        native.optical_family = ctx.meta.opticalFlowQueueFamily;
        native.device_proc = vkGetDeviceProcAddr;
        native.memory_properties = vkGetPhysicalDeviceMemoryProperties;
        native.properties = vkGetPhysicalDeviceProperties2;
        native.queue_properties = vkGetPhysicalDeviceQueueFamilyProperties;
        native.image_properties = vkGetPhysicalDeviceImageFormatProperties2;
        native.optical_formats = reinterpret_cast<PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV>(
            vkGetInstanceProcAddr(ctx.instance, "vkGetPhysicalDeviceOpticalFlowImageFormatsNV")
        );
        native.optical_flow_enabled = native.synchronization2_enabled = true;
        native.log = [](void*, const char* text) { Warn("Native motion estimator: %s", text); };
        for (auto& estimator : estimators)
            estimator = std::make_unique<NativeOpticalFlow>(
                native,
                lumaExtent.width,
                lumaExtent.height,
                VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV,
                bidirectionalFlow
            );
        Info(
            "Native motion controlled mode: alternate real/predicted, %ux%u per eye, luma %ux%u; "
            "exact flow exit %s; bidirectional flow %s; experimental pose association",
            extent.width / 2,
            extent.height,
            lumaExtent.width,
            lumaExtent.height,
            allowExactFlowExit ? "enabled" : "disabled",
            bidirectionalFlow ? "enabled" : "disabled"
        );
    }

    // Waits for a submission left running by submit(false).
    void waitPending() {
        if (idle)
            return;
        if (ctx.dev.waitForFences(fence, true, TimeoutNs) != vk::Result::eSuccess)
            throw std::runtime_error("Native motion GPU submission timed out");
        idle = true;
    }
    void begin() {
        waitPending();
        command.reset();
        command.begin(vk::CommandBufferBeginInfo {
            .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit });
    }
    void barrier(vk::Image image, vk::ImageLayout before, vk::ImageLayout after) {
        vk::ImageMemoryBarrier transition {
            .srcAccessMask = before == vk::ImageLayout::eUndefined
                ? vk::AccessFlags {}
                : vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .dstAccessMask = vk::AccessFlagBits::eMemoryRead | vk::AccessFlagBits::eMemoryWrite,
            .oldLayout = before,
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
            transition
        );
    }
    void transition(Image& image, vk::ImageLayout after) {
        barrier(image.handle, image.layout, after);
        image.layout = after;
    }
    // wait=false leaves the work running; begin(), estimate() and
    // NativeMotion::waitIdle() wait for it before anything depends on it.
    void submit(bool wait = true) {
        command.end();
        ctx.dev.resetFences(fence);
        vk::SubmitInfo info { .commandBufferCount = 1, .pCommandBuffers = &command };
        ctx.useQueue([&](auto& queue) { queue.submit(info, fence); });
        idle = false;
        if (wait)
            waitPending();
    }
    void dispatch(
        vk::Pipeline pipeline,
        vk::PipelineLayout layout,
        std::vector<vk::DescriptorImageInfo> const& images,
        const void* push,
        unsigned pushBytes,
        vk::Extent2D size
    ) {
        std::vector<vk::WriteDescriptorSet> writes;
        for (unsigned i = 0; i < images.size(); ++i) {
            writes.push_back(vk::WriteDescriptorSet {
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = i + 1 == images.size()
                    ? vk::DescriptorType::eStorageImage
                    : vk::DescriptorType::eCombinedImageSampler,
                .pImageInfo = &images[i],
            });
        }
        command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        command.pushDescriptorSetKHR(
            vk::PipelineBindPoint::eCompute, layout, 0, writes, ctx.dispatch
        );
        command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, pushBytes, push);
        command.dispatch((size.width + 7) / 8, (size.height + 7) / 8, 1);
    }
    vk::DescriptorImageInfo sampled(Image const& image, bool integer = false) const {
        return { integer ? nearestSampler : linearSampler,
                 image.view,
                 vk::ImageLayout::eShaderReadOnlyOptimal };
    }
    ImageResource resource(Image const& image) const {
        ImageResource r {};
        r.device = ctx.dev;
        r.image = image.handle;
        r.format = static_cast<VkFormat>(image.format);
        r.width = image.extent.width;
        r.height = image.extent.height;
        r.layout = static_cast<VkImageLayout>(image.layout);
        r.usage = static_cast<VkImageUsageFlags>(image.usage);
        r.owner_family = ctx.meta.queueFamily;
        r.render_accessible = true;
        return r;
    }
    void estimate() {
        if (!flowDirty)
            return;
        // The optical-flow queue reads lumas a deferred capture may still write.
        waitPending();
        double opticalGpuMs = 0;
        bool allTimed = true;
        for (unsigned eye = 0; eye < 2; ++eye)
            for (unsigned reverse = 0; reverse < (bidirectionalFlow ? 1u : 2u); ++reverse) {
                auto a = resource(lumas[reverse ? latest : 1 - latest][eye]);
                auto b = resource(lumas[reverse ? 1 - latest : latest][eye]);
                auto motion = resource(flows[eye][reverse]), cost = resource(costs[eye]);
                std::string error;
                auto backwardMotion = resource(flows[eye][1]);
                auto backwardCost = resource(backwardCosts[eye]);
                if (!estimators[eye]->execute(
                        a,
                        b,
                        motion,
                        cost,
                        error,
                        VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL,
                        bidirectionalFlow ? &backwardMotion : nullptr,
                        bidirectionalFlow ? &backwardCost : nullptr
                    ))
                    throw std::runtime_error("Native motion flow failed: " + error);
                flows[eye][reverse].layout = static_cast<vk::ImageLayout>(motion.layout);
                costs[eye].layout = static_cast<vk::ImageLayout>(cost.layout);
                if (bidirectionalFlow) {
                    flows[eye][1].layout = static_cast<vk::ImageLayout>(backwardMotion.layout);
                    backwardCosts[eye].layout = static_cast<vk::ImageLayout>(backwardCost.layout);
                }
                double deviceMs = estimators[eye]->opticalGpuMilliseconds();
                allTimed &= deviceMs >= 0;
                if (deviceMs >= 0)
                    opticalGpuMs += deviceMs;
            }
        cachedOpticalGpuMs = allTimed ? opticalGpuMs : -1;
        flowDirty = false;
    }
};

NativeMotion::NativeMotion(
    VkContext& ctx,
    vk::Extent2D extent,
    vk::Format format,
    uint32_t maxLumaDimension,
    bool allowExactFlowExit,
    bool bidirectionalFlow
)
    : state(std::make_unique<State>(
        ctx, extent, format, maxLumaDimension, allowExactFlowExit, bidirectionalFlow
    )) {
    state->initialize();
}
NativeMotion::~NativeMotion() = default;

bool NativeMotion::predict(
    vk::Image output,
    vk::ImageView view,
    vk::ImageLayout layout,
    uint64_t timestamp,
    std::function<void()> const& releaseInput,
    uint64_t clockNs, Rotation const* targetRotation
) {
    auto& s = *state;
    if (s.rotationMode && !targetRotation) return false;
    if (s.gapFill) {
        // Real pictures are all captured; this call is for a missed slot.
        s.captureThisFrame = true;
        if (s.retained < 2) {
            if (++s.gapNoHistory == 1 || s.gapNoHistory % 60 == 0)
                Info(
                    "Native motion gap fill without two real pictures: %llu (history resets %llu)",
                    static_cast<unsigned long long>(s.gapNoHistory),
                    static_cast<unsigned long long>(s.historyResets)
                );
            return false;
        }
        if (s.warmCaptures < GapFillWarmupPictures) {
            if (++s.gapWarmup == 1 || s.gapWarmup % 60 == 0)
                Info(
                    "Native motion gap fill waiting for a stable history: %llu slots",
                    static_cast<unsigned long long>(s.gapWarmup)
                );
            return false;
        }
        uint64_t newest = s.clocks[s.latest], previous = s.clocks[1 - s.latest];
        uint64_t const target = clockNs ? clockNs : timestamp;
        if (s.interpolate ? !interpolationTime(previous, newest, target)
                          : !predictionTime(previous, newest, target, 1.5)) {
            if (++s.gapRejected == 1 || s.gapRejected % 60 == 0)
                Info(
                    "Native motion gap fill rejected=%llu previous=%llu latest=%llu target=%llu",
                    static_cast<unsigned long long>(s.gapRejected),
                    static_cast<unsigned long long>(previous),
                    static_cast<unsigned long long>(newest),
                    static_cast<unsigned long long>(target)
                );
            return false;
        }
    } else {
        s.captureThisFrame = s.presentations++ % 2 == 0;
        if (s.captureThisFrame || s.retained < 2)
            return false;
    }
    uint64_t newest = s.timestamps[s.latest], previous = s.timestamps[1 - s.latest];
    auto time = s.gapFill
        ? (s.interpolate
               ? interpolationTime(s.clocks[1 - s.latest], s.clocks[s.latest], clockNs ? clockNs : timestamp)
               : predictionTime(s.clocks[1 - s.latest], s.clocks[s.latest], clockNs ? clockNs : timestamp, 1.5))
        : predictionTime(previous, newest, timestamp, 0.55);
    // A long pause, restart or missed job must not extrapolate stale motion.
    if (!time) {
        if (++s.rejected == 1 || s.rejected % 360 == 0)
            Info(
                "Native motion timestamp rejected=%llu previous=%llu latest=%llu target=%llu; "
                "using real frame",
                static_cast<unsigned long long>(s.rejected),
                static_cast<unsigned long long>(previous),
                static_cast<unsigned long long>(newest),
                static_cast<unsigned long long>(timestamp)
            );
        s.retained = 0;
        s.captureThisFrame = true;
        // This fallback real frame becomes the new even presentation. Leave
        // one warm-up presentation before retaining the next real frame;
        // otherwise the next two retained timestamps are only one tick apart
        // and every following prediction can reject its one-tick horizon.
        s.presentations = 1;
        return false;
    }
    if (releaseInput)
        releaseInput();
    s.timing = {};
    auto start = std::chrono::steady_clock::now();
    if (s.repeatReference) {
        auto& newest = s.colors[s.latest];
        s.begin();
        s.barrier(output, layout, vk::ImageLayout::eTransferDstOptimal);
        s.transition(newest, vk::ImageLayout::eTransferSrcOptimal);
        vk::ImageCopy copy {
            .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .extent = { s.extent.width, s.extent.height, 1 },
        };
        s.command.copyImage(
            newest.handle,
            vk::ImageLayout::eTransferSrcOptimal,
            output,
            vk::ImageLayout::eTransferDstOptimal,
            copy
        );
        s.transition(newest, vk::ImageLayout::eShaderReadOnlyOptimal);
        s.barrier(output, vk::ImageLayout::eTransferDstOptimal, layout);
        s.submit();
        if (++s.predictions == 1 || s.predictions % 360 == 0)
            Info(
                "Native motion repeat reference: repeated=%llu real=%llu",
                static_cast<unsigned long long>(s.predictions),
                static_cast<unsigned long long>(s.captures)
            );
        return true;
    }
    s.estimate();
    s.timing.opticalGpuMs = s.cachedOpticalGpuMs;
    auto flowEnd = std::chrono::steady_clock::now();
    s.begin();
    if (s.warpQueries) {
        s.command.resetQueryPool(s.warpQueries, 0, 2);
        s.command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, s.warpQueries, 0);
    }
    s.barrier(output, layout, vk::ImageLayout::eGeneral);
    WarpPush push {
        s.extent.width / 2, s.extent.height, s.lumaExtent.width, s.lumaExtent.height, *time
    };
    if (s.rotationMode) {
        push.rotationMode = s.stableResidual ? 5 : (s.confidenceFallback ? 3 : 1);
        // Bit 3: interpolate between both real pictures (mode 9).
        if (s.interpolate)
            push.rotationMode |= 8u;
        std::copy_n(s.historyRotation.quaternion, 4, push.previousQuaternion);
        std::copy_n(targetRotation->quaternion, 4, push.targetQuaternion);
        std::copy_n(targetRotation->leftTans, 4, push.leftTans);
        std::copy_n(targetRotation->rightTans, 4, push.rightTans);
        std::copy_n(targetRotation->translation, 3, push.targetTranslation);
        std::copy_n(s.historyRotation.translation, 3, push.previousTranslation);
    }
    if (s.depthValid[0] && s.depthValid[1])
        push.flags |= 1u;
    push.flags |= s.diagnosticFlags;
    if (s.depthOcclusion)
        push.flags |= 8u;
    s.dispatch(
        s.warpPipeline,
        s.warpLayout,
        { s.sampled(s.colors[1 - s.latest]),
          s.sampled(s.colors[s.latest]),
          s.sampled(s.flows[0][0], true),
          s.sampled(s.flows[0][1], true),
          s.sampled(s.flows[1][0], true),
          s.sampled(s.flows[1][1], true),
          // Without depth the colour images stand in; the shader skips them.
          s.sampled(s.depths[0].handle ? s.depths[1 - s.latest] : s.colors[1 - s.latest], true),
          s.sampled(s.depths[0].handle ? s.depths[s.latest] : s.colors[s.latest], true),
          { {}, view, vk::ImageLayout::eGeneral } },
        &push,
        sizeof(push),
        s.extent
    );
    s.barrier(output, vk::ImageLayout::eGeneral, layout);
    if (s.warpQueries)
        s.command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, s.warpQueries, 1);
    s.submit();
    auto warpEnd = std::chrono::steady_clock::now();
    s.timing.flowMs = std::chrono::duration<double, std::milli>(flowEnd - start).count();
    s.timing.warpMs = std::chrono::duration<double, std::milli>(warpEnd - flowEnd).count();
    if (s.warpQueries) {
        std::array<uint64_t, 2> ticks {};
        if (vkGetQueryPoolResults(
                s.ctx.dev,
                s.warpQueries,
                0,
                2,
                sizeof(ticks),
                ticks.data(),
                sizeof(uint64_t),
                VK_QUERY_RESULT_64_BIT
            )
            == VK_SUCCESS) {
            uint64_t mask
                = s.warpTimestampBits == 64 ? UINT64_MAX : (uint64_t(1) << s.warpTimestampBits) - 1;
            s.timing.warpGpuMs = double((ticks[1] - ticks[0]) & mask) * s.timestampPeriod / 1e6;
        }
    }
    if (++s.predictions == 1 || s.predictions % 360 == 0) {
        auto elapsed
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count();
        Info(
            "Native motion predicted=%llu real=%llu timestamp=%llu t=%.3f flow+warp=%.2f ms "
            "flow ahead=%.2f ms",
            static_cast<unsigned long long>(s.predictions),
            static_cast<unsigned long long>(s.captures),
            static_cast<unsigned long long>(timestamp),
            push.time,
            elapsed,
            s.aheadFlowMs
        );
    }
    return true;
}

NativeMotion::PredictionTiming NativeMotion::lastPredictionTiming() const { return state->timing; }

void NativeMotion::waitIdle() { state->waitPending(); }

void NativeMotion::setRepeatReference(bool repeat) { state->repeatReference = repeat; }

void NativeMotion::setConfidenceFallback(bool enabled) { state->confidenceFallback = enabled; }
#ifdef ALVR_NATIVE_MOTION_TESTING
std::vector<float> NativeMotion::diagnosticResidualMagnitudes() {
    auto& s=*state; s.estimate(); s.waitPending();
    auto extent=s.flows[0][0].extent;
    vk::DeviceSize bytes=vk::DeviceSize(extent.width)*extent.height*4;
    auto buffer=s.ctx.dev.createBuffer(vk::BufferCreateInfo {
        .size=bytes*4, .usage=vk::BufferUsageFlagBits::eTransferDst,
        .sharingMode=vk::SharingMode::eExclusive });
    auto requirements=s.ctx.dev.getBufferMemoryRequirements(buffer);
    auto properties=s.ctx.physDev.getMemoryProperties(); uint32_t type=UINT32_MAX;
    auto flags=vk::MemoryPropertyFlagBits::eHostVisible|vk::MemoryPropertyFlagBits::eHostCoherent;
    for(uint32_t i=0;i<properties.memoryTypeCount;i++)
        if((requirements.memoryTypeBits&(1u<<i)) && (properties.memoryTypes[i].propertyFlags&flags)==flags) {type=i;break;}
    if(type==UINT32_MAX) throw std::runtime_error("No diagnostic host memory");
    auto memory=s.ctx.dev.allocateMemory(vk::MemoryAllocateInfo {.allocationSize=requirements.size,.memoryTypeIndex=type});
    s.ctx.dev.bindBufferMemory(buffer,memory,0);
    s.begin();
    for(unsigned eye=0;eye<2;eye++) for(unsigned direction=0;direction<2;direction++) {
        auto& image=s.flows[eye][direction];auto before=image.layout;
        s.transition(image,vk::ImageLayout::eTransferSrcOptimal);
        vk::BufferImageCopy copy {.bufferOffset=bytes*(eye*2+direction),
            .imageSubresource={vk::ImageAspectFlagBits::eColor,0,0,1},
            .imageExtent={extent.width,extent.height,1}};
        s.command.copyImageToBuffer(image.handle,image.layout,buffer,copy);
        s.transition(image,before);
    }
    s.submit();
    auto values=static_cast<int16_t*>(s.ctx.dev.mapMemory(memory,0,bytes*4));
    std::vector<float> result;
    // Interior only: pixels revealed by rotation do not have correspondence.
    for(unsigned image=0;image<4;image++)
        for(uint32_t y=extent.height/5;y<extent.height*4/5;y++)
            for(uint32_t x=extent.width/5;x<extent.width*4/5;x++) {
                auto at=(image*extent.width*extent.height+y*extent.width+x)*2;
                float dx=values[at]/32.f*(s.extent.width/2)/s.lumaExtent.width;
                float dy=values[at+1]/32.f*s.extent.height/s.lumaExtent.height;
                result.push_back(std::sqrt(dx*dx+dy*dy));
            }
    s.ctx.dev.unmapMemory(memory);s.ctx.dev.destroy(buffer);s.ctx.dev.free(memory);
    return result;
}
#endif
#ifdef ALVR_NATIVE_MOTION_TESTING
void NativeMotion::setDiagnosticFlags(uint32_t flags) { state->diagnosticFlags = flags; }
#endif
void NativeMotion::setStableResidual(bool enabled) { state->stableResidual = enabled; }

void NativeMotion::setInterpolate(bool enabled) { state->interpolate = enabled; }

bool NativeMotion::restoreNewest(vk::Image output, vk::ImageLayout layout) {
    auto& s = *state;
    if (!s.retained)
        return false;
    auto& newest = s.colors[s.latest];
    s.begin();
    s.barrier(output, layout, vk::ImageLayout::eTransferDstOptimal);
    s.transition(newest, vk::ImageLayout::eTransferSrcOptimal);
    vk::ImageCopy copy {
        .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .extent = { s.extent.width, s.extent.height, 1 },
    };
    s.command.copyImage(
        newest.handle, vk::ImageLayout::eTransferSrcOptimal, output,
        vk::ImageLayout::eTransferDstOptimal, copy
    );
    s.transition(newest, vk::ImageLayout::eShaderReadOnlyOptimal);
    s.barrier(output, vk::ImageLayout::eTransferDstOptimal, layout);
    s.submit();
    return true;
}

void NativeMotion::setGapFill(bool fill) {
    state->gapFill = fill;
    state->captureThisFrame = true;
}

bool NativeMotion::gapFill() const { return state->gapFill; }

void NativeMotion::setDepthOcclusion(bool enabled) { state->depthOcclusion = enabled; }

bool NativeMotion::depthActive() const {
    return state->retained == 2 && state->depthValid[0] && state->depthValid[1];
}

void NativeMotion::prepare() {
    auto& s = *state;
    if (s.repeatReference || s.retained < 2 || !s.flowDirty)
        return;
    auto start = std::chrono::steady_clock::now();
    s.estimate();
    s.aheadFlowMs
        = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start).count();
}

void NativeMotion::capture(
    vk::Image output, vk::ImageLayout layout, uint64_t timestamp, uint64_t clockNs, Rotation const* historyRotation,
    Depth const* depth
) {
    auto& s = *state;
    if (!s.captureThisFrame || !timestamp)
        return;
    auto const captureStart = std::chrono::steady_clock::now();
    uint64_t const clock = clockNs ? clockNs : timestamp;
    if (s.retained && clock <= s.clocks[s.latest]) {
        s.retained = 0;
        ++s.historyResets;
    }
    // A pause (no real picture for 100 ms) or a reset restarts the warm-up.
    if (!s.retained || clock - s.clocks[s.latest] > 100'000'000)
        s.warmCaptures = 0;
    s.warmCaptures = std::min(s.warmCaptures + 1, 1000u);
    s.rotationMode = historyRotation != nullptr;
    if (historyRotation) s.historyRotation = *historyRotation;
    unsigned slot = s.retained ? 1 - s.latest : 0;
    s.begin();
    // In the General layout the copy only reads the output, without a layout
    // change, so it can run while the encoder reads the same picture: the
    // capture then does not wait (the encoder calls waitIdle() before the
    // next render writes the output). Other layouts keep the waiting path.
    bool const deferred = layout == vk::ImageLayout::eGeneral;
    vk::ImageLayout const copyLayout
        = deferred ? vk::ImageLayout::eGeneral : vk::ImageLayout::eTransferSrcOptimal;
    s.barrier(output, layout, copyLayout);
    s.transition(s.colors[slot], vk::ImageLayout::eTransferDstOptimal);
    vk::ImageCopy copy {
        .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .extent = { s.extent.width, s.extent.height, 1 },
    };
    s.command.copyImage(
        output, copyLayout, s.colors[slot].handle, vk::ImageLayout::eTransferDstOptimal, copy
    );
    s.transition(s.colors[slot], vk::ImageLayout::eShaderReadOnlyOptimal);
    if (!deferred)
        s.barrier(output, copyLayout, layout);
    s.depthValid[slot] = depth && depth->image && depth->extent.width && depth->extent.height;
    if (s.depthValid[slot]) {
        // Kept at the producer's extent: a plain copy works on any queue
        // (a blit needs a graphics queue). The warp scales coordinates.
        if (s.depths[0].extent != depth->extent) {
            // begin() waited for earlier work, so nothing still reads them.
            for (auto& image : s.depths) {
                s.ctx.dev.destroy(image.view);
                s.ctx.dev.destroy(image.handle);
                s.ctx.dev.free(image.memory);
                image = {};
                s.makeImage(image, depth->extent, vk::Format::eR32Sfloat,
                    vk::ImageUsageFlagBits::eSampled | vk::ImageUsageFlagBits::eTransferDst);
                // Both are bound on every prediction, written or not.
                s.transition(image, vk::ImageLayout::eShaderReadOnlyOptimal);
            }
            s.depthValid[1 - slot] = false;
        }
        s.barrier(depth->image, depth->layout, vk::ImageLayout::eTransferSrcOptimal);
        s.transition(s.depths[slot], vk::ImageLayout::eTransferDstOptimal);
        vk::ImageCopy depthCopy {
            .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
            .extent = { depth->extent.width, depth->extent.height, 1 },
        };
        s.command.copyImage(depth->image, vk::ImageLayout::eTransferSrcOptimal, s.depths[slot].handle,
            vk::ImageLayout::eTransferDstOptimal, depthCopy);
        s.transition(s.depths[slot], vk::ImageLayout::eShaderReadOnlyOptimal);
        s.barrier(depth->image, vk::ImageLayout::eTransferSrcOptimal, depth->layout);
    }
    for (uint32_t eye = 0; eye < 2; ++eye) {
        WarpPush lumaPush {};
        lumaPush.eye = eye;
        lumaPush.eyeWidth = s.extent.width / 2;
        lumaPush.height = s.extent.height;
        auto& luma = s.lumas[slot][eye];
        s.transition(luma, vk::ImageLayout::eGeneral);
        s.dispatch(
            s.lumaPipeline,
            s.lumaLayout,
            // Depth is unused here (flags 0); any sampled image fills the slot.
            { s.sampled(s.colors[slot]), s.sampled(s.colors[slot]), { {}, luma.view, vk::ImageLayout::eGeneral } },
            &lumaPush,
            sizeof(lumaPush),
            s.lumaExtent
        );
        s.transition(luma, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    if (historyRotation && s.retained) {
        // Rebuild the older luma in the newest real picture's geometry. The
        // older color stays untouched; the warp samples it with the same map.
        for (uint32_t eye = 0; eye < 2; ++eye) {
            auto& luma = s.lumas[s.latest][eye];
            s.transition(luma, vk::ImageLayout::eGeneral);
            WarpPush params {};
            params.eye = eye; params.eyeWidth = s.extent.width / 2; params.height = s.extent.height;
            params.rotationMode = 1;
            std::copy_n(historyRotation->quaternion,4,params.previousQuaternion);
            std::copy_n(historyRotation->leftTans,4,params.leftTans);
            std::copy_n(historyRotation->rightTans,4,params.rightTans);
            // With depth on both pictures the newest depth also aligns parallax.
            bool positional = s.depthValid[slot] && s.depthValid[s.latest];
            if (positional) {
                params.flags = 1;
                std::copy_n(historyRotation->translation,3,params.previousTranslation);
            }
            s.dispatch(s.lumaPipeline,s.lumaLayout,
                {s.sampled(s.colors[s.latest]), s.sampled(positional ? s.depths[slot] : s.colors[s.latest], true),
                 {{},luma.view,vk::ImageLayout::eGeneral}},
                &params,sizeof(params),s.lumaExtent);
            s.transition(luma,vk::ImageLayout::eShaderReadOnlyOptimal);
        }
    }
    s.submit(!deferred);
    s.latest = slot;
    s.timestamps[slot] = timestamp;
    s.clocks[slot] = clock;
    s.retained = std::min(2u, s.retained + 1);
    s.flowDirty = s.retained == 2;
    ++s.captures;
    // Capture runs on the encode path for every retained real picture.
    double ms = std::chrono::duration<double, std::milli>(
                    std::chrono::steady_clock::now() - captureStart
    )
                    .count();
    s.captureMsMax = std::max(s.captureMsMax, ms);
    s.captureMsSum += ms;
    if (s.captures % 360 == 0) {
        Info(
            "Native motion capture: %llu pictures, mean %.2f ms, max %.2f ms (last 360)",
            static_cast<unsigned long long>(s.captures),
            s.captureMsSum / 360.0,
            s.captureMsMax
        );
        s.captureMsSum = 0;
        s.captureMsMax = 0;
    }
}
}
