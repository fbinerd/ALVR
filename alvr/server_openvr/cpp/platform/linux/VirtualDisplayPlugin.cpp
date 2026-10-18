#include "VirtualDisplayPoc.h"
#include "openvr_driver.h"

#include <chrono>
#include <cmath>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <thread>
#include <unistd.h>

namespace {
using Clock = std::chrono::steady_clock;
constexpr double REFRESH_RATE = 72.0;

// This module runs inside vrcompositor, where ALVR's Rust runtime and logger
// have not been initialized. Keep every callback independent of that runtime.
class PluginLog {
public:
    ~PluginLog() { Close(); }

    void Write(const char* format, ...) {
        std::lock_guard lock(m_mutex);
        if (!m_file) {
            const char* home = std::getenv("HOME");
            if (!home) {
                return;
            }
            auto directory = std::filesystem::path(home) / ".config/alvr";
            std::error_code error;
            std::filesystem::create_directories(directory, error);
            if (error) {
                return;
            }
            m_file = std::fopen((directory / "vd_plugin.log").c_str(), "ae");
            if (!m_file) {
                return;
            }
        }

        auto wallMs = std::chrono::duration_cast<std::chrono::milliseconds>(
                          std::chrono::system_clock::now().time_since_epoch()
        ).count();
        auto monotonicNs = std::chrono::duration_cast<std::chrono::nanoseconds>(
                               Clock::now().time_since_epoch()
        ).count();
        std::fprintf(m_file, "wall_ms=%lld mono_ns=%lld pid=%d ",
                     static_cast<long long>(wallMs), static_cast<long long>(monotonicNs),
                     static_cast<int>(getpid()));
        va_list args;
        va_start(args, format);
        std::vfprintf(m_file, format, args);
        va_end(args);
        std::fputc('\n', m_file);
        std::fflush(m_file);
    }

    void Close() {
        std::lock_guard lock(m_mutex);
        if (m_file) {
            std::fclose(m_file);
            m_file = nullptr;
        }
    }

private:
    std::mutex m_mutex;
    FILE* m_file = nullptr;
};

class PluginDisplay : public vr::IVRVirtualDisplay {
public:
    explicit PluginDisplay(PluginLog& log) : m_log(log) {}

    void Reset() {
        std::lock_guard lock(m_mutex);
        m_epoch = Clock::now();
        m_hasLastPresent = false;
    }

    void Present(const vr::PresentInfo_t* info, uint32_t infoSize) override {
        if (!info || infoSize < sizeof(*info)) {
            m_log.Write("Present invalid info=%p infoSize=%u expected=%zu",
                        static_cast<const void*>(info), infoSize, sizeof(vr::PresentInfo_t));
            return;
        }

        const auto now = Clock::now();
        double intervalMs;
        {
            std::lock_guard lock(m_mutex);
            intervalMs = m_hasLastPresent
                ? std::chrono::duration<double, std::milli>(now - m_lastPresent).count() : 0.0;
            m_lastPresent = now;
            m_hasLastPresent = true;
        }

        uint64_t ipcHandle = 0;
        auto* resources = vr::VRIPCResourceManager();
        const bool referenced = resources && resources->RefResource(
            info->backbufferTextureHandle, &ipcHandle
        );
        const bool released = referenced && resources->UnrefResource(info->backbufferTextureHandle);
        m_log.Write(
            "Present handle=0x%llx nFrameId=%llu vsync=%d flVSyncTimeInSeconds=%.9f "
            "interval_ms=%.6f infoSize=%u RefResource=%s ipc=0x%llx UnrefResource=%s",
            static_cast<unsigned long long>(info->backbufferTextureHandle),
            static_cast<unsigned long long>(info->nFrameId), static_cast<int>(info->vsync),
            info->flVSyncTimeInSeconds, intervalMs, infoSize,
            referenced ? "ok" : (resources ? "failed" : "unavailable"),
            static_cast<unsigned long long>(ipcHandle),
            referenced ? (released ? "ok" : "failed") : "not_called"
        );
    }

