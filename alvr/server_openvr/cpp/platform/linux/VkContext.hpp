#pragma once

#include <algorithm>
#include <cassert>
#include <cstdint>
#include <cstdlib>
#include <cstring>
#include <fcntl.h>
#include <functional>
#include <iostream>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#define VULKAN_HPP_NO_CONSTRUCTORS
#include <vulkan/vulkan.h>
#include <vulkan/vulkan.hpp>
#include <vulkan/vulkan_core.h>
#include <vulkan/vulkan_enums.hpp>
#include <vulkan/vulkan_handles.hpp>
#include <vulkan/vulkan_structs.hpp>

#include "utils.hpp"

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/bindings.h"
#include "GpuDiag.h"

extern "C" {
#include <libavutil/log.h>
}

namespace alvr {

enum class Vendor { Amd, Intel, Nvidia };

class VkContext {
public:
    vk::Instance instance;
    vk::PhysicalDevice physDev;
    vk::Device dev;

    vk::detail::DispatchLoaderDynamic dispatch;

    struct Meta {
        Vendor vendor;

        std::vector<char const*> instExtensions;
        std::vector<char const*> devExtensions;

        vk::PhysicalDeviceVulkan12Features feats12;
        vk::PhysicalDeviceFeatures2 feats;

        u32 queueFamily;
        u32 queueIndex;
    } meta;

private:
    vk::Queue queue;
    VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

    static VkBool32
    validationMessage(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* data, void*) {
        if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
            Error("Vulkan validation: %s", data->pMessage);
        else
            Warn("Vulkan validation: %s", data->pMessage);
        return VK_FALSE;
    }

public:
    // Environment variables that restrict which GPUs a Vulkan or OpenGL
    // process sees or uses, as "NAME=value, ..." for those set in this
    // process ("" if none). SteamVR and its driver inherit them from Steam,
    // so they can defeat a GPU chosen in ALVR's settings; warnings about an
    // unmet GPU choice include this list.
    // Why a requested render GPU was not honoured ("" if it was, or none was
    // requested). The device is chosen once at driver start, when the log is
    // not yet streamed to the dashboard, so the encoder repeats this warning
    // every time it is built (Encoder::resolveTarget).
    inline static std::string renderGpuWarning;

    static std::string gpuOverrideEnvironment() {
        static char const* const names[] = {
            "VK_DRIVER_FILES",
            "VK_ICD_FILENAMES",
            "VK_ADD_DRIVER_FILES",
            "VK_LOADER_DRIVERS_SELECT",
            "VK_LOADER_DRIVERS_DISABLE",
            "VK_LOADER_LAYERS_DISABLE",
            "__VK_LAYER_NV_optimus",
            "MESA_VK_DEVICE_SELECT",
            "DRI_PRIME",
            "__NV_PRIME_RENDER_OFFLOAD",
            "__GLX_VENDOR_LIBRARY_NAME",
            "__EGL_VENDOR_LIBRARY_FILENAMES",
            "ALVR_VULKAN_DEVICE",
        };
        std::string list;
        for (auto name : names) {
            if (char const* value = std::getenv(name)) {
                list += (list.empty() ? "" : ", ") + std::string(name) + "=" + value;
            }
        }
        return list;
    }

