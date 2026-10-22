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
    // Finer estimator grid reduces measured real-frame edge and temporal error.
    static constexpr uint32_t DefaultLumaDimension = 768;
    NativeMotion(
        VkContext&,
        vk::Extent2D,
        vk::Format,
        uint32_t maxLumaDimension = DefaultLumaDimension,
        bool allowExactFlowExit = true,
        bool bidirectionalFlow = false
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
        std::function<void()> const& releaseInput = {},
        uint64_t clockNs = 0
    );

    // Retains an internally owned copy of a real frame on capture presentations.
    // Output's layout is restored; imported SteamVR images are never retained.
    // clockNs (optional, steady-clock nanoseconds) is when the picture was
    // presented. Gap filling times its extrapolation with these clocks rather
    // than with pose timestamps, whose association to a picture can jump by
    // a tracking sample; the pose timestamp stays the picture's stamp.
    void capture(vk::Image output, vk::ImageLayout, uint64_t timestamp, uint64_t clockNs = 0);

    // Reference mode for measurements: same real/predicted cadence, but each
    // "predicted" picture repeats the newest real picture (no flow, no warp).
    // It shows what the stream looks like at half the input rate without
    // synthesis, the baseline synthesis has to beat.
    void setRepeatReference(bool);

    // Waits for a capture still running on the GPU. Call it before anything
    // writes the picture that was captured (the next render).
    void waitIdle();

    // Runs the optical flow for the next prediction as soon as two real
    // pictures are retained. Call it after the real picture has been encoded
    // and sent: the flow then runs in the gap before the next presentation,
    // and predict() only has to warp. predict() still estimates if needed.
    void prepare();

    // Gap-filling mode: every real picture is captured, and predict() runs
    // only when the encoder asks for a picture between real ones (the
    // application missed a display slot). It then extrapolates from the two
    // newest real pictures to the given timestamp.
    void setGapFill(bool);
    bool gapFill() const;

    // Valid after a successful prediction. No queue/device operation.
    PredictionTiming lastPredictionTiming() const;
};

}
