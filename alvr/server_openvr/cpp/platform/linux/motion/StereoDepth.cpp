#include "StereoDepth.hpp"
#include "StereoDepthShaders.hpp"
#include "NativeOpticalFlow.hpp"

#include <chrono>
#include <cstdlib>
#include <string>
#include <cmath>
#include <iostream>
#include <stdexcept>
#include <vector>

namespace alvr::motion {

namespace {
    constexpr uint64_t TimeoutNs = 1'000'000'000;

    struct Image {
        vk::Image handle {};
        vk::DeviceMemory memory {};
        vk::ImageView view {};
        vk::ImageLayout layout = vk::ImageLayout::eUndefined;
        vk::Extent2D extent {};
        vk::Format format {};
        vk::ImageUsageFlags usage {};
    };

    struct LumaPush {
        uint32_t eyeWidth = 0;
        uint32_t height = 0;
        uint32_t lumaWidth = 0;
        uint32_t lumaHeight = 0;
    };

    struct DepthPush {
        uint32_t eyeWidth = 0;
        uint32_t height = 0;
        uint32_t lumaWidth = 0;
        uint32_t lumaHeight = 0;
        float ipd = 0.063f;
        float maxVerticalError = 2.0f;
        float maxConsistencyError = 2.0f;
        uint32_t padding = 0;
        float leftTans[4] {};  // tan_left, tan_right, tan_up, tan_down
        float rightTans[4] {}; // tan_left, tan_right, tan_up, tan_down
    };

    struct FillPush {
        uint32_t eyeWidth = 0;
        uint32_t height = 0;
        uint32_t radius = 4;
        uint32_t minValidNeighbors = 2;
    };
} // namespace

struct StereoDepth::State {
    VkContext& ctx;
    vk::Extent2D fullExtent {};
    vk::Extent2D eyeExtent {};
    vk::Extent2D lumaExtent {};
    vk::Extent2D flowExtent {};
    vk::Format colorFormat {};

    Image leftLuma, rightLuma;
    Image leftToRightFlow, rightToLeftFlow;
    Image leftCost, rightCost;
    Image rawDepth;
    Image internalDepth;

    std::unique_ptr<NativeOpticalFlow> estimator;

    vk::CommandPool pool {};
    vk::CommandBuffer command {};
    vk::Fence fence {};

    vk::QueryPool queries {};
    uint32_t timestampBits = 0;
    double timestampPeriod = 0.0;

    vk::Sampler linearSampler {};
    vk::Sampler nearestSampler {};

    vk::DescriptorSetLayout lumaDescriptors {};
    vk::PipelineLayout lumaLayout {};
    vk::ShaderModule lumaShader {};
    vk::Pipeline lumaPipeline {};

    vk::DescriptorSetLayout depthDescriptors {};
    vk::PipelineLayout depthLayout {};
    vk::ShaderModule depthShader {};
    vk::Pipeline depthPipeline {};

    vk::DescriptorSetLayout fillDescriptors {};
    vk::PipelineLayout fillLayout {};
    vk::ShaderModule fillShader {};
    vk::Pipeline fillPipeline {};

    State(
        VkContext& c,
        vk::Extent2D extent,
        vk::Format format,
        uint32_t maxLumaDimension
    )
        : ctx(c)
        , fullExtent(extent)
        , colorFormat(format) {
        if (!extent.width || extent.width % 2 != 0 || !extent.height) {
            throw std::runtime_error("StereoDepth: Invalid full dimensions (width must be even)");
        }
        eyeExtent = vk::Extent2D { extent.width / 2, extent.height };

        // Luma dimensions: multiple of 4 required by NV_optical_flow
        lumaExtent = vk::Extent2D { eyeExtent.width & ~3u, eyeExtent.height & ~3u };
        if (maxLumaDimension > 0) {
            uint32_t largest = std::max(lumaExtent.width, lumaExtent.height);
            if (largest > maxLumaDimension) {
                lumaExtent.width = uint32_t(uint64_t(lumaExtent.width) * maxLumaDimension / largest) & ~3u;
                lumaExtent.height = uint32_t(uint64_t(lumaExtent.height) * maxLumaDimension / largest) & ~3u;
            }
        }
        if (lumaExtent.width < 64 || lumaExtent.height < 64) {
            throw std::runtime_error("StereoDepth: Luma dimensions too small for optical flow");
        }

        flowExtent = vk::Extent2D { lumaExtent.width / 4, lumaExtent.height / 4 };
        initialize();
    }