    // Finds the device a user-written selector names: its index in the
    // list, or part of its name (case-insensitive). Shared by the render-GPU
    // and encoder-GPU settings.
    static std::optional<usize>
    findPhysicalDevice(std::vector<vk::PhysicalDevice> const& devs, std::string_view selector) {
        if (selector.empty()) {
            return std::nullopt;
        }
        std::string wanted(selector);
        std::transform(wanted.begin(), wanted.end(), wanted.begin(), ::tolower);
        for (usize i = 0; i < devs.size(); ++i) {
            std::string name = devs[i].getProperties().deviceName.data();
            std::transform(name.begin(), name.end(), name.begin(), ::tolower);
            if (wanted == std::to_string(i) || name.find(wanted) != std::string::npos) {
                return i;
            }
        }
        return std::nullopt;
    }

private:
    // The render GPU: ALVR's Vulkan device must be the GPU SteamVR renders
    // on, because SteamVR shares its textures as opaque fds, which import
    // only on the GPU that created them, and SteamVR does not tell the driver
    // which one that is. Order: ALVR_VULKAN_DEVICE, then the "Render GPU"
    // setting (video.encoder_config.linux_gpu.render_gpu), then automatic:
    // the system's order (the first hardware device, as upstream ALVR's
    // devs[0]; a CPU implementation such as llvmpipe listed first is skipped).
    static vk::PhysicalDevice selectPhysicalDevice(std::vector<vk::PhysicalDevice> const& devs) {
        if (devs.empty()) {
            throw std::runtime_error("No Vulkan device found");
        }
        for (usize i = 0; i < devs.size(); ++i) {
            auto props = devs[i].getProperties();
            Info(
                "Vulkan device %zu: %s (%s)",
                i,
                props.deviceName.data(),
                vk::to_string(props.deviceType).c_str()
            );
        }

        struct Source {
            char const* label;
            char const* selector;
        };
        Settings const* settings = Settings_Instance();
        Source const sources[] = {
            { "ALVR_VULKAN_DEVICE", std::getenv("ALVR_VULKAN_DEVICE") },
            { "Render GPU setting", settings ? settings->m_renderGpu : nullptr },
        };
        for (auto const& source : sources) {
            if (!source.selector || !source.selector[0]) {
                continue;
            }
            if (auto index = findPhysicalDevice(devs, source.selector)) {
                Info("Using Vulkan device %zu (%s)", *index, source.label);
                return devs[*index];
            }
            std::string env = gpuOverrideEnvironment();
            renderGpuWarning = std::string(source.label) + " \"" + source.selector
                + "\" matches no Vulkan device, so it cannot be honoured; GPU-restricting "
                  "environment inherited from Steam/SteamVR: "
                + (env.empty() ? "none" : env);
            Warn("%s", renderGpuWarning.c_str());
        }

        // Automatic = what the system presents, as ALVR always did: the first
        // device in the Vulkan loader's order, which already follows the
        // system's defaults and any GPU environment Steam/SteamVR was started
        // with (prime-run, DRI_PRIME, ...). No discrete-first preference: on a
        // hybrid laptop that moved the driver to the NVIDIA GPU while Steam,
        // started without the selector's environment, stayed on the integrated
        // one. Only a CPU implementation (llvmpipe) listed first is skipped
        // when a hardware device exists. (User decision, 2026-10-25.)
        for (usize i = 0; i < devs.size(); ++i) {
            if (devs[i].getProperties().deviceType != vk::PhysicalDeviceType::eCpu) {
                Info("Using Vulkan device %zu (automatic: system order)", i);
                return devs[i];
            }
        }
        return devs[0];
    }

    void sharedInit() {
        auto devProps = physDev.getProperties2();

        if (devProps.properties.vendorID == 0x1002)
            meta.vendor = Vendor::Amd;
        else if (devProps.properties.vendorID == 0x8086)
            meta.vendor = Vendor::Intel;
        else if (devProps.properties.vendorID == 0x10de)
            meta.vendor = Vendor::Nvidia;

        // Which driver drives it (for example NVIDIA's proprietary driver or
        // Mesa's NVK on the same NVIDIA GPU).
        auto driverProps = physDev
                               .getProperties2<
                                   vk::PhysicalDeviceProperties2,
                                   vk::PhysicalDeviceDriverProperties>()
                               .get<vk::PhysicalDeviceDriverProperties>();
        Info(
            "Vulkan render device: %s, driver %s (%s)",
            devProps.properties.deviceName.data(),
            driverProps.driverName.data(),
            driverProps.driverInfo.data()
        );

        dispatch = { instance, vkGetInstanceProcAddr };
    }

public:
    VkContext(std::vector<u8> deviceUUID) {
        std::vector<std::string_view> wantedInstExts = {
            VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME,
        };

        auto availInstExts = vk::enumerateInstanceExtensionProperties();
        std::vector<const char*> acquiredInstExts;
        for (auto wantedName : wantedInstExts) {
            auto it = std::find_if(availInstExts.begin(), availInstExts.end(), [&](auto& ext) {
                return wantedName == ext.extensionName;
            });
            if (it != availInstExts.end())
                acquiredInstExts.push_back(wantedName.data());
            else
                assert(false);
        }

        // ALVR_VULKAN_VALIDATION=1 enables the Khronos validation layer on
        // ALVR's own instance only, so its messages are told apart from
        // SteamVR's in the ALVR log.
        std::vector<const char*> layers;
        bool validation = std::getenv("ALVR_VULKAN_VALIDATION") != nullptr;
        if (validation) {
            auto availLayers = vk::enumerateInstanceLayerProperties();
            bool hasLayer = std::any_of(availLayers.begin(), availLayers.end(), [](auto& layer) {
                return std::string_view(layer.layerName) == "VK_LAYER_KHRONOS_validation";
            });
            bool hasDebugUtils
                = std::any_of(availInstExts.begin(), availInstExts.end(), [](auto& ext) {
                      return std::string_view(ext.extensionName)
                          == VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
                  });
            if (hasLayer && hasDebugUtils) {
                layers.push_back("VK_LAYER_KHRONOS_validation");
                acquiredInstExts.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
            } else {
                Warn("ALVR_VULKAN_VALIDATION set but the validation layer is not available");
                validation = false;
            }
        }

        vk::ApplicationInfo appInfo {
            .pApplicationName = "ALVR",
            .apiVersion = VK_API_VERSION_1_2,
        };
        vk::InstanceCreateInfo instanceCI {
            .pApplicationInfo = &appInfo,
            .enabledLayerCount = static_cast<uint32_t>(layers.size()),
            .ppEnabledLayerNames = layers.data(),
            .enabledExtensionCount = static_cast<uint32_t>(acquiredInstExts.size()),
            .ppEnabledExtensionNames = acquiredInstExts.data(),
        };
        instance = vk::createInstance(instanceCI);

        if (validation) {
            auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT")
            );
            VkDebugUtilsMessengerCreateInfoEXT messengerCI {
                .sType = VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT,
                .pNext = nullptr,
                .flags = 0,
                .messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT,
                .messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT
                    | VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT,
                .pfnUserCallback = validationMessage,
                .pUserData = nullptr,
            };
            if (!create || create(instance, &messengerCI, nullptr, &messenger) != VK_SUCCESS)
                Warn("Failed to create the Vulkan validation messenger");
            else
                Info("Vulkan validation enabled for ALVR's instance");
        }
        auto physDevs = instance.enumeratePhysicalDevices();

