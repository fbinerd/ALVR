#pragma once

#include <cstdint>
#include <optional>

namespace alvr::motion {
// Timestamps are pose associations in ALVR's monotonic time domain, not wall
// time. Predict at most half the observed real-frame interval. The small
// tolerance accommodates tracking sample jitter without accepting another tick.
inline std::optional<float> predictionTime(uint64_t previous, uint64_t latest, uint64_t target) {
    if (!previous || latest <= previous || target <= latest || latest - previous > 100'000'000)
        return std::nullopt;
    double ratio = double(target - latest) / double(latest - previous);
    if (ratio > 0.55)
        return std::nullopt;
    return float(1.0 + ratio);
}
}