    void WaitForPresent() override {
        Clock::time_point epoch;
        {
            std::lock_guard lock(m_mutex);
            epoch = m_epoch;
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - epoch).count();
        const uint64_t nextFrame = static_cast<uint64_t>(std::floor(elapsed * REFRESH_RATE)) + 1;
        const auto target = epoch + std::chrono::duration_cast<Clock::duration>(
            std::chrono::duration<double>(static_cast<double>(nextFrame) / REFRESH_RATE)
        );
        m_log.Write("WaitForPresent begin target_frame=%llu refresh_hz=72",
                    static_cast<unsigned long long>(nextFrame));
        std::this_thread::sleep_until(target);
        m_log.Write("WaitForPresent end target_frame=%llu",
                    static_cast<unsigned long long>(nextFrame));
    }

    bool GetTimeSinceLastVsync(float* seconds, uint64_t* frameCounter) override {
        if (!seconds || !frameCounter) {
            m_log.Write("GetTimeSinceLastVsync invalid output pointers");
            return false;
        }
        Clock::time_point epoch;
        {
            std::lock_guard lock(m_mutex);
            epoch = m_epoch;
        }
        const double elapsed = std::chrono::duration<double>(Clock::now() - epoch).count();
        *frameCounter = static_cast<uint64_t>(std::floor(elapsed * REFRESH_RATE));
        *seconds = static_cast<float>(elapsed - static_cast<double>(*frameCounter) / REFRESH_RATE);
        m_log.Write("GetTimeSinceLastVsync seconds=%.9f frame_counter=%llu refresh_hz=72",
                    *seconds, static_cast<unsigned long long>(*frameCounter));
        return true;
    }

private:
    PluginLog& m_log;
    std::mutex m_mutex;
    Clock::time_point m_epoch = Clock::now();
    Clock::time_point m_lastPresent {};
    bool m_hasLastPresent = false;
};

class CompositorPluginProvider : public vr::IVRCompositorPluginProvider {
public:
    CompositorPluginProvider() : m_display(m_log) {}

    vr::EVRInitError Init(vr::IVRDriverContext* context) override {
        m_log.Write("Init begin context=%p", static_cast<void*>(context));
        if (!context) {
            return vr::VRInitError_Init_InvalidInterface;
        }
        const auto error = vr::InitCompositorDriverContext(context);
        m_log.Write("InitCompositorDriverContext result=%d", static_cast<int>(error));
        if (error != vr::VRInitError_None) {
            vr::CleanupDriverContext();
            return error;
        }
        m_display.Reset();
        m_initialized = true;
        m_log.Write("Init complete refresh_hz=72 runtime=local_proof_only");
        return vr::VRInitError_None;
    }

    void Cleanup() override {
        m_log.Write("Cleanup");
        m_initialized = false;
        vr::CleanupDriverContext();
        m_log.Close();
    }

    const char* const* GetInterfaceVersions() override {
        m_log.Write("GetInterfaceVersions");
        return vr::k_InterfaceVersions;
    }

    void* GetComponent(const char* name) override {
        const bool supported = m_initialized && name
            && std::strcmp(name, vr::IVRVirtualDisplay_Version) == 0;
        m_log.Write("GetComponent name=%s supported=%d", name ? name : "<null>", supported);
        return supported ? static_cast<vr::IVRVirtualDisplay*>(&m_display) : nullptr;
    }

    PluginLog m_log;

private:
    PluginDisplay m_display;
    bool m_initialized = false;
};
} // namespace

extern "C" void* CppLinuxCompositorPluginEntryPoint(int* returnCode) {
    if (!IsLinuxVirtualDisplayPocEnabled()) {
        if (returnCode) {
            *returnCode = vr::VRInitError_Init_InterfaceNotFound;
        }
        return nullptr;
    }

    static CompositorPluginProvider provider;
    provider.m_log.Write("Factory interface=%s", vr::IVRCompositorPluginProvider_Version);
    if (returnCode) {
        *returnCode = vr::VRInitError_None;
    }
    return static_cast<vr::IVRCompositorPluginProvider*>(&provider);
}
