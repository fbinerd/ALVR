#pragma once
// Diagnostic observations only. No clock is relabelled and no calibration is
// inferred from packet arrival. Value 2 selects measurement without delivery.
#include <array>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <iomanip>
#include <sstream>
#include "alvr_server/Logger.h"
#include "alvr_server/openvr_driver_wrap.h"

namespace alvr::depth {
inline void observeRuntimeClock(uint64_t tag, uint32_t owner,
    vr::IVRDriverDirectModeComponent::SubmitLayerPerEye_t const& eye,
    std::array<float,3> const& trackedPosition, uint64_t entryNs=0) {
    using Clock=std::chrono::steady_clock;
    static Clock::time_point checked{};
    static bool enabled=false;
    static uint64_t observations=0;
    auto now=Clock::now();
    if(now-checked>std::chrono::seconds(1)){
        checked=now;int value=0;
        if(auto home=std::getenv("HOME"))std::ifstream(std::string(home)+"/.config/alvr/diag_depth_receiver")>>value;
        enabled=value==2;
    }
    if(!enabled)return;
    ++observations;
    if(observations>1200&&observations%120)return;
    auto ns=std::chrono::duration_cast<std::chrono::nanoseconds>(now.time_since_epoch()).count();
    auto prediction=eye.flHmdPosePredictionTimeInSecondsFromNow;
    if(!std::isfinite(prediction))return;
    std::ostringstream s;s<<std::setprecision(12)<<"Game depth observation {\"mono_ns\":"<<ns
        <<",\"entry_ns\":"<<entryNs<<",\"uv\":["<<eye.bounds.uMin<<','<<eye.bounds.vMin<<','<<eye.bounds.uMax<<','<<eye.bounds.vMax<<"]"
        <<",\"tag_ns\":"<<tag<<",\"owner\":"<<owner<<",\"prediction_s\":"<<prediction
        <<",\"submitted\":[";
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++)s<<(i||j?",":"")<<eye.mHmdPose.m[i][j];
    s<<"],\"tracked_position\":["<<trackedPosition[0]<<','<<trackedPosition[1]<<','<<trackedPosition[2]<<"]}";
    Info("%s",s.str().c_str());
}
}