    ~State() {
        try {
            ctx.dev.waitIdle();
        } catch (...) {
        }
        destroyImage(leftLuma);
        destroyImage(rightLuma);
        destroyImage(leftToRightFlow);
        destroyImage(rightToLeftFlow);
        destroyImage(leftCost);
        destroyImage(rightCost);
        destroyImage(rawDepth);
        destroyImage(internalDepth);

        ctx.dev.destroy(fillPipeline);
        ctx.dev.destroy(fillShader);
        ctx.dev.destroy(fillLayout);
        ctx.dev.destroy(fillDescriptors);

        ctx.dev.destroy(depthPipeline);
        ctx.dev.destroy(depthShader);
        ctx.dev.destroy(depthLayout);
        ctx.dev.destroy(depthDescriptors);

        ctx.dev.destroy(lumaPipeline);
        ctx.dev.destroy(lumaShader);
        ctx.dev.destroy(lumaLayout);
        ctx.dev.destroy(lumaDescriptors);

        ctx.dev.destroy(linearSampler);
        ctx.dev.destroy(nearestSampler);
        if (queries) ctx.dev.destroy(queries);
        ctx.dev.destroy(fence);
        ctx.dev.destroy(pool);
    }

    void destroyImage(Image& img) {
        if (img.view) ctx.dev.destroy(img.view);
        if (img.handle) ctx.dev.destroy(img.handle);
        if (img.memory) ctx.dev.free(img.memory);
        img = {};
    }

