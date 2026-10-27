#pragma once

#include <climits>

#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <stdexcept>
#include <type_traits>

#include "EncodePipeline.h"
#include "Renderer.hpp"

#include "alvr_server/IDRScheduler.h"
#include "alvr_server/Settings.h"
#include "alvr_server/bindings.h"
#include <unistd.h>

namespace alvr {

// TODO: Move to source file
template <typename... T>
concept floats = (std::is_same_v<float, T> && ...);

template <typename... T>
auto makeSpecs(T... args)
    requires floats<T...>
{
    render::PipelineCreateInfo info {};

    constexpr auto Size = sizeof(float);
    u32 index = 0;

    // NOTE: This only works with little endian (but what pcvr target is big endian anyway)
    auto put = [&](float data) {
        std::array<u8, Size> arr;
        memcpy(arr.data(), reinterpret_cast<u8*>(&data), arr.size());

        auto const offset = static_cast<u32>(info.specData.size());
        info.specData.insert(info.specData.end(), arr.begin(), arr.end());
        info.specs.push_back({
            .constantID = index,
            .offset = offset,
            .size = Size,
        });
        ++index;
    };

    (put(args), ...);

    return info;
}

inline auto makeColorCorrection(Settings const& settings, vk::Extent2D extent) {
    // The order of this needs to be kept in sync with the shader!
    // clang-format off
    auto info = makeSpecs(
        (float)extent.width,
        (float)extent.height,
        settings.m_brightness,
        settings.m_contrast + 1.f,
        settings.m_saturation + 1.f,
        settings.m_gamma,
        settings.m_sharpening);
    // clang-format on

    info.shaderData = std::vector(
        COLOR_SHADER_COMP_SPV_PTR, COLOR_SHADER_COMP_SPV_PTR + COLOR_SHADER_COMP_SPV_LEN
    );

    return info;
}

// The encoder-aligned parameters are computed on the Rust side from the
// negotiated config. The center shifts are not specialization constants: they
// can move every frame and reach the shader as push constants.
inline auto makeFoveation(Settings const& settings) {
    auto const& params = settings.m_foveatedEncoding;

    vk::Extent2D outSize {
        .width = params.encodedViewResolution[0] * 2,
        .height = params.encodedViewResolution[1],
    };

    // The order of this needs to be kept in sync with the shader!
    // clang-format off
    auto info = makeSpecs(
        params.viewRatio[0],
        params.viewRatio[1],
        params.centerSize[0],
        params.centerSize[1],
        params.edgeRatio[0],
        params.edgeRatio[1]);
    // clang-format on

    info.shaderData
        = std::vector(FFR_SHADER_COMP_SPV_PTR, FFR_SHADER_COMP_SPV_PTR + FFR_SHADER_COMP_SPV_LEN);

    return std::tuple(info, outSize);
}

class Encoder {
    vk::Extent2D outExtent;

    VkContext vkCtx;

    Optional<render::Renderer> renderer;

    std::unique_ptr<EncodePipeline> encoder;
    std::unique_ptr<VkFrame> frame;
    bool encoderMissingLogged = false;
    IDRScheduler idrScheduler;

    // SteamVR's per-texture timeline semaphores (SteamVrSync.h), imported
    // into this device once per handle and kept until SteamVR destroys their
    // texture or the encoder goes.
    std::map<u64, vk::Semaphore> timelines;
    bool timelineImportFailureLogged = false;

    // Whether the current pass chain ends in a shader carrying the warp fold,
    // which is ffr.comp or quad.comp. A colour-correction-only chain does not,
    // and warping there would move the frame's stamp while leaving the pixels
    // unwarped, which is worse than not warping at all.
    bool warpCapableChain = false;

    // Resolved in createImages, used by initEncoding.
    EncoderTarget target;

    // DRM render node (/dev/dri/renderD*) of a Vulkan device, or "" if the
    // device has none (e.g. llvmpipe) or VK_EXT_physical_device_drm is absent.
    static std::string renderNodeOf(vk::PhysicalDevice physDev) {
        VkPhysicalDeviceDrmPropertiesEXT drmProps = {};
        drmProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_DRM_PROPERTIES_EXT;
        VkPhysicalDeviceProperties2 deviceProps = {};
        deviceProps.sType = VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2;
        deviceProps.pNext = &drmProps;
        vkGetPhysicalDeviceProperties2(physDev, &deviceProps);
        if (!drmProps.hasRender && !drmProps.hasPrimary) {
            return "";
        }
        dev_t primaryDev = makedev(drmProps.primaryMajor, drmProps.primaryMinor);
        dev_t renderDev = makedev(drmProps.renderMajor, drmProps.renderMinor);
        for (int i = 128; i < 136; ++i) {
            auto path = "/dev/dri/renderD" + std::to_string(i);
            struct stat s = {};
            if (stat(path.c_str(), &s) != 0) {
                continue;
            }
            if (s.st_rdev == renderDev || s.st_rdev == primaryDev) {
                return path;
            }
        }
        return "";
    }

