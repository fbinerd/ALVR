#pragma once

#include "../VkContext.hpp"
#include <memory>

namespace alvr::motion {

// Experimental controlled half-input-rate synthesis. All calls and device queue
// submissions belong to the encoder worker. The output image remains owned by
// the renderer and is sampled/written only after its render submission completes.
class NativeMotion {
    struct State;
    std::unique_ptr<State> state;

public:
    // CPU wall time including GPU completion, not device timestamp queries.
    struct PredictionTiming {
        double flowMs = 0, warpMs = 0;
    };
    NativeMotion(VkContext&, vk::Extent2D, vk::Format);
    ~NativeMotion();
    NativeMotion(const NativeMotion&) = delete;
    NativeMotion& operator=(const NativeMotion&) = delete;

    // Writes a prediction into output only on alternating presentations, after
    // two retained real frames. Returns false without touching output when the
    // history/timestamp is unsuitable. A GPU failure throws; the caller must
    // stop encoding and drain the device before reusing or destroying images.
    // releaseInput is called before prediction starts, only when the supplied
    // SteamVR textures will not be read. It allows PostPresent to continue.
    bool predict(
        vk::Image output,
        vk::ImageView,
        vk::ImageLayout,
        uint64_t timestamp,
        std::function<void()> const& releaseInput = {}
    );

    // Retains an internally owned copy of a real frame on capture presentations.
    // Output's layout is restored; imported SteamVR images are never retained.
    void capture(vk::Image output, vk::ImageLayout, uint64_t timestamp);

    // Valid after a successful prediction. No queue/device operation.
    PredictionTiming lastPredictionTiming() const;
};

}
