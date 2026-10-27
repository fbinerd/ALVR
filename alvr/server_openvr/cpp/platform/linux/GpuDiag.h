#pragma once
// Diagnostics for GPU selection problems (hybrid laptops, mismatched GPUs).
// Every line starts with "[gpu-diag]" so a log can be grepped for them. Each
// helper logs once per call site; nothing here changes behaviour.

#include "alvr_server/Logger.h"

#include <cstdio>
#include <dirent.h>
#include <fstream>
#include <limits.h>
#include <sstream>
#include <string>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>
#include <vector>

// Keep the same Vulkan-Hpp configuration as Renderer.hpp and VkContext.hpp,
// including when this diagnostic header is the first Vulkan include.
#ifndef VULKAN_HPP_NO_CONSTRUCTORS
#define VULKAN_HPP_NO_CONSTRUCTORS
#endif
#include <vulkan/vulkan.hpp>

namespace alvr::gpudiag {

// Environment variables that move a process to one GPU or another.
inline std::vector<char const*> const& gpuVariables() {
    static std::vector<char const*> names = {
        "DRI_PRIME",
        "MESA_VK_DEVICE_SELECT",
        "__NV_PRIME_RENDER_OFFLOAD",
        "__VK_LAYER_NV_optimus",
        "__GLX_VENDOR_LIBRARY_NAME",
        "__EGL_VENDOR_LIBRARY_FILENAMES",
        "VK_ICD_FILENAMES",
        "VK_DRIVER_FILES",
        "VK_ADD_DRIVER_FILES",
        "VK_LOADER_DRIVERS_SELECT",
        "VK_LOADER_DRIVERS_DISABLE",
        "ALVR_VULKAN_DEVICE",
    };
    return names;
}

// GPU variables from /proc/<pid>/environ ("self" for this process).
inline std::string environmentOf(std::string const& pid) {
    std::ifstream file("/proc/" + pid + "/environ", std::ios::binary);
    if (!file)
        return "(unreadable)";
    std::string all((std::istreambuf_iterator<char>(file)), std::istreambuf_iterator<char>());
    std::string out;
    size_t start = 0;
    while (start < all.size()) {
        size_t end = all.find('\0', start);
        if (end == std::string::npos)
            end = all.size();
        std::string entry = all.substr(start, end - start);
        for (auto name : gpuVariables()) {
            std::string prefix = std::string(name) + "=";
            if (entry.rfind(prefix, 0) == 0)
                out += (out.empty() ? "" : " ") + entry;
        }
        start = end + 1;
    }
    return out.empty() ? "(none set)" : out;
}

// Kernel driver bound to a character device (e.g. i915, xe, amdgpu, nvidia).
inline std::string kernelDriverOf(dev_t rdev) {
    char link[64];
    std::snprintf(
        link,
        sizeof(link),
        "/sys/dev/char/%u:%u/device/driver",
        gnu_dev_major(rdev),
        gnu_dev_minor(rdev)
    );
    char target[PATH_MAX] = {};
    ssize_t n = readlink(link, target, sizeof(target) - 1);
    if (n <= 0)
        return "?";
    std::string path(target, n);
    auto slash = path.rfind('/');
    return slash == std::string::npos ? path : path.substr(slash + 1);
}

inline std::string describeNode(std::string const& path) {
    struct stat s { };
    if (stat(path.c_str(), &s) != 0)
        return path + " (missing)";
    std::ostringstream out;
    out << path << " " << gnu_dev_major(s.st_rdev) << ":" << gnu_dev_minor(s.st_rdev)
        << " driver=" << kernelDriverOf(s.st_rdev);
    return out.str();
}

// One line per Vulkan device: type, ids, driver, UUID, DRM nodes.
inline void
logDevices(std::vector<vk::PhysicalDevice> const& devs, size_t chosen, char const* why) {
    for (size_t i = 0; i < devs.size(); ++i) {
        bool hasDrm = false;
        for (auto const& ext : devs[i].enumerateDeviceExtensionProperties())
            hasDrm |= std::string_view(ext.extensionName.data())
                == VK_EXT_PHYSICAL_DEVICE_DRM_EXTENSION_NAME;
        vk::PhysicalDeviceDrmPropertiesEXT drm {};
        vk::PhysicalDeviceDriverProperties driver {};
        vk::PhysicalDeviceIDProperties ids {};
        driver.pNext = &ids;
        if (hasDrm)
            ids.pNext = &drm;
        vk::PhysicalDeviceProperties2 props { .pNext = &driver };
        devs[i].getProperties2(&props);
        auto const& p = props.properties;
        std::ostringstream uuid;
        for (auto b : ids.deviceUUID)
            uuid << std::hex << (b >> 4) << (b & 15);
        std::ostringstream nodes;
        if (!hasDrm)
            nodes << "no VK_EXT_physical_device_drm";
        else
            nodes << "primary "
                  << (drm.hasPrimary ? std::to_string(drm.primaryMajor) + ":"
                              + std::to_string(drm.primaryMinor)
                                     : "-")
                  << " render "
                  << (drm.hasRender
                          ? std::to_string(drm.renderMajor) + ":" + std::to_string(drm.renderMinor)
                          : "-");
        InfoToDriver(
            "[gpu-diag] Vulkan device %zu%s: %s, %s, %04x:%04x, driver %s (%s), uuid %s, %s",
            i,
            i == chosen ? " [CHOSEN]" : "",
            p.deviceName.data(),
            vk::to_string(p.deviceType).c_str(),
            p.vendorID,
            p.deviceID,
            driver.driverName.data(),
            driver.driverInfo.data(),
            uuid.str().c_str(),
            nodes.str().c_str()
        );
    }
    InfoToDriver("[gpu-diag] ALVR uses Vulkan device %zu (%s)", chosen, why);
    InfoToDriver("[gpu-diag] vrserver GPU environment: %s", environmentOf("self").c_str());
}

// The first process whose /proc/<pid>/comm equals name, or "".
inline std::string pidOf(std::string const& name) {
    DIR* dir = opendir("/proc");
    if (!dir)
        return "";
    std::string found;
    while (auto entry = readdir(dir)) {
        std::string pid = entry->d_name;
        if (pid.empty() || pid.find_first_not_of("0123456789") != std::string::npos)
            continue;
        std::ifstream comm("/proc/" + pid + "/comm");
        std::string value;
        if (std::getline(comm, value) && value == name) {
            found = pid;
            break;
        }
    }
    closedir(dir);
    return found;
}

// vrcompositor imports the textures ALVR creates; it must run on the same
// GPU. Its environment and the Vulkan/GL driver libraries it has loaded show
// which GPU it is on.
inline void logCompositor() {
    auto pid = pidOf("vrcompositor");
    if (pid.empty()) {
        InfoToDriver("[gpu-diag] vrcompositor not found");
        return;
    }
    std::ifstream maps("/proc/" + pid + "/maps");
    std::string line, libs;
    if (!maps) {
        InfoToDriver(
            "[gpu-diag] vrcompositor pid %s: process maps and GPU environment are unreadable; "
            "check SteamVR's vrcompositor.txt for its selected GPU",
            pid.c_str()
        );
        return;
    }
    for (char const* marker : { "libnvidia-glcore",
                                "libGLX_nvidia",
                                "libnvidia-gpucomp",
                                "libvulkan_intel",
                                "libvulkan_radeon",
                                "libvulkan_nouveau",
                                "libvulkan_lvp",
                                "libvulkan_asahi" }) {
        maps.clear();
        maps.seekg(0);
        while (std::getline(maps, line)) {
            if (line.find(marker) != std::string::npos) {
                libs += (libs.empty() ? "" : " ") + std::string(marker);
                break;
            }
        }
    }
    InfoToDriver(
        "[gpu-diag] vrcompositor pid %s, GPU environment: %s; GPU driver libraries loaded: %s",
        pid.c_str(),
        environmentOf(pid).c_str(),
        libs.empty() ? "(none found)" : libs.c_str()
    );
}

}