    static Vendor vendorOfId(u32 vendorId) {
        switch (vendorId) {
        case 0x10de:
            return Vendor::Nvidia;
        case 0x1002:
            return Vendor::Amd;
        default:
            return Vendor::Intel;
        }
    }

    // Kernel driver bound to a DRM render node's GPU ("nvidia", "nouveau",
    // "i915", "xe", "amdgpu", ...), from sysfs, or "" if unknown.
    static std::string kernelDriverOfNode(std::string const& path) {
        auto slash = path.rfind('/');
        std::string node = slash == std::string::npos ? path : path.substr(slash + 1);
        char target[PATH_MAX] = {};
        auto link = "/sys/class/drm/" + node + "/device/driver";
        ssize_t len = readlink(link.c_str(), target, sizeof(target) - 1);
        if (len <= 0) {
            return "";
        }
        std::string driver(target, len);
        auto base = driver.rfind('/');
        return base == std::string::npos ? driver : driver.substr(base + 1);
    }

    // PCI vendor of a DRM render node, from sysfs (0 if unknown).
    static u32 vendorIdOfNode(std::string const& path) {
        auto slash = path.rfind('/');
        std::string node = slash == std::string::npos ? path : path.substr(slash + 1);
        std::ifstream file("/sys/class/drm/" + node + "/device/vendor");
        u32 vendorId = 0;
        file >> std::hex >> vendorId;
        return vendorId;
    }

    // PCI vendor a GPU selector names ("Intel(R) Graphics", "NVIDIA GeForce
    // RTX 3060", "AMD Radeon ..."), or 0.
    static u32 vendorIdOfSelector(std::string selector) {
        std::transform(selector.begin(), selector.end(), selector.begin(), ::tolower);
        if (selector.find("intel") != std::string::npos) {
            return 0x8086;
        }
        if (selector.find("nvidia") != std::string::npos
            || selector.find("geforce") != std::string::npos) {
            return 0x10de;
        }
        if (selector.find("amd") != std::string::npos
            || selector.find("radeon") != std::string::npos) {
            return 0x1002;
        }
        return 0;
    }

    // First DRM render node whose PCI vendor is vendorId, preferring one
    // other than `avoid`, or "".
    static std::string renderNodeOfVendor(u32 vendorId, std::string const& avoid) {
        std::string fallback;
        for (int i = 128; i < 136; ++i) {
            auto path = "/dev/dri/renderD" + std::to_string(i);
            if (access(path.c_str(), F_OK) != 0 || vendorIdOfNode(path) != vendorId) {
                continue;
            }
            if (path != avoid) {
                return path;
            }
            fallback = path;
        }
        return fallback;
    }