        physDev = selectPhysicalDevice(physDevs);
        {
            // Diagnostics: every device and the one the selector took (the
            // reason is on the "Using Vulkan device" line above).
            size_t chosen = 0;
            for (size_t i = 0; i < physDevs.size(); ++i)
                if (physDevs[i] == physDev)
                    chosen = i;
            alvr::gpudiag::logDevices(physDevs, chosen, "GPU selector: ALVR_VULKAN_DEVICE, Render GPU setting, else the system's order");
        }

        auto queueFamilyProps = physDev.getQueueFamilyProperties();

        std::optional<u32> wantedQueueFamily;

        for (u32 i = 0; i < queueFamilyProps.size(); ++i) {
            auto& props = queueFamilyProps[i];
            bool isGraphics = static_cast<bool>(props.queueFlags & vk::QueueFlagBits::eGraphics);
            bool isCompute = static_cast<bool>(props.queueFlags & vk::QueueFlagBits::eCompute);

            if (isCompute && (!wantedQueueFamily.has_value() || !isGraphics)) {
                wantedQueueFamily = i;
            }
        }
        meta.queueFamily = wantedQueueFamily.value();
        meta.queueIndex = 0;

        std::vector<std::string_view> wantedExts = {
            VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_DMA_BUF_EXTENSION_NAME,
            VK_EXT_IMAGE_DRM_FORMAT_MODIFIER_EXTENSION_NAME,
            VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME,
            VK_EXT_EXTERNAL_MEMORY_HOST_EXTENSION_NAME,
            VK_KHR_PUSH_DESCRIPTOR_EXTENSION_NAME,
            VK_KHR_SAMPLER_YCBCR_CONVERSION_EXTENSION_NAME,
            VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME,
            VK_EXT_CALIBRATED_TIMESTAMPS_EXTENSION_NAME,
            VK_KHR_COPY_COMMANDS_2_EXTENSION_NAME,
        };

        auto availExts = physDev.enumerateDeviceExtensionProperties();

        std::vector<char const*> acquiredExts;
        for (auto name : wantedExts) {
            auto it = std::find_if(availExts.begin(), availExts.end(), [&](auto& other) {
                return name == (const char*)other.extensionName;
            });
            if (it != availExts.end())
                acquiredExts.push_back(name.data());
        }

        f32 queuePrio = 1.f;
        vk::DeviceQueueCreateInfo queueCI {
            .queueFamilyIndex = meta.queueFamily,
            .queueCount = 1,
            .pQueuePriorities = &queuePrio,
        };

        meta.feats12 = vk::PhysicalDeviceVulkan12Features {
            .timelineSemaphore = 1,
        };

        meta.feats = vk::PhysicalDeviceFeatures2 {
            .pNext = &meta.feats12,
            .features = {
                .robustBufferAccess = true,
                .samplerAnisotropy = true,
            },
        };
        vk::DeviceCreateInfo devCI {
            .pNext = &meta.feats,
            .queueCreateInfoCount = 1,
            .pQueueCreateInfos = &queueCI,
            .enabledExtensionCount = static_cast<u32>(acquiredExts.size()),
            .ppEnabledExtensionNames = acquiredExts.data(),
        };
        dev = physDev.createDevice(devCI);

        meta.devExtensions = acquiredExts;

        queue = dev.getQueue(wantedQueueFamily.value(), meta.queueIndex);

        sharedInit();
    }

    void useQueue(std::function<void(vk::Queue&)> fn) { fn(queue); }

    void destroy() {
        dev.destroy();
        if (messenger) {
            auto destroyMessenger = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
                vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT")
            );
            if (destroyMessenger)
                destroyMessenger(instance, messenger, nullptr);
        }
        instance.destroy();
    }
};

}