    void makeImage(Image& image, vk::Extent2D size, vk::Format f, vk::ImageUsageFlags usage) {
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

        auto req = ctx.dev.getImageMemoryRequirements(image.handle);
        auto props = ctx.physDev.getMemoryProperties();
        uint32_t type = UINT32_MAX;
        for (uint32_t i = 0; i < props.memoryTypeCount; ++i) {
            if ((req.memoryTypeBits & (1u << i))
                && (props.memoryTypes[i].propertyFlags & vk::MemoryPropertyFlagBits::eDeviceLocal)) {
                type = i;
                break;
            }
        }
        if (type == UINT32_MAX) throw std::runtime_error("StereoDepth: No device-local memory");

        image.memory = ctx.dev.allocateMemory(vk::MemoryAllocateInfo {
            .allocationSize = req.size,
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
        const std::vector<vk::DescriptorType>& descriptorTypes,
        unsigned pushConstantSize,
        vk::DescriptorSetLayout& descriptors,
        vk::PipelineLayout& layout,
        vk::ShaderModule& shader,
        vk::Pipeline& pipeline
    ) {
        std::vector<vk::DescriptorSetLayoutBinding> entries;
        for (uint32_t i = 0; i < descriptorTypes.size(); ++i) {
            entries.push_back(vk::DescriptorSetLayoutBinding {
                .binding = i,
                .descriptorType = descriptorTypes[i],
                .descriptorCount = 1,
                .stageFlags = vk::ShaderStageFlagBits::eCompute,
            });
        }
        descriptors = ctx.dev.createDescriptorSetLayout(vk::DescriptorSetLayoutCreateInfo {
            .flags = vk::DescriptorSetLayoutCreateFlagBits::ePushDescriptorKHR,
            .bindingCount = uint32_t(entries.size()),
            .pBindings = entries.data(),
        });
        vk::PushConstantRange push { vk::ShaderStageFlagBits::eCompute, 0, pushConstantSize };
        layout = ctx.dev.createPipelineLayout(vk::PipelineLayoutCreateInfo {
            .setLayoutCount = 1,
            .pSetLayouts = &descriptors,
            .pushConstantRangeCount = 1,
            .pPushConstantRanges = &push,
        });
        shader = ctx.dev.createShaderModule(vk::ShaderModuleCreateInfo {
            .codeSize = bytes,
            .pCode = words,
        });
        auto result = ctx.dev.createComputePipeline(
            {},
            vk::ComputePipelineCreateInfo {
                .stage = {
                    .stage = vk::ShaderStageFlagBits::eCompute,
                    .module = shader,
                    .pName = "main",
                },
                .layout = layout,
            }
        );
        if (result.result != vk::Result::eSuccess)
            throw std::runtime_error("StereoDepth: Compute pipeline creation failed");
        pipeline = result.value;
    }

    void initialize() {
        if (!ctx.meta.opticalFlowEnabled) {
            throw std::runtime_error("StereoDepth: Vulkan device lacks optical-flow capability");
        }

        auto sampled = vk::ImageUsageFlagBits::eSampled;
        auto storage = vk::ImageUsageFlagBits::eStorage;
        auto transferDst = vk::ImageUsageFlagBits::eTransferDst;
        auto transferSrc = vk::ImageUsageFlagBits::eTransferSrc;

        // Allocate lumas
        makeImage(leftLuma, lumaExtent, vk::Format::eR8Unorm, sampled | storage | transferSrc);
        makeImage(rightLuma, lumaExtent, vk::Format::eR8Unorm, sampled | storage | transferSrc);

        // Allocate flows & costs
        makeImage(leftToRightFlow, flowExtent, vk::Format::eR16G16Sint, sampled | transferDst | transferSrc);
        makeImage(rightToLeftFlow, flowExtent, vk::Format::eR16G16Sint, sampled | transferDst | transferSrc);
        makeImage(leftCost, flowExtent, vk::Format::eR16G16Sint, sampled | transferDst);
        makeImage(rightCost, flowExtent, vk::Format::eR16G16Sint, sampled | transferDst);

        // Allocate raw & internal depth
        makeImage(rawDepth, fullExtent, vk::Format::eR32Sfloat, storage | sampled | transferSrc);
        makeImage(internalDepth, fullExtent, vk::Format::eR32Sfloat, storage | sampled | transferSrc | transferDst);

        // Command pool, buffer, fence
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

        // Timestamps
        auto families = ctx.physDev.getQueueFamilyProperties();
        timestampBits = families.at(ctx.meta.queueFamily).timestampValidBits;
        timestampPeriod = ctx.physDev.getProperties().limits.timestampPeriod;
        if (timestampBits && timestampPeriod > 0) {
            queries = ctx.dev.createQueryPool(vk::QueryPoolCreateInfo {
                .queryType = vk::QueryType::eTimestamp,
                .queryCount = 2,
            });
        }

        // Samplers
        linearSampler = ctx.dev.createSampler(vk::SamplerCreateInfo {
            .magFilter = vk::Filter::eLinear,
            .minFilter = vk::Filter::eLinear,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        });
        nearestSampler = ctx.dev.createSampler(vk::SamplerCreateInfo {
            .magFilter = vk::Filter::eNearest,
            .minFilter = vk::Filter::eNearest,
            .addressModeU = vk::SamplerAddressMode::eClampToEdge,
            .addressModeV = vk::SamplerAddressMode::eClampToEdge,
            .addressModeW = vk::SamplerAddressMode::eClampToEdge,
        });

        // Initialize Optical Flow Wrapper
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
        native.log = [](void*, const char* text) { std::cerr << "StereoDepth OF: " << text << std::endl; };

        estimator = std::make_unique<NativeOpticalFlow>(
            native,
            lumaExtent.width,
            lumaExtent.height,
            // STEREO_FLOW_LEVEL=slow|medium (default fast, as native motion).
            [] {
                char const* level = std::getenv("STEREO_FLOW_LEVEL");
                std::string text = level ? level : "";
                return text == "slow" ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV
                    : text == "medium" ? VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV
                                       : VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV;
            }(),
            // Two one-way estimates: the combined backward output came back
            // empty at some luma sizes (every match then failed consistency).
            false
        );

        // Build Pipelines
        // 1. stereo_luma
        makePipeline(
            shader::stereo_luma,
            sizeof(shader::stereo_luma),
            { vk::DescriptorType::eCombinedImageSampler,
              vk::DescriptorType::eStorageImage,
              vk::DescriptorType::eStorageImage },
            sizeof(LumaPush),
            lumaDescriptors,
            lumaLayout,
            lumaShader,
            lumaPipeline
        );

        // 2. stereo_depth
        makePipeline(
            shader::stereo_depth,
            sizeof(shader::stereo_depth),
            { vk::DescriptorType::eCombinedImageSampler,
              vk::DescriptorType::eCombinedImageSampler,
              vk::DescriptorType::eStorageImage },
            sizeof(DepthPush),
            depthDescriptors,
            depthLayout,
            depthShader,
            depthPipeline
        );

        // 3. stereo_fill
        makePipeline(
            shader::stereo_fill,
            sizeof(shader::stereo_fill),
            { vk::DescriptorType::eStorageImage,
              vk::DescriptorType::eStorageImage },
            sizeof(FillPush),
            fillDescriptors,
            fillLayout,
            fillShader,
            fillPipeline
        );
    }

    void barrier(
        vk::Image image,
        vk::ImageLayout before,
        vk::ImageLayout after,
        vk::AccessFlags srcAccess,
        vk::AccessFlags dstAccess,
        vk::PipelineStageFlags srcStage = vk::PipelineStageFlagBits::eAllCommands,
        vk::PipelineStageFlags dstStage = vk::PipelineStageFlagBits::eAllCommands
    ) {
        vk::ImageMemoryBarrier b {
            .srcAccessMask = srcAccess,
            .dstAccessMask = dstAccess,
            .oldLayout = before,
            .newLayout = after,
            .srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED,
            .image = image,
            .subresourceRange = { vk::ImageAspectFlagBits::eColor, 0, 1, 0, 1 },
        };
        command.pipelineBarrier(srcStage, dstStage, {}, {}, {}, b);
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

    void dispatch(
        vk::Pipeline pipeline,
        vk::PipelineLayout layout,
        const std::vector<vk::DescriptorImageInfo>& images,
        const std::vector<vk::DescriptorType>& types,
        const void* push,
        unsigned pushBytes,
        vk::Extent2D workSize
    ) {
        std::vector<vk::WriteDescriptorSet> writes;
        writes.reserve(images.size());
        for (uint32_t i = 0; i < images.size(); ++i) {
            writes.push_back(vk::WriteDescriptorSet {
                .dstBinding = i,
                .descriptorCount = 1,
                .descriptorType = types[i],
                .pImageInfo = &images[i],
            });
        }
        command.bindPipeline(vk::PipelineBindPoint::eCompute, pipeline);
        command.pushDescriptorSetKHR(
            vk::PipelineBindPoint::eCompute, layout, 0, writes, ctx.dispatch
        );
        command.pushConstants(layout, vk::ShaderStageFlagBits::eCompute, 0, pushBytes, push);
        command.dispatch((workSize.width + 7) / 8, (workSize.height + 7) / 8, 1);
    }
};

StereoDepth::StereoDepth(
    VkContext& ctx,
    vk::Extent2D fullExtent,
    vk::Format colorFormat,
    uint32_t maxLumaDimension
)
    : state(std::make_unique<State>(ctx, fullExtent, colorFormat, maxLumaDimension)) {}

StereoDepth::~StereoDepth() = default;

vk::Extent2D StereoDepth::fullExtent() const { return state->fullExtent; }
vk::Extent2D StereoDepth::eyeExtent() const { return state->eyeExtent; }
vk::Extent2D StereoDepth::lumaExtent() const { return state->lumaExtent; }

vk::Image StereoDepth::internalDepthImage() const { return state->internalDepth.handle; }
vk::ImageView StereoDepth::internalDepthView() const { return state->internalDepth.view; }

bool StereoDepth::estimate(
    vk::Image inputColor,
    vk::ImageView inputColorView,
    vk::ImageLayout inputColorLayout,
    vk::Image outputDepth,
    vk::ImageView outputDepthView,
    vk::ImageLayout outputDepthLayout,
    const StereoParams& params,
    StereoDepthTiming* timing
) {
    auto& s = *state;
    auto tStart = std::chrono::steady_clock::now();

    vk::Image targetDepth = outputDepth ? outputDepth : s.internalDepth.handle;
    vk::ImageView targetDepthView = outputDepthView ? outputDepthView : s.internalDepth.view;
    vk::ImageLayout targetDepthLayout = outputDepth ? outputDepthLayout : vk::ImageLayout::eGeneral;

    // --- PHASE 1: Luma extraction ---
    auto tLuma0 = std::chrono::steady_clock::now();
    s.command.reset();
    s.command.begin(vk::CommandBufferBeginInfo {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });

    // Ensure inputColor in shader read layout
    if (inputColorLayout != vk::ImageLayout::eShaderReadOnlyOptimal &&
        inputColorLayout != vk::ImageLayout::eGeneral) {
        s.barrier(
            inputColor,
            inputColorLayout,
            vk::ImageLayout::eShaderReadOnlyOptimal,
            vk::AccessFlagBits::eMemoryWrite,
            vk::AccessFlagBits::eShaderRead
        );
    }

    s.barrier(
        s.leftLuma.handle,
        s.leftLuma.layout,
        vk::ImageLayout::eGeneral,
        {},
        vk::AccessFlagBits::eShaderWrite
    );
    s.barrier(
        s.rightLuma.handle,
        s.rightLuma.layout,
        vk::ImageLayout::eGeneral,
        {},
        vk::AccessFlagBits::eShaderWrite
    );
    s.leftLuma.layout = vk::ImageLayout::eGeneral;
    s.rightLuma.layout = vk::ImageLayout::eGeneral;

    LumaPush lumaPush {
        .eyeWidth = s.eyeExtent.width,
        .height = s.eyeExtent.height,
        .lumaWidth = s.lumaExtent.width,
        .lumaHeight = s.lumaExtent.height,
    };

    std::vector<vk::DescriptorImageInfo> lumaImages = {
        { s.linearSampler, inputColorView, vk::ImageLayout::eShaderReadOnlyOptimal },
        { {}, s.leftLuma.view, vk::ImageLayout::eGeneral },
        { {}, s.rightLuma.view, vk::ImageLayout::eGeneral },
    };
    std::vector<vk::DescriptorType> lumaTypes = {
        vk::DescriptorType::eCombinedImageSampler,
        vk::DescriptorType::eStorageImage,
        vk::DescriptorType::eStorageImage,
    };
    s.dispatch(s.lumaPipeline, s.lumaLayout, lumaImages, lumaTypes, &lumaPush, sizeof(lumaPush), s.lumaExtent);

    // Make luma available for reading
    s.barrier(
        s.leftLuma.handle,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eMemoryRead
    );
    s.barrier(
        s.rightLuma.handle,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eMemoryRead
    );

    s.command.end();
    s.ctx.dev.resetFences(s.fence);
    vk::SubmitInfo lumaSubmit { .commandBufferCount = 1, .pCommandBuffers = &s.command };
    s.ctx.useQueue([&](auto& queue) { queue.submit(lumaSubmit, s.fence); });
    if (s.ctx.dev.waitForFences(s.fence, true, TimeoutNs) != vk::Result::eSuccess) {
        throw std::runtime_error("StereoDepth: Luma pass timed out");
    }
    auto tLuma1 = std::chrono::steady_clock::now();

    // --- PHASE 2: Hardware Optical Flow ---
    auto tFlow0 = std::chrono::steady_clock::now();
    auto inLeft = s.resource(s.leftLuma);
    auto refRight = s.resource(s.rightLuma);
    auto motionL2R = s.resource(s.leftToRightFlow);
    auto costL2R = s.resource(s.leftCost);
    auto motionR2L = s.resource(s.rightToLeftFlow);
    auto costR2L = s.resource(s.rightCost);

    std::string flowError;
    double opticalMs = 0;
    bool timed = true;
    for (bool rightToLeft : { false, true }) {
        auto& a = rightToLeft ? refRight : inLeft;
        auto& b = rightToLeft ? inLeft : refRight;
        if (!s.estimator->execute(a, b, rightToLeft ? motionR2L : motionL2R, rightToLeft ? costR2L : costL2R,
                flowError, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, nullptr, nullptr))
            throw std::runtime_error("StereoDepth: Optical flow failed: " + flowError);
        double ms = s.estimator->opticalGpuMilliseconds();
        timed &= ms >= 0;
        opticalMs += ms;
    }
    s.leftToRightFlow.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    s.rightToLeftFlow.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    s.leftCost.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    s.rightCost.layout = vk::ImageLayout::eShaderReadOnlyOptimal;
    auto tFlow1 = std::chrono::steady_clock::now();

    // --- PHASE 3 & 4: Disparity-to-Depth & Hole Filling ---
    auto tDepth0 = std::chrono::steady_clock::now();
    s.command.reset();
    s.command.begin(vk::CommandBufferBeginInfo {
        .flags = vk::CommandBufferUsageFlagBits::eOneTimeSubmit,
    });

    if (s.queries) {
        s.command.resetQueryPool(s.queries, 0, 2);
        s.command.writeTimestamp(vk::PipelineStageFlagBits::eTopOfPipe, s.queries, 0);
    }

    s.barrier(
        s.rawDepth.handle,
        s.rawDepth.layout,
        vk::ImageLayout::eGeneral,
        {},
        vk::AccessFlagBits::eShaderWrite
    );
    s.rawDepth.layout = vk::ImageLayout::eGeneral;

    DepthPush depthPush {
        .eyeWidth = s.eyeExtent.width,
        .height = s.eyeExtent.height,
        .lumaWidth = s.lumaExtent.width,
        .lumaHeight = s.lumaExtent.height,
        .ipd = params.ipd,
        .maxVerticalError = params.maxVerticalError,
        .maxConsistencyError = params.maxConsistencyError,
        .padding = 0,
        .leftTans = { params.leftTans.left, params.leftTans.right, params.leftTans.up, params.leftTans.down },
        .rightTans = { params.rightTans.left, params.rightTans.right, params.rightTans.up, params.rightTans.down },
    };

    std::vector<vk::DescriptorImageInfo> depthImages = {
        { s.nearestSampler, s.leftToRightFlow.view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { s.nearestSampler, s.rightToLeftFlow.view, vk::ImageLayout::eShaderReadOnlyOptimal },
        { {}, s.rawDepth.view, vk::ImageLayout::eGeneral },
    };
    std::vector<vk::DescriptorType> depthTypes = {
        vk::DescriptorType::eCombinedImageSampler,
        vk::DescriptorType::eCombinedImageSampler,
        vk::DescriptorType::eStorageImage,
    };
    s.dispatch(s.depthPipeline, s.depthLayout, depthImages, depthTypes, &depthPush, sizeof(depthPush), s.fullExtent);

    // Barrier: rawDepth write -> read
    s.barrier(
        s.rawDepth.handle,
        vk::ImageLayout::eGeneral,
        vk::ImageLayout::eGeneral,
        vk::AccessFlagBits::eShaderWrite,
        vk::AccessFlagBits::eShaderRead
    );

    // Hole filling pass
    s.barrier(
        targetDepth,
        vk::ImageLayout::eUndefined,
        vk::ImageLayout::eGeneral,
        {},
        vk::AccessFlagBits::eShaderWrite
    );

    FillPush fillPush {
        .eyeWidth = s.eyeExtent.width,
        .height = s.eyeExtent.height,
        .radius = params.fillRadius,
        .minValidNeighbors = params.fillMinNeighbors,
    };

    std::vector<vk::DescriptorImageInfo> fillImages = {
        { {}, s.rawDepth.view, vk::ImageLayout::eGeneral },
        { {}, targetDepthView, vk::ImageLayout::eGeneral },
    };
    std::vector<vk::DescriptorType> fillTypes = {
        vk::DescriptorType::eStorageImage,
        vk::DescriptorType::eStorageImage,
    };
    s.dispatch(s.fillPipeline, s.fillLayout, fillImages, fillTypes, &fillPush, sizeof(fillPush), s.fullExtent);

    // Transition output to target layout
    if (targetDepthLayout != vk::ImageLayout::eGeneral) {
        s.barrier(
            targetDepth,
            vk::ImageLayout::eGeneral,
            targetDepthLayout,
            vk::AccessFlagBits::eShaderWrite,
            vk::AccessFlagBits::eMemoryRead
        );
    }

    if (s.queries) {
        s.command.writeTimestamp(vk::PipelineStageFlagBits::eBottomOfPipe, s.queries, 1);
    }

    s.command.end();
    s.ctx.dev.resetFences(s.fence);
    vk::SubmitInfo depthSubmit { .commandBufferCount = 1, .pCommandBuffers = &s.command };
    s.ctx.useQueue([&](auto& queue) { queue.submit(depthSubmit, s.fence); });
    if (s.ctx.dev.waitForFences(s.fence, true, TimeoutNs) != vk::Result::eSuccess) {
        throw std::runtime_error("StereoDepth: Depth/fill pass timed out");
    }
    auto tDepth1 = std::chrono::steady_clock::now();

    double depthGpuMs = -1.0;
    if (s.queries && s.timestampBits) {
        std::array<uint64_t, 2> ticks {};
        if (s.ctx.dev.getQueryPoolResults(
                s.queries, 0, 2, sizeof(ticks), ticks.data(), sizeof(uint64_t), vk::QueryResultFlagBits::e64
            ) == vk::Result::eSuccess) {
            uint64_t mask = s.timestampBits == 64 ? UINT64_MAX : (uint64_t(1) << s.timestampBits) - 1;
            depthGpuMs = double((ticks[1] - ticks[0]) & mask) * s.timestampPeriod / 1e6;
        }
    }

    if (timing) {
        auto tEnd = std::chrono::steady_clock::now();
        timing->totalWallMs = std::chrono::duration<double, std::milli>(tEnd - tStart).count();
        timing->lumaWallMs = std::chrono::duration<double, std::milli>(tLuma1 - tLuma0).count();
        timing->flowWallMs = std::chrono::duration<double, std::milli>(tFlow1 - tFlow0).count();
        timing->depthWallMs = std::chrono::duration<double, std::milli>(tDepth1 - tDepth0).count();
        timing->fillWallMs = timing->depthWallMs; // integrated pass
        timing->opticalGpuMs = timed ? opticalMs : -1.0;
        timing->depthGpuMs = depthGpuMs;
    }

    return true;
}

} // namespace alvr::motion