    // Encoder GPU and technology from the "Encoder GPU" / "Encoder
    // technology" settings (video.encoder_config.linux_gpu). With nothing
    // set this is ALVR's default: the render GPU's node, NVENC on NVIDIA,
    // VAAPI otherwise.
    void resolveTarget() {
        auto const* settings = Settings_Instance();
        EncoderTarget resolved;

        if (!VkContext::renderGpuWarning.empty()) {
            Warn("%s", VkContext::renderGpuWarning.c_str());
        }

        std::string renderNode = renderNodeOf(vkCtx.physDev);
        resolved.devicePath = renderNode.empty() ? "/dev/dri/renderD128" : renderNode;
        resolved.vendor = vkCtx.meta.vendor;

        std::string selector = settings->m_encoderGpu;
        if (!selector.empty()) {
            if (selector.rfind("/dev/", 0) == 0) {
                resolved.devicePath = selector;
                u32 vendorId = vendorIdOfNode(selector);
                if (vendorId != 0) {
                    resolved.vendor = vendorOfId(vendorId);
                }
            } else {
                auto devs = vkCtx.instance.enumeratePhysicalDevices();
                auto index = VkContext::findPhysicalDevice(devs, selector);
                std::string node = index ? renderNodeOf(devs[*index]) : "";
                if (!node.empty()) {
                    resolved.devicePath = node;
                    resolved.vendor = vendorOfId(devs[*index].getProperties().vendorID);
                } else if (u32 vendorId = vendorIdOfSelector(selector); vendorId != 0
                           && !(node = renderNodeOfVendor(vendorId, renderNode)).empty()) {
                    // SteamVR's environment may hide that GPU from Vulkan
                    // (e.g. VK_DRIVER_FILES limited to NVIDIA's ICD). VAAPI only
                    // needs its DRM render node, found here by PCI vendor.
                    resolved.devicePath = node;
                    resolved.vendor = vendorOfId(vendorId);
                    std::string env = VkContext::gpuOverrideEnvironment();
                    Info(
                        "Encoder GPU \"%s\" is not visible to Vulkan here (GPU-restricting "
                        "environment: %s); using DRM node %s by vendor",
                        selector.c_str(),
                        env.empty() ? "none" : env.c_str(),
                        node.c_str()
                    );
                } else {
                    std::string env = VkContext::gpuOverrideEnvironment();
                    Warn(
                        "Encoder GPU \"%s\" matches no GPU with a DRM render node, so it cannot "
                        "be honoured; using the render GPU. GPU-restricting environment: %s",
                        selector.c_str(),
                        env.empty() ? "none" : env.c_str()
                    );
                }
            }
            resolved.crossDevice = resolved.devicePath != renderNode;
        }

        auto backend = static_cast<EncoderBackend>(settings->m_encoderBackend);
        if (settings->m_forceSwEncoding) {
            backend = EncoderBackend::Software;
        }
        // NVENC needs NVIDIA's proprietary kernel driver; an NVIDIA GPU on
        // nouveau (Mesa NVK) has no encoder ALVR can use.
        std::string const kernelDriver = kernelDriverOfNode(resolved.devicePath);
        bool const nouveau = resolved.vendor == Vendor::Nvidia && kernelDriver == "nouveau";
        if (backend == EncoderBackend::Automatic) {
            backend = resolved.vendor == Vendor::Nvidia && !nouveau ? EncoderBackend::Nvenc
                                                                    : EncoderBackend::Vaapi;
        }
        if (nouveau) {
            Warn(
                "Encoder GPU %s is an NVIDIA GPU on nouveau (Mesa NVK), which has no NVENC; "
                "set \"Encoder GPU\" to another GPU with VAAPI or use NVIDIA's driver",
                resolved.devicePath.c_str()
            );
        }
        resolved.backend = backend;

        Info(
            "Encoder: %s on %s (kernel driver %s)%s (render GPU node %s)",
            encoderBackendName(resolved.backend),
            resolved.devicePath.c_str(),
            kernelDriver.empty() ? "unknown" : kernelDriver.c_str(),
            resolved.crossDevice ? ", a different GPU (linear dma-buf hand-over)" : "",
            renderNode.empty() ? "unknown" : renderNode.c_str()
        );
        target = resolved;
    }

public:
    // TODO: How are we supposed to match the physical device with direct mode?
    Encoder()
        : vkCtx(std::vector<u8> {}) { }

    void createImages(render::RendererCreateInfo rendererCI) {
        if (renderer.hasValue()) {
            renderer.get().destroy(vkCtx);
            renderer.reset();
        }

        auto const& settings = Settings_Instance();

        vk::Extent2D inExtent {
            .width = rendererCI.inputEyeExtent.width * 2,
            .height = rendererCI.inputEyeExtent.height,
        };

        std::vector<render::PipelineCreateInfo> pipeCIs;

        if (settings->m_enableColorCorrection)
            pipeCIs.push_back(makeColorCorrection(*settings, inExtent));

        // NOTE: This needs to be last as it needs to render into the output image
        if (settings->m_enableFoveatedEncoding) {
            auto [info, newExtent] = makeFoveation(*settings);
            rendererCI.outputExtent = newExtent;

            pipeCIs.push_back(info);
        }

        if (pipeCIs.empty()) {
            pipeCIs.push_back({
                .shaderData = std::vector(
                    QUAD_SHADER_COMP_SPV_PTR, QUAD_SHADER_COMP_SPV_PTR + QUAD_SHADER_COMP_SPV_LEN
                ),
            });
            warpCapableChain = true;
        } else {
            warpCapableChain = settings->m_enableFoveatedEncoding;
        }

        resolveTarget();
        rendererCI.outputHandle = target.backend == EncoderBackend::Nvenc
            ? render::HandleType::OpaqueFd
            : render::HandleType::DmaBuf;
        rendererCI.linearOutput = target.crossDevice;
        rendererCI.hostOutputCopy = target.crossDevice;

        outExtent = rendererCI.outputExtent;
        renderer.emplace(vkCtx, rendererCI, pipeCIs);
        if (target.crossDevice) {
            auto host = renderer.get().hostOutput();
            target.hostPixels = host.pixels;
            target.hostRowPitch = host.rowPitch;
        }
    }

