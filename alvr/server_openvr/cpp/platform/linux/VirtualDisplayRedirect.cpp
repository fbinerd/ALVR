#include "VirtualDisplayRedirect.h"

#include "alvr_server/Logger.h"

vr::EVRInitError VirtualDisplayRedirect::Activate(vr::TrackedDeviceIndex_t deviceIndex) {
    auto* properties = vr::VRProperties();
    const auto container = properties->TrackedDeviceToPropertyContainer(deviceIndex);
    // This must match driver.vrdrivermanifest, not the emulated headset brand.
    const auto nameError = properties->SetStringProperty(
        container, vr::Prop_TrackingSystemName_String, "alvr_server"
    );
    properties->SetStringProperty(container, vr::Prop_ModelNumber_String, "ALVR Virtual Display POC");
    properties->SetBoolProperty(container, vr::Prop_HasVirtualDisplayComponent_Bool, true);
    properties->SetBoolProperty(container, vr::Prop_HasDriverDirectModeComponent_Bool, false);
    properties->SetFloatProperty(container, vr::Prop_DisplayFrequency_Float, 72.0f);
    Info("Linux virtual display redirect activated index=%u trackingSystemNameResult=%d",
         deviceIndex, static_cast<int>(nameError));
    return nameError == vr::TrackedProp_Success
        ? vr::VRInitError_None : vr::VRInitError_Driver_Failed;
}

void VirtualDisplayRedirect::DebugRequest(const char*, char* response, uint32_t responseSize) {
    if (response && responseSize) {
        response[0] = '\0';
    }
}

vr::DriverPose_t VirtualDisplayRedirect::GetPose() {
    vr::DriverPose_t pose {};
    pose.qRotation.w = 1.0;
    pose.qWorldFromDriverRotation.w = 1.0;
    pose.qDriverFromHeadRotation.w = 1.0;
    pose.deviceIsConnected = true;
    pose.poseIsValid = false;
    pose.result = vr::TrackingResult_Uninitialized;
    return pose;
}
