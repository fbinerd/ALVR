#include "OvrVirtualDisplayComponent.h"

#include "alvr_server/Logger.h"
#include "alvr_server/Settings.h"
#include "alvr_server/bindings.h"

#include <cmath>

void OvrVirtualDisplayComponent::Present(
    const vr::PresentInfo_t* presentInfo,
    uint32_t presentInfoSize
) {
    const auto now = std::chrono::steady_clock::now();
    const double intervalMs = m_hasLastPresentTime
                                 ? std::chrono::duration<double, std::milli>(
                                       now - m_lastPresentTime
                                   )
                                       .count()
                                 : 0.0;
    m_lastPresentTime = now;
    m_hasLastPresentTime = true;

    if (!presentInfo) {
        Warn("VirtualDisplay Present received null info (size=%u)", presentInfoSize);
        return;
    }

    uint64_t ipcHandle = 0;
    bool resourceManagerAvailable = vr::VRIPCResourceManager() != nullptr;
    bool refSucceeded = false;
    if (resourceManagerAvailable) {
        refSucceeded = vr::VRIPCResourceManager()->RefResource(
            presentInfo->backbufferTextureHandle,
            &ipcHandle
        );
        if (refSucceeded) {
            vr::VRIPCResourceManager()->UnrefResource(presentInfo->backbufferTextureHandle);
        }
    }

    Info(
        "VirtualDisplay Present frame=%llu handle=0x%llx size=%ux%u interval=%.3fms "
        "infoSize=%u ref=%s ipc=0x%llx vsync=%d vsyncTime=%.6f",
        static_cast<unsigned long long>(presentInfo->nFrameId),
        static_cast<unsigned long long>(presentInfo->backbufferTextureHandle),
        Settings_Instance()->m_renderWidth,
        Settings_Instance()->m_renderHeight,
        intervalMs,
        presentInfoSize,
        refSucceeded ? "ok" : (resourceManagerAvailable ? "failed" : "unavailable"),
        static_cast<unsigned long long>(ipcHandle),
        static_cast<int>(presentInfo->vsync),
        presentInfo->flVSyncTimeInSeconds
    );
}

void OvrVirtualDisplayComponent::WaitForPresent() {
    WaitForVSync();
}

bool OvrVirtualDisplayComponent::GetTimeSinceLastVsync(
    float* secondsSinceLastVsync,
    uint64_t* frameCounter
) {
    if (!secondsSinceLastVsync || !frameCounter) {
        return false;
    }

    unsigned long long elapsedNs = 0;
    unsigned long long counter = 0;
    if (!TimeSinceLastVSync(&elapsedNs, &counter)) {
        return false;
    }

    *secondsSinceLastVsync = static_cast<float>(elapsedNs) / 1'000'000'000.0f;
    *frameCounter = counter;
    return std::isfinite(*secondsSinceLastVsync);
}