    void initEncoding() {
        // Kept from mesa-v2: the GPU selector already resolved the encoder's
        // render node (target.devicePath), which may belong to a different
        // GPU than the render device. Upstream's search for the render
        // device's own node (from #3398) stays replaced; Vixea's 3c468d97
        // only touched it trivially (unused variable, spacing), which caused
        // the merge conflict for v3.
        Info("Using device path %s", target.devicePath.c_str());
        InfoToDriver(
            "[gpu-diag] encoder node (GPU selector): %s, backend %d, %s",
            alvr::gpudiag::describeNode(target.devicePath).c_str(),
            (int)target.backend,
            target.crossDevice ? "on a DIFFERENT GPU than the render device (host-memory hand-over)"
                               : "on the render device"
        );

        // av_log_set_level(AV_LOG_DEBUG);

        auto out = renderer.get().getOutput();

        // TODO: Fix Nvidia

        frame = std::make_unique<VkFrame>(
            vkCtx, out.image.image, out.imageCI, out.size, out.image.memory, out.drm
        );

        encoder = EncodePipeline::Create(vkCtx, target, *frame, outExtent.width, outExtent.height);
        if (!encoder) {
            throw std::runtime_error("no usable encoder for the current settings");
        }
        encoderMissingLogged = false;

        idrScheduler.OnStreamStart();
    }

    // Tears down the encode pipeline so the next createImages + initEncoding
    // rebuilds from current settings. The pipeline references the frame, so
    // destroy it first. Only call this from the thread that runs present().
    void shutdown() {
        // Drain the queue before releasing the frame. Nothing on the VAAPI
        // path leaves Vulkan work in flight, but the Nvidia path signals the
        // frame's semaphore from a queue submission and will need this when
        // it comes back.
        vkCtx.dev.waitIdle();

        encoder.reset();
        frame.reset();
        if (renderer.hasValue()) {
            renderer.get().destroy(vkCtx);
            renderer.reset();
        }
        for (auto& [handle, sem] : timelines) {
            vkCtx.dev.destroy(sem);
        }
        timelines.clear();
        encoderMissingLogged = false;
        timelineImportFailureLogged = false;
    }

    // Waits until SteamVR's timeline semaphore for a texture reaches value,
    // the end of the write submitted into it before Present. The semaphore is
    // imported on first use from dupFd() (Vulkan takes the fd; a failed
    // import is retried on the next call). Says whether value was reached,
    // not reached within timeoutNs, or the semaphore could not be imported.
    enum class TimelineWait { Reached, TimedOut, NotImportable };

    TimelineWait
    waitTimeline(u64 handle, std::function<int()> const& dupFd, u64 value, u64 timeoutNs) {
        auto reportFailure = [&](char const* stage, char const* detail) {
            if (!timelineImportFailureLogged) {
                Warn(
                    "[gpu-diag] SteamVR timeline %llx failed at %s on ALVR render GPU %s: %s. "
                    "SteamVR and ALVR must use the same GPU and Vulkan driver; restart Steam "
                    "from the Dashboard to apply Render GPU.",
                    (unsigned long long)handle,
                    stage,
                    vkCtx.physDev.getProperties().deviceName.data(),
                    detail
                );
                timelineImportFailureLogged = true;
            }
        };
        auto it = timelines.find(handle);
        if (it == timelines.end()) {
            int fd = dupFd();
            if (fd < 0) {
                reportFailure("SteamVR IPC", "no semaphore file descriptor received");
                return TimelineWait::NotImportable;
            }
            vk::SemaphoreTypeCreateInfo typeInfo {
                .semaphoreType = vk::SemaphoreType::eTimeline,
                .initialValue = 0,
            };
            vk::SemaphoreCreateInfo semInfo {
                .pNext = &typeInfo,
            };
            vk::Semaphore sem;
            try {
                sem = vkCtx.dev.createSemaphore(semInfo);
            } catch (std::exception const& e) {
                reportFailure("vkCreateSemaphore", e.what());
                close(fd);
                return TimelineWait::NotImportable;
            }
            vk::ImportSemaphoreFdInfoKHR importInfo {
                .semaphore = sem,
                .handleType = vk::ExternalSemaphoreHandleTypeFlagBits::eOpaqueFd,
                .fd = fd,
            };
            auto const result = vkCtx.dev.importSemaphoreFdKHR(&importInfo, vkCtx.dispatch);
            if (result != vk::Result::eSuccess) {
                reportFailure("vkImportSemaphoreFdKHR", vk::to_string(result).c_str());
                close(fd);
                vkCtx.dev.destroy(sem);
                return TimelineWait::NotImportable;
            }
            it = timelines.emplace(handle, sem).first;
        }
        vk::SemaphoreWaitInfo waitInfo {
            .semaphoreCount = 1,
            .pSemaphores = &it->second,
            .pValues = &value,
        };
        return vkCtx.dev.waitSemaphores(waitInfo, timeoutNs) == vk::Result::eSuccess
            ? TimelineWait::Reached
            : TimelineWait::TimedOut;
    }

