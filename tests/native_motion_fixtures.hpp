#pragma once

// Independent analytic scenes: render a known time directly, without using
// the predictor's flow or warp equations as a reference.
#include <algorithm>
#include <array>
#include <cmath>
#include <cstdint>
#include <string>
#include <vector>

namespace fixture {
using Pixels = std::vector<uint8_t>;
struct Size {
    uint32_t eyeWidth = 512, height = 384;
    uint32_t width() const { return eyeWidth * 2; }
    size_t bytes() const { return size_t(width()) * height * 4; }
};
enum class Scene { Translation, Stationary, FineTexture, Rotation, Foreground, Jitter };
inline const char* name(Scene scene) {
    switch (scene) {
    case Scene::Translation:
        return "translation";
    case Scene::Stationary:
        return "stationary";
    case Scene::FineTexture:
        return "fine_texture";
    case Scene::Rotation:
        return "rotation";
    case Scene::Foreground:
        return "foreground_occlusion";
    case Scene::Jitter:
        return "timestamp_jitter";
    }
    return "unknown";
}
inline uint32_t hash(int x, int y, unsigned eye) {
    uint32_t n = uint32_t(x) * 374761393u + uint32_t(y) * 668265263u + eye * 2246822519u;
    n = (n ^ (n >> 13)) * 1274126177u;
    return n ^ (n >> 16);
}
inline float texture(float x, float y, unsigned seed, float cell) {
    x /= cell;
    y /= cell;
    int ix = int(std::floor(x)), iy = int(std::floor(y));
    float fx = x - ix, fy = y - iy;
    auto value = [&](int dx, int dy) { return float(32 + hash(ix + dx, iy + dy, seed) % 192); };
    return (value(0, 0) * (1 - fx) + value(1, 0) * fx) * (1 - fy)
        + (value(0, 1) * (1 - fx) + value(1, 1) * fx) * fy;
}
inline bool foreground(Size size, unsigned eye, float x, float y, double time) {
    float offset = float((eye ? -1 : 1) * time * 6 * size.eyeWidth / 512.0);
    return std::abs(x - (size.eyeWidth * .45f + offset)) < size.eyeWidth * .16f
        && std::abs(y - size.height * .5f) < size.height * .25f;
}
inline Pixels render(Size size, Scene scene, double time) {
    Pixels pixels(size.bytes());
    for (uint32_t y = 0; y < size.height; ++y)
        for (uint32_t x = 0; x < size.width(); ++x) {
            unsigned eye = x / size.eyeWidth;
            float sx = float(x % size.eyeWidth), sy = float(y);
            float direction = eye ? -1.f : 1.f;
            unsigned seed = eye;
            float cell = scene == Scene::FineTexture ? 3.f : 8.f;
            if (scene == Scene::Rotation) {
                float angle = float(time * .004) * direction;
                float dx = sx - size.eyeWidth * .5f, dy = sy - size.height * .5f;
                sx = std::cos(angle) * dx + std::sin(angle) * dy + size.eyeWidth * .5f;
                sy = -std::sin(angle) * dx + std::cos(angle) * dy + size.height * .5f;
            } else if (scene == Scene::Foreground) {
                if (foreground(size, eye, sx, sy, time)) {
                    sx -= float(direction * time * 6 * size.eyeWidth / 512.0);
                    seed += 71;
                    cell = 6.f;
                }
            } else if (scene != Scene::Stationary) {
                sx -= float(direction * time * 4 * size.eyeWidth / 512.0);
            }
            uint8_t value = uint8_t(std::clamp(texture(sx, sy, seed, cell), 0.f, 255.f));
            size_t at = (size_t(y) * size.width() + x) * 4;
            pixels[at] = eye ? value / 2 : value;
            pixels[at + 1] = value;
            pixels[at + 2] = eye ? value : value / 2;
            pixels[at + 3] = 255;
        }
    return pixels;
}
struct Error {
    double repeated = 0, predicted = 0;
    size_t pixels = 0;
    void add(Pixels const& baseline, Pixels const& got, Pixels const& expected, size_t at) {
        for (unsigned c = 0; c < 3; ++c) {
            repeated += std::abs(int(baseline[at + c]) - int(expected[at + c]));
            predicted += std::abs(int(got[at + c]) - int(expected[at + c]));
        }
        ++pixels;
    }
    double baselineMae() const { return pixels ? repeated / (3 * pixels) : 0; }
    double predictionMae() const { return pixels ? predicted / (3 * pixels) : 0; }
};
struct Score {
    Error all, changed, revealed;
    bool validPixels = true;
    bool improved() const {
        if (!validPixels || !all.pixels)
            return false;
        if (all.baselineMae() <= .25)
            return all.predictionMae() <= .25;
        // The changed region prevents static background from diluting the error.
        return changed.pixels && all.predictionMae() < .95 * all.baselineMae()
            && changed.predictionMae() < .95 * changed.baselineMae();
    }
};
inline Score score(
    Size size,
    Scene scene,
    Pixels const& latest,
    Pixels const& predicted,
    Pixels const& expected,
    unsigned eye,
    double latestTime,
    double targetTime
) {
    Score result;
    unsigned border = std::max(32u, size.eyeWidth / 16);
    for (unsigned y = 0; y < size.height; ++y)
        for (unsigned x = 0; x < size.eyeWidth; ++x) {
            size_t at = (size_t(y) * size.width() + eye * size.eyeWidth + x) * 4;
            // Include the seam and outer edges in the alpha/eye-isolation contract.
            result.validPixels &= predicted[at + 3] == 255;
            result.validPixels
                &= eye ? predicted[at + 2] >= predicted[at] : predicted[at] >= predicted[at + 2];
            if (x < border || y < border || x + border >= size.eyeWidth
                || y + border >= size.height)
                continue;
            result.all.add(latest, predicted, expected, at);
            bool changed = false;
            for (unsigned c = 0; c < 3; ++c)
                changed |= std::abs(int(latest[at + c]) - int(expected[at + c])) >= 3;
            if (changed)
                result.changed.add(latest, predicted, expected, at);
            if (scene == Scene::Foreground && foreground(size, eye, float(x), float(y), latestTime)
                && !foreground(size, eye, float(x), float(y), targetTime))
                result.revealed.add(latest, predicted, expected, at);
        }
    return result;
}
}
