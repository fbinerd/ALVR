#include "NativeMotion.hpp"
#include "NativeMotionShaders.hpp"
#include "NativeMotionTiming.hpp"
#include "NativeOpticalFlow.hpp"

#include <array>
#include <chrono>
#include <stdexcept>

namespace alvr::motion {
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
    };
    static_assert(sizeof(WarpPush) == 20);
}

struct NativeMotion::State {
    VkContext& ctx;
    vk::Extent2D extent, lumaExtent;
    vk::Format format;
    std::array<Image, 2> colors;
    std::array<std::array<Image, 2>, 2> lumas; // retained frame / eye
    std::array<std::array<Image, 2>, 2> flows; // eye / forward, backward
    std::array<Image, 2> costs;
    std::array<std::unique_ptr<NativeOpticalFlow>, 2> estimators;
    vk::CommandPool pool {};
    vk::CommandBuffer command {};
    vk::Fence fence {};
    vk::Sampler linearSampler {}, nearestSampler {};
    vk::DescriptorSetLayout lumaDescriptors {}, warpDescriptors {};
    vk::PipelineLayout lumaLayout {}, warpLayout {};
    vk::ShaderModule lumaShader {}, warpShader {};
    vk::Pipeline lumaPipeline {}, warpPipeline {};
    std::array<uint64_t, 2> timestamps {};
    uint64_t presentations = 0, predictions = 0, captures = 0, rejected = 0;
    unsigned retained = 0, latest = 0;
    bool captureThisFrame = true, flowDirty = false, idle = true;

    State(VkContext& c, vk::Extent2D size, vk::Format f)
        : ctx(c)
        , extent(size)
        , format(f) {
        lumaExtent = vk::Extent2D { (size.width / 4) & ~3u, (size.height / 2) & ~3u };
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
        for (auto& pair : lumas)
            for (auto& image : pair)
                destroy(image);
        for (auto& pair : flows)
            for (auto& image : pair)
                destroy(image);
        for (auto& image : costs)
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
        vk::Pipeline& pipeline
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
                .stage
                = { .stage = vk::ShaderStageFlagBits::eCompute, .module = shader, .pName = "main" },
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
        for (auto& pair : flows)
            for (auto& image : pair)
                makeImage(image, flowSize, vk::Format::eR16G16Sint, sampled | transferDst);
        for (auto& image : costs)
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
            2,
            lumaDescriptors,
            lumaLayout,
            lumaShader,
            lumaPipeline
        );
        makePipeline(
            shader::warp,
            sizeof(shader::warp),
            7,
            warpDescriptors,
            warpLayout,
            warpShader,
            warpPipeline
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
                VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV
            );
        Info(
            "Native motion controlled mode: alternate real/predicted, %ux%u per eye, luma %ux%u; "
            "experimental pose association",
            extent.width / 2,
            extent.height,
            lumaExtent.width,
            lumaExtent.height
        );
    }

    void begin() {
        if (!idle)
            throw std::runtime_error("Native motion submission still pending");
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
    void submit() {
        command.end();
        ctx.dev.resetFences(fence);
        vk::SubmitInfo info { .commandBufferCount = 1, .pCommandBuffers = &command };
        ctx.useQueue([&](auto& queue) { queue.submit(info, fence); });
        idle = false;
        if (ctx.dev.waitForFences(fence, true, TimeoutNs) != vk::Result::eSuccess)
            throw std::runtime_error("Native motion GPU submission timed out");
        idle = true;
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
        for (unsigned eye = 0; eye < 2; ++eye)
            for (unsigned reverse = 0; reverse < 2; ++reverse) {
                auto a = resource(lumas[reverse ? latest : 1 - latest][eye]);
                auto b = resource(lumas[reverse ? 1 - latest : latest][eye]);
                auto motion = resource(flows[eye][reverse]), cost = resource(costs[eye]);
                std::string error;
                if (!estimators[eye]->execute(
                        a, b, motion, cost, error, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                    ))
                    throw std::runtime_error("Native motion flow failed: " + error);
                flows[eye][reverse].layout = static_cast<vk::ImageLayout>(motion.layout);
                costs[eye].layout = static_cast<vk::ImageLayout>(cost.layout);
            }
        flowDirty = false;
    }
};

