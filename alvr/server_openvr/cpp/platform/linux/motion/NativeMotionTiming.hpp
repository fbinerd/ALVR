#pragma once

#include <cstdint>
#include <optional>

namespace alvr::motion {
// Timestamps are pose associations in ALVR's monotonic time domain, not wall
// time. Predict at most half the observed real-frame interval. The small
// tolerance accommodates tracking sample jitter without accepting another tick.
// Gap filling predicts across a missed display slot of a slower application,
// so it passes a larger limit (at most one real-frame interval).
inline std::optional<float> predictionTime(
    uint64_t previous, uint64_t latest, uint64_t target, double maxRatio = 0.55
) {
    if (!previous || latest <= previous || target <= latest || latest - previous > 100'000'000)
        return std::nullopt;
    double ratio = double(target - latest) / double(latest - previous);
    if (ratio > maxRatio)
        return std::nullopt;
    return float(1.0 + ratio);
}
}
