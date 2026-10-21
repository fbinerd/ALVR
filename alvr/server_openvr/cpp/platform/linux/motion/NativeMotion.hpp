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
    // Wall durations include completion waits. Device durations are -1 when
    // the relevant queue does not support timestamps or results are unavailable.
    struct PredictionTiming {
        double flowMs = 0, warpMs = 0;
        double opticalGpuMs = -1, warpGpuMs = -1;
    };
    // The cap applies only to estimator luma, not color/output. Zero retains
    // the original half-resolution estimator for controlled comparisons.
    // Disabling exact flow exit is a checker control; runtime uses the default.
    static constexpr uint32_t DefaultLumaDimension = 512;
    NativeMotion(
        VkContext&,
        vk::Extent2D,
        vk::Format,
        uint32_t maxLumaDimension = DefaultLumaDimension,
        bool allowExactFlowExit = true
    );
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
