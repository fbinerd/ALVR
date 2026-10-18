#pragma once

#include "openvr_driver.h"

// The Linux compositor discovers plugin providers through DisplayRedirect
// devices whose TrackingSystemName matches the registered driver's name.
class VirtualDisplayRedirect : public vr::ITrackedDeviceServerDriver {
public:
    static constexpr const char* SERIAL = "ALVR-virtual-display-poc";

    vr::EVRInitError Activate(vr::TrackedDeviceIndex_t deviceIndex) override;
    void Deactivate() override {}
    void EnterStandby() override {}
    void* GetComponent(const char*) override { return nullptr; }
    void DebugRequest(const char*, char* response, uint32_t responseSize) override;
    vr::DriverPose_t GetPose() override;
};