NativeMotion::NativeMotion(VkContext& ctx, vk::Extent2D extent, vk::Format format)
    : state(std::make_unique<State>(ctx, extent, format)) {
    state->initialize();
}
NativeMotion::~NativeMotion() = default;

bool NativeMotion::predict(
    vk::Image output,
    vk::ImageView view,
    vk::ImageLayout layout,
    uint64_t timestamp,
    std::function<void()> const& releaseInput
) {
    auto& s = *state;
    s.captureThisFrame = s.presentations++ % 2 == 0;
    if (s.captureThisFrame || s.retained < 2)
        return false;
    uint64_t newest = s.timestamps[s.latest], previous = s.timestamps[1 - s.latest];
    auto time = predictionTime(previous, newest, timestamp);
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
    auto start = std::chrono::steady_clock::now();
    s.estimate();
    s.begin();
    s.barrier(output, layout, vk::ImageLayout::eGeneral);
    WarpPush push {
        s.extent.width / 2, s.extent.height, s.lumaExtent.width, s.lumaExtent.height, *time
    };
    s.dispatch(
        s.warpPipeline,
        s.warpLayout,
        { s.sampled(s.colors[1 - s.latest]),
          s.sampled(s.colors[s.latest]),
          s.sampled(s.flows[0][0], true),
          s.sampled(s.flows[0][1], true),
          s.sampled(s.flows[1][0], true),
          s.sampled(s.flows[1][1], true),
          { {}, view, vk::ImageLayout::eGeneral } },
        &push,
        sizeof(push),
        s.extent
    );
    s.barrier(output, vk::ImageLayout::eGeneral, layout);
    s.submit();
    if (++s.predictions == 1 || s.predictions % 360 == 0) {
        auto elapsed
            = std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - start)
                  .count();
        Info(
            "Native motion predicted=%llu real=%llu timestamp=%llu t=%.3f flow+warp=%.2f ms",
            static_cast<unsigned long long>(s.predictions),
            static_cast<unsigned long long>(s.captures),
            static_cast<unsigned long long>(timestamp),
            push.time,
            elapsed
        );
    }
    return true;
}

void NativeMotion::capture(vk::Image output, vk::ImageLayout layout, uint64_t timestamp) {
    auto& s = *state;
    if (!s.captureThisFrame || !timestamp)
        return;
    if (s.retained && timestamp <= s.timestamps[s.latest])
        s.retained = 0;
    unsigned slot = s.retained ? 1 - s.latest : 0;
    s.begin();
    s.barrier(output, layout, vk::ImageLayout::eTransferSrcOptimal);
    s.transition(s.colors[slot], vk::ImageLayout::eTransferDstOptimal);
    vk::ImageCopy copy {
        .srcSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .dstSubresource = { vk::ImageAspectFlagBits::eColor, 0, 0, 1 },
        .extent = { s.extent.width, s.extent.height, 1 },
    };
    s.command.copyImage(
        output,
        vk::ImageLayout::eTransferSrcOptimal,
        s.colors[slot].handle,
        vk::ImageLayout::eTransferDstOptimal,
        copy
    );
    s.transition(s.colors[slot], vk::ImageLayout::eShaderReadOnlyOptimal);
    s.barrier(output, vk::ImageLayout::eTransferSrcOptimal, layout);
    for (uint32_t eye = 0; eye < 2; ++eye) {
        auto& luma = s.lumas[slot][eye];
        s.transition(luma, vk::ImageLayout::eGeneral);
        s.dispatch(
            s.lumaPipeline,
            s.lumaLayout,
            { s.sampled(s.colors[slot]), { {}, luma.view, vk::ImageLayout::eGeneral } },
            &eye,
            sizeof(eye),
            s.lumaExtent
        );
        s.transition(luma, vk::ImageLayout::eShaderReadOnlyOptimal);
    }
    s.submit();
    s.latest = slot;
    s.timestamps[slot] = timestamp;
    s.retained = std::min(2u, s.retained + 1);
    s.flowDirty = s.retained == 2;
    ++s.captures;
}
}
