#include "NativeOpticalFlow.hpp"
#include <array>
#include <mutex>
#include <stdexcept>
#include <vector>

namespace alvr::motion {
namespace {
    void checked(VkResult r, const char* op) {
        if (r != VK_SUCCESS)
            throw std::runtime_error(std::string(op) + ": VkResult " + std::to_string(r));
    }
    struct Scope {
        const Context& c;
        explicit Scope(const Context& ctx)
            : c(ctx) {
            if (c.lock)
                c.lock(c.user);
        }
        ~Scope() {
            if (c.unlock)
                c.unlock(c.user);
        }
    };
    struct Image {
        VkImage handle = VK_NULL_HANDLE;
        VkImageView view = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
        VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    };
    struct Buffer {
        VkBuffer handle = VK_NULL_HANDLE;
        VkDeviceMemory memory = VK_NULL_HANDLE;
    };
#define DEVICE_FUNCTIONS(X)                                                                        \
    X(CreateImage)                                                                                 \
    X(DestroyImage) X(GetImageMemoryRequirements) X(AllocateMemory) X(FreeMemory)                  \
        X(BindImageMemory) X(CreateImageView) X(DestroyImageView) X(CreateBuffer) X(DestroyBuffer) \
            X(GetBufferMemoryRequirements) X(BindBufferMemory) X(CreateCommandPool)                \
                X(DestroyCommandPool) X(AllocateCommandBuffers) X(ResetCommandBuffer)              \
                    X(BeginCommandBuffer) X(EndCommandBuffer) X(CreateFence) X(DestroyFence)       \
                        X(ResetFences) X(WaitForFences) X(CreateSemaphore) X(DestroySemaphore)     \
                            X(QueueSubmit) X(CmdCopyImage) X(CmdCopyImageToBuffer)                 \
                                X(CmdCopyBufferToImage) X(CreateOpticalFlowSessionNV)              \
                                    X(DestroyOpticalFlowSessionNV)                                 \
                                        X(BindOpticalFlowSessionImageNV)                           \
                                            X(CmdOpticalFlowExecuteNV)
    struct Dispatch {
#define FIELD(n) PFN_vk##n n = nullptr;
        DEVICE_FUNCTIONS(FIELD)
#undef FIELD
        PFN_vkCmdPipelineBarrier2 CmdPipelineBarrier2 = nullptr;
        void load(const Context& c) {
#define LOAD(n)                                                                                    \
    n = reinterpret_cast<PFN_vk##n>(c.device_proc(c.device, "vk" #n));                             \
    if (!n)                                                                                        \
        throw std::runtime_error("missing native device entry point vk" #n);
            DEVICE_FUNCTIONS(LOAD)
#undef LOAD
            CmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
                c.device_proc(c.device, "vkCmdPipelineBarrier2KHR")
            );
            if (!CmdPipelineBarrier2)
                CmdPipelineBarrier2 = reinterpret_cast<PFN_vkCmdPipelineBarrier2>(
                    c.device_proc(c.device, "vkCmdPipelineBarrier2")
                );
            if (!CmdPipelineBarrier2)
                throw std::runtime_error("missing synchronization2 entry point");
        }
    };
#undef DEVICE_FUNCTIONS
} // namespace

struct NativeOpticalFlow::State {
    Context c;
    Dispatch f;
    mutable std::mutex mutex;
    uint32_t width = 0, height = 0, fw = 0, fh = 0;
    VkDeviceSize bytes = 0;
    VkPhysicalDeviceMemoryProperties memory {};
    Image input, reference, motion, cost;
    std::array<Buffer, 2> packed {};
    VkOpticalFlowSessionNV session = VK_NULL_HANDLE;
    VkCommandPool render_pool = VK_NULL_HANDLE, optical_pool = VK_NULL_HANDLE;
    std::array<VkCommandBuffer, 3> commands {};
    std::array<VkFence, 3> fences {};
    VkSemaphore inputs_ready = VK_NULL_HANDLE, results_ready = VK_NULL_HANDLE;
    bool idle = true, poisoned = false;
    int last_submitted = -1;

