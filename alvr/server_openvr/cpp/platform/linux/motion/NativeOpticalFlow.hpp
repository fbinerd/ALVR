#pragma once
// Native estimator for a caller-owned Linux Vulkan device. No process hooks here.
#include <cstdint>
#include <memory>
#include <string>
#include <vulkan/vk_layer.h>
#include <vulkan/vulkan.h>

namespace alvr::motion {
struct Context {
    VkPhysicalDevice physical = VK_NULL_HANDLE;
    VkDevice device = VK_NULL_HANDLE;
    VkQueue render_queue = VK_NULL_HANDLE, optical_queue = VK_NULL_HANDLE;
    uint32_t render_family = UINT32_MAX, optical_family = UINT32_MAX;
    PFN_vkGetDeviceProcAddr device_proc = nullptr;
    PFN_vkGetPhysicalDeviceMemoryProperties memory_properties = nullptr;
    PFN_vkGetPhysicalDeviceProperties2 properties = nullptr;
    PFN_vkGetPhysicalDeviceQueueFamilyProperties queue_properties = nullptr;
    PFN_vkGetPhysicalDeviceImageFormatProperties2 image_properties = nullptr;
    PFN_vkGetPhysicalDeviceOpticalFlowImageFormatsNV optical_formats = nullptr;
    // A layer bypassing the loader supplies its loader-data callback for owned
    // command buffers. A standalone caller using loader entry points may omit it.
    PFN_vkSetDeviceLoaderData set_loader_data = nullptr;
    bool optical_flow_enabled = false, synchronization2_enabled = false;
    uint64_t timeout_ns = 500'000'000;
    // The caller must serialize application submissions/resource destruction
    // with this whole operation, not just serialize this estimator's calls.
    void* user = nullptr;
    void (*lock)(void*) = nullptr;
    void (*unlock)(void*) = nullptr;
    void (*log)(void*, const char*) = nullptr;
};

struct ImageResource {
    VkDevice device = VK_NULL_HANDLE;
    VkImage image = VK_NULL_HANDLE;
    VkFormat format = VK_FORMAT_UNDEFINED;
    uint32_t width = 0, height = 0;
    VkImageUsageFlags usage = 0;
    VkImageLayout layout = VK_IMAGE_LAYOUT_UNDEFINED;
    VkSharingMode sharing = VK_SHARING_MODE_EXCLUSIVE;
    uint32_t owner_family = UINT32_MAX;
    // For concurrent images the registry must verify render_family occurs
    // in the creation family list, or is the only permitted family.
    bool render_accessible = false;
    uint32_t mip_levels = 1, array_layers = 1;
    VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
};

class NativeOpticalFlow {
    struct State;
    std::unique_ptr<State> state;

public:
    // Throws before use if features, formats, queues or allocation are unavailable.
    NativeOpticalFlow(
        const Context&,
        uint32_t width,
        uint32_t height,
        VkOpticalFlowPerformanceLevelNV = VK_OPTICAL_FLOW_PERFORMANCE_LEVEL_SLOW_NV
    );
    ~NativeOpticalFlow();
    NativeOpticalFlow(const NativeOpticalFlow&) = delete;
    NativeOpticalFlow& operator=(const NativeOpticalFlow&) = delete;

    // Input -> reference forward motion, signed 1/32-pixel XY. NV12 Y or R8
    // input; output and cost use the observed RG16_SINT packed byte contract.
    // Inputs return to their original layout. Outputs return in output_layout
    // and metadata is updated only on successful completion. No host pixel I/O.
    bool execute(
        const ImageResource& input,
        const ImageResource& reference,
        ImageResource& motion,
        ImageResource& cost,
        std::string& error,
        VkImageLayout output_layout = VK_IMAGE_LAYOUT_GENERAL
    ) noexcept;

    // After submission failure/timeout, do not reuse the estimator. The owner
    // must keep the device/images alive until this confirms submitted work ended.
    // If pending work cannot be confirmed finished, destruction leaks owned
    // allocations rather than freeing live resources. It never destroys the device.
    bool quiesce(std::string& error) noexcept;
    bool faulted() const;
};
} // namespace alvr::motion
