#pragma once

#include "../VkContext.hpp"
#include "NativeOpticalFlow.hpp"
#include <array>
#include <memory>
#include <string>

namespace alvr::motion {

struct StereoEyeTangents {
    float left = 0.0f;
    float right = 0.0f;
    float up = 0.0f;
    float down = 0.0f;
};

struct StereoParams {
    StereoEyeTangents leftTans;
    StereoEyeTangents rightTans;
    float ipd = 0.063f; // Eye separation in metres (default 63 mm)
    float maxVerticalError = 2.0f; // Max vertical flow discrepancy in pixels before rejection
    float maxConsistencyError = 2.0f; // Max left-right consistency discrepancy in pixels before rejection
    uint32_t fillRadius = 4; // Search radius for hole filling from farther neighbour
    uint32_t fillMinNeighbors = 2; // Minimum valid neighbours required to fill a hole
};

struct StereoDepthTiming {
    double totalWallMs = 0.0;
    double lumaWallMs = 0.0;
    double flowWallMs = 0.0;
    double depthWallMs = 0.0;
    double fillWallMs = 0.0;
    double opticalGpuMs = -1.0;
    double depthGpuMs = -1.0;
};

class StereoDepth {
    struct State;
    std::unique_ptr<State> state;

public:
    static constexpr uint32_t DefaultLumaDimension = 0; // 0 = full resolution

    StereoDepth(
        VkContext& ctx,
        vk::Extent2D fullExtent, // Side-by-side dimensions (width = 2 * eyeWidth, height)
        vk::Format colorFormat = vk::Format::eR8G8B8A8Unorm,
        uint32_t maxLumaDimension = DefaultLumaDimension
    );
    ~StereoDepth();
    StereoDepth(const StereoDepth&) = delete;
    StereoDepth& operator=(const StereoDepth&) = delete;

    // Estimates canonical depth into outputDepth image.
    // Canonical depth format:
    // - R32F view distance in metres along -Z of each eye
    // - +0 = invalid or sky
    // - both eyes side by side, like the colour picture
    bool estimate(
        vk::Image inputColor,
        vk::ImageView inputColorView,
        vk::ImageLayout inputColorLayout,
        vk::Image outputDepth,
        vk::ImageView outputDepthView,
        vk::ImageLayout outputDepthLayout,
        const StereoParams& params,
        StereoDepthTiming* timing = nullptr
    );

    vk::Extent2D fullExtent() const;
    vk::Extent2D eyeExtent() const;
    vk::Extent2D lumaExtent() const;

    vk::Image internalDepthImage() const;
    vk::ImageView internalDepthView() const;
};

} // namespace alvr::motion
