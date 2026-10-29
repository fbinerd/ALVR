#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace alvr::motion {

// All arguments are tags in the client's tracking history, not runtime or
// steady clocks. Preserve the output tag; move only the geometric query by
// the elapsed output-slot duration from the actual submitted picture's tag.
inline std::optional<uint64_t> pictureTargetTime(
    uint64_t pictureSource, uint64_t outputSource, uint64_t outputTarget
) {
    if (!pictureSource || !outputSource || outputTarget < outputSource)
        return {};
    uint64_t horizon = outputTarget - outputSource;
    if (horizon > 100'000'000 || pictureSource > std::numeric_limits<uint64_t>::max() - horizon)
        return {};
    return pictureSource + horizon;
}

}
