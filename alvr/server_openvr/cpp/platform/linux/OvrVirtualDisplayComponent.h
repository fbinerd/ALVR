#pragma once

#include "openvr_driver.h"

#include <chrono>

class OvrVirtualDisplayComponent : public vr::IVRVirtualDisplay {
public:
    void Present(const vr::PresentInfo_t* presentInfo, uint32_t presentInfoSize) override;
    void WaitForPresent() override;
    bool GetTimeSinceLastVsync(float* secondsSinceLastVsync, uint64_t* frameCounter) override;

private:
    std::chrono::steady_clock::time_point m_lastPresentTime {};
    bool m_hasLastPresentTime = false;
};