    // Frees what was imported for textures and semaphores SteamVR destroyed.
    // Call between frames, with the worker kept out (m_encMutex).
    void releaseTextures(std::vector<u64> const& textures, std::vector<u64> const& semaphores) {
        if (renderer.hasValue()) {
            renderer.get().releaseTextures(vkCtx, textures);
            renderer.get().releaseSemaphores(vkCtx, semaphores);
        }
        for (u64 handle : semaphores) {
            auto it = timelines.find(handle);
            if (it != timelines.end()) {
                vkCtx.dev.destroy(it->second);
                timelines.erase(it);
            }
        }
    }

    // layers[0] is the scene, later layers are blended over it.
    void present(
        u64 targetTimestampNs,
        render::WarpParams const& warp,
        std::vector<render::Layer> const& layers
    ) {
        if (!encoder) {
            // Say it once instead of at frame rate.
            if (!encoderMissingLogged) {
                Error("Encoder not initialized, skipping frames until it is rebuilt.\n");
                encoderMissingLogged = true;
            }
            return;
        }
        renderer.get().warpParams = warp;
        ReportPresent(targetTimestampNs, 0);
        updateFoveationCenters(targetTimestampNs);
        renderer.get().render(vkCtx, layers);
        ReportComposed(targetTimestampNs, 0);

        encoder->SetParams(GetDynamicEncoderParams());
        encoder->PushFrame(0, idrScheduler.CheckIDRInsertion());

        alvr::FramePacket framePacket;
        if (!encoder->GetEncoded(framePacket)) {
            assert(false);
        }

        ParseFrameNals(
            encoder->GetCodec(),
            framePacket.data,
            framePacket.size,
            targetTimestampNs,
            framePacket.isIDR
        );
    }

    void requestIdr() { idrScheduler.InsertIDR(); }

    // True when engaging the reprojection actually warps pixels with the
    // current pass chain. Also false while the encoder has not been built.
    // Non-const because Optional::hasValue() is not const-qualified.
    bool warpCapable() { return renderer.hasValue() && warpCapableChain; }

private:
    // Eye-tracked centers when the client supplies gaze, the negotiated static
    // ones otherwise. Looked up and reported under the timestamp the frame is
    // sent with, which after a warp is the newest pose's, so the client
    // decodes the frame with the same centers it was encoded with.
    void updateFoveationCenters(u64 targetTimestampNs) {
        auto const& settings = Settings_Instance();
        if (!settings->m_enableFoveatedEncoding) {
            return;
        }

        auto const gazeCenters = GetEyeTrackedFoveationCenters(targetTimestampNs);
        auto const& centers = gazeCenters.valid ? gazeCenters.centerShifts
                                                : settings->m_foveatedEncoding.centerShifts;

        auto& shifts = renderer.get().foveationCenterShifts;
        shifts.left[0] = centers[0][0];
        shifts.left[1] = centers[0][1];
        shifts.right[0] = centers[1][0];
        shifts.right[1] = centers[1][1];

        ReportEncoderFoveationCenters(
            targetTimestampNs, shifts.left[0], shifts.left[1], shifts.right[0], shifts.right[1]
        );
    }

public:
    ~Encoder() {
        // The device dies in this body, but members are destroyed after the
        // body runs. Release the pipeline and the frame first so ~VkFrame
        // does not destroy its semaphore on a dead device.
        encoder.reset();
        frame.reset();

        if (renderer.hasValue()) {
            renderer.get().destroy(vkCtx);
            renderer.reset();
        }

        for (auto& [handle, sem] : timelines) {
            vkCtx.dev.destroy(sem);
        }
        timelines.clear();

        vkCtx.destroy();
    }
};

}
