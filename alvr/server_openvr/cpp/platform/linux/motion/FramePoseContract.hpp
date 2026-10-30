#pragma once

#include "alvr_server/bindings.h"
#include "openvr_driver.h"
#include <array>
#include <cmath>
#include <optional>

namespace alvr::motion {
// Packet timestamps are tracking sample IDs, not a substitute for geometry.
// Supply the world-from-head pose of the actual encoded pixels separately.
inline std::optional<FfiPose> encodedFramePose(
    vr::HmdQuaternion_t q, std::array<float, 3> const& position
) {
    double norm = q.w*q.w + q.x*q.x + q.y*q.y + q.z*q.z;
    if (!std::isfinite(norm) || std::abs(norm-1) > .01) return {};
    for (float p : position) if (!std::isfinite(p)) return {};
    double s = 1/std::sqrt(norm);
    return FfiPose{{float(q.x*s), float(q.y*s), float(q.z*s), float(q.w*s)},
        {position[0], position[1], position[2]}};
}

inline double orientationErrorDegrees(FfiQuat a, FfiQuat b) {
    double dot = double(a.x)*b.x + double(a.y)*b.y + double(a.z)*b.z + double(a.w)*b.w;
    double na = double(a.x)*a.x + double(a.y)*a.y + double(a.z)*a.z + double(a.w)*a.w;
    double nb = double(b.x)*b.x + double(b.y)*b.y + double(b.z)*b.z + double(b.w)*b.w;
    return 2*std::acos(std::fmin(1., std::abs(dot)/std::sqrt(na*nb)))*180/std::acos(-1.);
}
}