    explicit State(const Context& context)
        : c(context) { }
    ~State() {
        if (!idle) {
            if (c.log)
                c.log(c.user, "native optical flow: pending work, retaining owned resources");
            return;
        }
        if (session)
            f.DestroyOpticalFlowSessionNV(c.device, session, nullptr);
        for (auto* i : { &input, &reference, &motion, &cost }) {
            if (i->view)
                f.DestroyImageView(c.device, i->view, nullptr);
            if (i->handle)
                f.DestroyImage(c.device, i->handle, nullptr);
            if (i->memory)
                f.FreeMemory(c.device, i->memory, nullptr);
        }
        for (auto& b : packed) {
            if (b.handle)
                f.DestroyBuffer(c.device, b.handle, nullptr);
            if (b.memory)
                f.FreeMemory(c.device, b.memory, nullptr);
        }
        for (auto fence : fences)
            if (fence)
                f.DestroyFence(c.device, fence, nullptr);
        if (inputs_ready)
            f.DestroySemaphore(c.device, inputs_ready, nullptr);
        if (results_ready)
            f.DestroySemaphore(c.device, results_ready, nullptr);
        if (render_pool)
            f.DestroyCommandPool(c.device, render_pool, nullptr);
        if (optical_pool)
            f.DestroyCommandPool(c.device, optical_pool, nullptr);
    }
    uint32_t memory_type(uint32_t bits) {
        for (uint32_t i = 0; i < memory.memoryTypeCount; ++i)
            if ((bits & (1u << i))
                && (memory.memoryTypes[i].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
                return i;
        throw std::runtime_error("no device-local native optical-flow memory");
    }
    bool supports(VkOpticalFlowUsageFlagsNV usage, VkFormat wanted) {
        VkOpticalFlowImageFormatInfoNV q { VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV };
        q.usage = usage;
        uint32_t count = 0;
        checked(
            c.optical_formats(c.physical, &q, &count, nullptr), "count native optical-flow formats"
        );
        std::vector<VkOpticalFlowImageFormatPropertiesNV> v(
            count, { VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_PROPERTIES_NV }
        );
        checked(
            c.optical_formats(c.physical, &q, &count, v.data()), "read native optical-flow formats"
        );
        for (uint32_t i = 0; i < count; ++i)
            if (v[i].format == wanted)
                return true;
        return false;
    }
    void make_image(
        Image& out,
        uint32_t w,
        uint32_t h,
        VkFormat format,
        VkOpticalFlowUsageFlagsNV usage,
        bool shared
    ) {
        VkOpticalFlowImageFormatInfoNV of { VK_STRUCTURE_TYPE_OPTICAL_FLOW_IMAGE_FORMAT_INFO_NV };
        of.usage = usage;
        VkPhysicalDeviceImageFormatInfo2 q {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2
        };
        q.pNext = &of;
        q.format = format;
        q.type = VK_IMAGE_TYPE_2D;
        q.tiling = VK_IMAGE_TILING_OPTIMAL;
        q.usage = VK_IMAGE_USAGE_SAMPLED_BIT
            | (usage == VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV ? VK_IMAGE_USAGE_TRANSFER_DST_BIT
                                                           : VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
        VkImageFormatProperties2 supported { VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2 };
        checked(
            c.image_properties(c.physical, &q, &supported), "query native estimator image format"
        );
        if (w > supported.imageFormatProperties.maxExtent.width
            || h > supported.imageFormatProperties.maxExtent.height)
            throw std::runtime_error("native estimator image exceeds format extent");
        VkImageCreateInfo info { VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO };
        info.pNext = &of;
        info.imageType = VK_IMAGE_TYPE_2D;
        info.format = format;
        info.extent = { w, h, 1 };
        info.mipLevels = info.arrayLayers = 1;
        info.samples = VK_SAMPLE_COUNT_1_BIT;
        info.tiling = q.tiling;
        info.usage = q.usage;
        uint32_t families[] = { c.render_family, c.optical_family };
        if (shared && families[0] != families[1]) {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        }
        checked(
            f.CreateImage(c.device, &info, nullptr, &out.handle), "create native estimator image"
        );
        VkMemoryRequirements req {};
        f.GetImageMemoryRequirements(c.device, out.handle, &req);
        VkMemoryDedicatedAllocateInfo dedicated {
            VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO
        };
        dedicated.image = out.handle;
        VkMemoryAllocateInfo alloc { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        alloc.pNext = &dedicated;
        alloc.allocationSize = req.size;
        alloc.memoryTypeIndex = memory_type(req.memoryTypeBits);
        checked(
            f.AllocateMemory(c.device, &alloc, nullptr, &out.memory),
            "allocate native estimator image"
        );
        checked(
            f.BindImageMemory(c.device, out.handle, out.memory, 0), "bind native estimator image"
        );
        VkImageViewCreateInfo view { VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO };
        view.image = out.handle;
        view.viewType = VK_IMAGE_VIEW_TYPE_2D;
        view.format = format;
        view.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        checked(
            f.CreateImageView(c.device, &view, nullptr, &out.view), "create native estimator view"
        );
    }
    void make_buffer(Buffer& out) {
        VkBufferCreateInfo info { VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO };
        info.size = bytes;
        info.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
        uint32_t families[] = { c.render_family, c.optical_family };
        if (families[0] != families[1]) {
            info.sharingMode = VK_SHARING_MODE_CONCURRENT;
            info.queueFamilyIndexCount = 2;
            info.pQueueFamilyIndices = families;
        }
        checked(
            f.CreateBuffer(c.device, &info, nullptr, &out.handle), "create native packed buffer"
        );
        VkMemoryRequirements req {};
        f.GetBufferMemoryRequirements(c.device, out.handle, &req);
        VkMemoryAllocateInfo a { VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO };
        a.allocationSize = req.size;
        a.memoryTypeIndex = memory_type(req.memoryTypeBits);
        checked(
            f.AllocateMemory(c.device, &a, nullptr, &out.memory), "allocate native packed buffer"
        );
        checked(
            f.BindBufferMemory(c.device, out.handle, out.memory, 0), "bind native packed buffer"
        );
    }
    void allocate_command(VkCommandPool pool, VkCommandBuffer& out) {
        VkCommandBufferAllocateInfo a { VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO };
        a.commandPool = pool;
        a.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
        a.commandBufferCount = 1;
        checked(f.AllocateCommandBuffers(c.device, &a, &out), "allocate native command buffer");
        if (c.set_loader_data)
            checked(c.set_loader_data(c.device, out), "initialize layer command-buffer dispatch");
    }
    void initialize(uint32_t w, uint32_t h, VkOpticalFlowPerformanceLevelNV performance) {
        if (!c.physical || !c.device || !c.render_queue || !c.optical_queue || !c.device_proc
            || !c.memory_properties || !c.properties || !c.queue_properties || !c.image_properties
            || !c.optical_formats || !c.optical_flow_enabled || !c.synchronization2_enabled
            || bool(c.lock) != bool(c.unlock) || !c.timeout_ns || c.timeout_ns > 5'000'000'000ull
            || !w || !h || w % 4 || h % 4)
            throw std::runtime_error("invalid borrowed native context, dimensions or timeout");
        if (performance != VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV
            && performance != VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_MEDIUM_NV
            && performance != VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_FAST_NV)
            throw std::runtime_error("invalid optical-flow performance level");
        f.load(c);
        uint32_t count = 0;
        c.queue_properties(c.physical, &count, nullptr);
        std::vector<VkQueueFamilyProperties> families(count);
        c.queue_properties(c.physical, &count, families.data());
        if (c.render_family >= count || c.optical_family >= count
            || !(
                families[c.render_family].queueFlags
                & (VK_QUEUE_GRAPHICS_BIT | VK_QUEUE_COMPUTE_BIT | VK_QUEUE_TRANSFER_BIT)
            )
            || (families[c.optical_family].queueFlags
                & (VK_QUEUE_OPTICAL_FLOW_BIT_NV | VK_QUEUE_TRANSFER_BIT))
                != (VK_QUEUE_OPTICAL_FLOW_BIT_NV | VK_QUEUE_TRANSFER_BIT))
            throw std::runtime_error("invalid native queue families");
        VkPhysicalDeviceOpticalFlowPropertiesNV caps {
            VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_OPTICAL_FLOW_PROPERTIES_NV
        };
        VkPhysicalDeviceProperties2 props { VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2 };
        props.pNext = &caps;
        c.properties(c.physical, &props);
        if (!caps.costSupported
            || !(caps.supportedOutputGridSizes & VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV)
            || w < caps.minWidth || h < caps.minHeight || w > caps.maxWidth || h > caps.maxHeight
            || !supports(VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, VK_FORMAT_R8_UNORM)
            || !supports(VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, VK_FORMAT_R16G16_S10_5_NV)
            || !supports(VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, VK_FORMAT_R32_UINT))
            throw std::runtime_error(
                "borrowed device lacks required native optical-flow capabilities"
            );
        width = w;
        height = h;
        fw = w / 4;
        fh = h / 4;
        bytes = VkDeviceSize(fw) * fh * 4;
        c.memory_properties(c.physical, &memory);
        make_image(input, w, h, VK_FORMAT_R8_UNORM, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, true);
        make_image(reference, w, h, VK_FORMAT_R8_UNORM, VK_OPTICAL_FLOW_USAGE_INPUT_BIT_NV, true);
        make_image(
            motion, fw, fh, VK_FORMAT_R16G16_S10_5_NV, VK_OPTICAL_FLOW_USAGE_OUTPUT_BIT_NV, false
        );
        make_image(cost, fw, fh, VK_FORMAT_R32_UINT, VK_OPTICAL_FLOW_USAGE_COST_BIT_NV, false);
        for (auto& b : packed)
            make_buffer(b);
        VkOpticalFlowSessionCreateInfoNV info {
            VK_STRUCTURE_TYPE_OPTICAL_FLOW_SESSION_CREATE_INFO_NV
        };
        info.width = w;
        info.height = h;
        info.imageFormat = VK_FORMAT_R8_UNORM;
        info.flowVectorFormat = VK_FORMAT_R16G16_S10_5_NV;
        info.costFormat = VK_FORMAT_R32_UINT;
        info.outputGridSize = VK_OPTICAL_FLOW_GRID_SIZE_4X4_BIT_NV;
        info.performanceLevel = performance;
        info.flags = VK_OPTICAL_FLOW_SESSION_CREATE_ENABLE_COST_BIT_NV;
        checked(
            f.CreateOpticalFlowSessionNV(c.device, &info, nullptr, &session),
            "create borrowed native session"
        );
        checked(
            f.BindOpticalFlowSessionImageNV(
                c.device,
                session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_INPUT_NV,
                input.view,
                VK_IMAGE_LAYOUT_GENERAL
            ),
            "bind native input"
        );
        checked(
            f.BindOpticalFlowSessionImageNV(
                c.device,
                session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_REFERENCE_NV,
                reference.view,
                VK_IMAGE_LAYOUT_GENERAL
            ),
            "bind native reference"
        );
        checked(
            f.BindOpticalFlowSessionImageNV(
                c.device,
                session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_FLOW_VECTOR_NV,
                motion.view,
                VK_IMAGE_LAYOUT_GENERAL
            ),
            "bind native motion"
        );
        checked(
            f.BindOpticalFlowSessionImageNV(
                c.device,
                session,
                VK_OPTICAL_FLOW_SESSION_BINDING_POINT_COST_NV,
                cost.view,
                VK_IMAGE_LAYOUT_GENERAL
            ),
            "bind native cost"
        );
        VkCommandPoolCreateInfo pool { VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO };
        pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
        pool.queueFamilyIndex = c.render_family;
        checked(
            f.CreateCommandPool(c.device, &pool, nullptr, &render_pool), "create native render pool"
        );
        pool.queueFamilyIndex = c.optical_family;
        checked(
            f.CreateCommandPool(c.device, &pool, nullptr, &optical_pool),
            "create native optical pool"
        );
        allocate_command(render_pool, commands[0]);
        allocate_command(optical_pool, commands[1]);
        allocate_command(render_pool, commands[2]);
        VkFenceCreateInfo fi { VK_STRUCTURE_TYPE_FENCE_CREATE_INFO };
        for (auto& fence : fences)
            checked(f.CreateFence(c.device, &fi, nullptr, &fence), "create native fence");
        VkSemaphoreCreateInfo si { VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO };
        checked(
            f.CreateSemaphore(c.device, &si, nullptr, &inputs_ready),
            "create native input semaphore"
        );
        checked(
            f.CreateSemaphore(c.device, &si, nullptr, &results_ready),
            "create native result semaphore"
        );
    }
    void image_barrier(
        VkCommandBuffer cmd,
        VkImage image,
        VkImageLayout old,
        VkImageLayout next,
        VkPipelineStageFlags2 before,
        VkAccessFlags2 source,
        VkPipelineStageFlags2 after,
        VkAccessFlags2 target
    ) {
        VkImageMemoryBarrier2 b { VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER_2 };
        b.srcStageMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? VK_PIPELINE_STAGE_2_NONE : before;
        b.srcAccessMask = old == VK_IMAGE_LAYOUT_UNDEFINED ? 0 : source;
        b.dstStageMask = after;
        b.dstAccessMask = target;
        b.oldLayout = old;
        b.newLayout = next;
        b.srcQueueFamilyIndex = b.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        b.image = image;
        b.subresourceRange = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1 };
        VkDependencyInfo d { VK_STRUCTURE_TYPE_DEPENDENCY_INFO };
        d.imageMemoryBarrierCount = 1;
        d.pImageMemoryBarriers = &b;
        f.CmdPipelineBarrier2(cmd, &d);
    }
    void own_barrier(
        VkCommandBuffer cmd,
        Image& image,
        VkImageLayout next,
        VkPipelineStageFlags2 before,
        VkAccessFlags2 source,
        VkPipelineStageFlags2 after,
        VkAccessFlags2 target
    ) {
        image_barrier(cmd, image.handle, image.layout, next, before, source, after, target);
        image.layout = next;
    }
    void validate(const ImageResource& i, bool output) {
        bool layout_ok = i.layout == VK_IMAGE_LAYOUT_GENERAL
            || (output && i.layout == VK_IMAGE_LAYOUT_UNDEFINED)
            || (i.layout == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                && (i.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
            || (i.layout == VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL
                && (i.usage & VK_IMAGE_USAGE_TRANSFER_DST_BIT))
            || (i.layout == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                && (i.usage & VK_IMAGE_USAGE_SAMPLED_BIT))
            || (i.layout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL
                && (i.usage & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT));
        if (i.device != c.device || !i.image || i.width != (output ? fw : width)
            || i.height != (output ? fh : height) || i.mip_levels != 1 || i.array_layers != 1
            || i.samples != VK_SAMPLE_COUNT_1_BIT
            || !(
                i.usage
                & (output ? VK_IMAGE_USAGE_TRANSFER_DST_BIT : VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
            )
            || (i.sharing == VK_SHARING_MODE_EXCLUSIVE
                    ? i.owner_family != c.render_family
                    : i.sharing != VK_SHARING_MODE_CONCURRENT || !i.render_accessible)
            || (output ? i.format != VK_FORMAT_R16G16_SINT
                       : i.format != VK_FORMAT_R8_UNORM
                        && i.format != VK_FORMAT_G8_B8R8_2PLANE_420_UNORM)
            || !layout_ok)
            throw std::runtime_error(
                "unusable native image: device, dimensions, format, usage, owner or layout"
            );
    }
    void begin(size_t i) {
        checked(f.ResetCommandBuffer(commands[i], 0), "reset native commands");
        VkCommandBufferBeginInfo b { VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO };
        b.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
        checked(f.BeginCommandBuffer(commands[i], &b), "begin native commands");
    }
    void prepare_inputs(const ImageResource& a, const ImageResource& b) {
        begin(0);
        const ImageResource* sources[] = { &a, &b };
        Image* targets[] = { &input, &reference };
        for (uint32_t i = 0; i < 2; ++i) {
            const auto& src = *sources[i];
            auto& dst = *targets[i];
            image_barrier(
                commands[0],
                src.image,
                src.layout,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_WRITE_BIT | VK_ACCESS_2_MEMORY_READ_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT
            );
            own_barrier(
                commands[0],
                dst,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT
            );
            VkImageCopy copy {};
            copy.srcSubresource = { src.format == VK_FORMAT_R8_UNORM
                                        ? VkImageAspectFlags(VK_IMAGE_ASPECT_COLOR_BIT)
                                        : VkImageAspectFlags(VK_IMAGE_ASPECT_PLANE_0_BIT),
                                    0,
                                    0,
                                    1 };
            copy.dstSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.extent = { width, height, 1 };
            f.CmdCopyImage(
                commands[0],
                src.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                dst.handle,
                dst.layout,
                1,
                &copy
            );
            own_barrier(
                commands[0],
                dst,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_NONE,
                0
            );
            image_barrier(
                commands[0],
                src.image,
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                src.layout,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
            );
        }
        checked(f.EndCommandBuffer(commands[0]), "end native input copy");
    }
    void estimate() {
        begin(1);
        for (auto* i : { &motion, &cost })
            own_barrier(
                commands[1],
                *i,
                VK_IMAGE_LAYOUT_GENERAL,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT,
                VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
                VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV
            );
        VkOpticalFlowExecuteInfoNV run { VK_STRUCTURE_TYPE_OPTICAL_FLOW_EXECUTE_INFO_NV };
        run.flags = VK_OPTICAL_FLOW_EXECUTE_DISABLE_TEMPORAL_HINTS_BIT_NV;
        f.CmdOpticalFlowExecuteNV(commands[1], session, &run);
        Image* images[] = { &motion, &cost };
        for (uint32_t i = 0; i < 2; ++i) {
            own_barrier(
                commands[1],
                *images[i],
                VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
                VK_PIPELINE_STAGE_2_OPTICAL_FLOW_BIT_NV,
                VK_ACCESS_2_OPTICAL_FLOW_WRITE_BIT_NV,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_READ_BIT
            );
            VkBufferImageCopy copy {};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { fw, fh, 1 };
            f.CmdCopyImageToBuffer(
                commands[1], images[i]->handle, images[i]->layout, packed[i].handle, 1, &copy
            );
        }
        checked(f.EndCommandBuffer(commands[1]), "end native estimate");
    }
    void finish(const ImageResource& a, const ImageResource& b, VkImageLayout after) {
        begin(2);
        const ImageResource* targets[] = { &a, &b };
        for (uint32_t i = 0; i < 2; ++i) {
            const auto& dst = *targets[i];
            image_barrier(
                commands[2],
                dst.image,
                dst.layout,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT
            );
            VkBufferImageCopy copy {};
            copy.imageSubresource = { VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1 };
            copy.imageExtent = { fw, fh, 1 };
            f.CmdCopyBufferToImage(
                commands[2],
                packed[i].handle,
                dst.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                1,
                &copy
            );
            image_barrier(
                commands[2],
                dst.image,
                VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL,
                after,
                VK_PIPELINE_STAGE_2_COPY_BIT,
                VK_ACCESS_2_TRANSFER_WRITE_BIT,
                VK_PIPELINE_STAGE_2_ALL_COMMANDS_BIT,
                VK_ACCESS_2_MEMORY_READ_BIT | VK_ACCESS_2_MEMORY_WRITE_BIT
            );
        }
        checked(f.EndCommandBuffer(commands[2]), "end native output copy");
    }
    void submit(
        size_t i, VkQueue queue, VkSemaphore wait, VkPipelineStageFlags stage, VkSemaphore signal
    ) {
        VkSubmitInfo s { VK_STRUCTURE_TYPE_SUBMIT_INFO };
        s.commandBufferCount = 1;
        s.pCommandBuffers = &commands[i];
        if (wait) {
            s.waitSemaphoreCount = 1;
            s.pWaitSemaphores = &wait;
            s.pWaitDstStageMask = &stage;
        }
        if (signal) {
            s.signalSemaphoreCount = 1;
            s.pSignalSemaphores = &signal;
        }
        checked(f.QueueSubmit(queue, 1, &s, fences[i]), "submit native phase");
        idle = false;
        last_submitted = int(i);
    }
    void wait() {
        if (idle)
            return;
        checked(
            f.WaitForFences(c.device, 1, &fences[size_t(last_submitted)], VK_TRUE, c.timeout_ns),
            "wait for native phase"
        );
        idle = true;
    }
};

NativeOpticalFlow::NativeOpticalFlow(
    const Context& c, uint32_t w, uint32_t h, VkOpticalFlowPerformanceLevelNV perf
)
    : state(std::make_unique<State>(c)) {
    Scope scope(c);
    state->initialize(w, h, perf);
}
NativeOpticalFlow::~NativeOpticalFlow() = default;
bool NativeOpticalFlow::execute(
    const ImageResource& input,
    const ImageResource& reference,
    ImageResource& motion,
    ImageResource& cost,
    std::string& error,
    VkImageLayout after
) noexcept {
    auto& s = *state;
    std::lock_guard lock(s.mutex);
    try {
        if (s.poisoned || !s.idle)
            throw std::runtime_error("native estimator faulted or busy");
        s.validate(input, false);
        s.validate(reference, false);
        s.validate(motion, true);
        s.validate(cost, true);
        if (input.image == reference.image || motion.image == cost.image
            || input.image == motion.image || input.image == cost.image
            || reference.image == motion.image || reference.image == cost.image
            || (after != VK_IMAGE_LAYOUT_GENERAL
                && after != VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                && after != VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                && after != VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL)
            || (after == VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL
                && (!(motion.usage & VK_IMAGE_USAGE_SAMPLED_BIT)
                    || !(cost.usage & VK_IMAGE_USAGE_SAMPLED_BIT)))
            || (after == VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL
                && (!(motion.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT)
                    || !(cost.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))))
            throw std::runtime_error("invalid native resource alias or destination layout");
        Scope scope(s.c);
        // Record all phases before submission so ordinary validation/recording
        // errors cannot leave a semaphore chain half submitted.
        s.poisoned = true;
        s.prepare_inputs(input, reference);
        s.estimate();
        s.finish(motion, cost, after);
        checked(
            s.f.ResetFences(s.c.device, s.fences.size(), s.fences.data()), "reset native fences"
        );
        try {
            s.submit(0, s.c.render_queue, VK_NULL_HANDLE, 0, s.inputs_ready);
            s.submit(
                1,
                s.c.optical_queue,
                s.inputs_ready,
                VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
                s.results_ready
            );
            s.submit(
                2, s.c.render_queue, s.results_ready, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_NULL_HANDLE
            );
            s.wait();
        } catch (...) {
            s.poisoned = true;
            throw;
        }
        s.poisoned = false;
        motion.layout = cost.layout = after;
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        if (s.c.log)
            s.c.log(s.c.user, error.c_str());
        return false;
    }
}
bool NativeOpticalFlow::quiesce(std::string& error) noexcept {
    auto& s = *state;
    std::lock_guard lock(s.mutex);
    try {
        Scope scope(s.c);
        s.wait();
        error.clear();
        return true;
    } catch (const std::exception& e) {
        error = e.what();
        return false;
    }
}
bool NativeOpticalFlow::faulted() const {
    std::lock_guard lock(state->mutex);
    return state->poisoned;
}
} // namespace alvr::motion
