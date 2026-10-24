// Offline depth-aware gap-fill check. A textured near object (0.5 m) moves
// in front of a textured far background (3 m); the analytic scene supplies
// colour and canonical depth (R32F view distance in metres) for every frame.
// Each variant runs the production NativeMotion gap-fill path on the even
// (real) frames and extrapolates every odd frame; the result is compared
// with the analytic odd frame, with and without depth. No ALVR, SteamVR,
// headset or shared configuration is used. Requires NVIDIA optical flow.
#define main syntheticCheckerMain
#include "linux_native_motion_images.cpp"
#undef main

#include "native_motion_recording_metrics.hpp"
#include "platform/linux/motion/StereoDepth.hpp"
#include <map>

namespace depthfixture {
struct Scene {
    char const* name;
    float objectSpeed; // eye pixels per frame
    float backgroundSpeed;
    float yawSpeed = 0; // head yaw, radians per frame
    bool flatObject = false; // untextured object: colour cannot confirm motion
    bool sky = false; // top of the background has no depth (+0)
    float swaySpeed = 0; // sideways head motion, metres per frame (+x = right)
};
// Mock-like geometry: tangents +/-tan(1) for both eyes.
inline float tangent() { return std::tan(1.0f); }
// Maps a picture pixel seen with head yaw to the yaw-0 picture plane, in
// pixels, where the scene is defined. Rotation is about +y, the same
// convention as the warp's quaternions.
inline bool toWorld(Size size, float yaw, float x, float y, float& wx, float& wy) {
    if (yaw == 0) {
        wx = x;
        wy = y;
        return true;
    }
    float t = tangent();
    float rx = -t + (x + .5f) / size.eyeWidth * 2 * t, ry = t - (y + .5f) / size.height * 2 * t, rz = -1;
    float c = std::cos(yaw), sn = std::sin(yaw);
    float qx = rx * c + rz * sn, qz = -rx * sn + rz * c;
    if (qz >= -1e-5f)
        return false;
    float px = qx / -qz, py = ry / -qz;
    wx = (px + t) / (2 * t) * size.eyeWidth - .5f;
    wy = (t - py) / (2 * t) * size.height - .5f;
    return true;
}
inline alvr::motion::NativeMotion::Rotation yawRotation(double delta) {
    alvr::motion::NativeMotion::Rotation r;
    r.quaternion[1] = float(std::sin(delta / 2));
    r.quaternion[3] = float(std::cos(delta / 2));
    for (auto t : { r.leftTans, r.rightTans }) {
        t[0] = -tangent();
        t[1] = tangent();
        t[2] = tangent();
        t[3] = -tangent();
    }
    return r;
}
constexpr float NearMetres = 0.5f, FarMetres = 3.0f, Ipd = 0.063f;
// Stereo parallax of a point at a distance, in pixels: the left eye sees it
// half of this to the right, the right eye half to the left.
inline float disparity(Size size, float metres) {
    return Ipd * size.eyeWidth / (2 * std::tan(1.0f)) / metres;
}
// Image shift, in pixels, of content at a distance when the head has moved
// sideways by the scene's sway at that time.
inline float swayShift(Size size, float swaySpeed, double time, float metres) {
    return -float(time) * swaySpeed * size.eyeWidth / (2 * std::tan(1.0f)) / metres;
}
// Object rectangle centre at a time; the eyes see it with opposite parallax.
inline bool object(Size size, Scene const& scene, unsigned eye, float x, float y, double time) {
    if (!toWorld(size, float(time * scene.yawSpeed), x, y, x, y))
        return false;
    float centre = size.eyeWidth * .2f + float(time) * scene.objectSpeed
        + (eye ? -.5f : .5f) * disparity(size, NearMetres)
        + swayShift(size, scene.swaySpeed, time, NearMetres);
    return std::abs(x - centre) < size.eyeWidth * .12f && std::abs(y - size.height * .5f) < size.height * .22f;
}
struct Frame {
    Pixels colour, depth;
};
inline Frame render(Size size, Scene const& scene, double time) {
    Frame frame { Pixels(size.bytes()), Pixels(size.bytes()) };
    for (uint32_t y = 0; y < size.height; ++y)
        for (uint32_t x = 0; x < size.width(); ++x) {
            unsigned eye = x / size.eyeWidth;
            float px = float(x % size.eyeWidth), py = float(y), sx, sy;
            bool seen = toWorld(size, float(time * scene.yawSpeed), px, py, sx, sy);
            bool near = seen && object(size, scene, eye, px, py, time);
            float value, metres;
            size_t at = (size_t(y) * size.width() + x) * 4;
            if (near) {
                float local = sx - float(time) * scene.objectSpeed
                    - (eye ? -.5f : .5f) * disparity(size, NearMetres)
                    - swayShift(size, scene.swaySpeed, time, NearMetres);
                value = scene.flatObject ? 150.f : fixture::texture(local, sy, 91, 6.f);
                metres = NearMetres;
                // A warm object on a neutral background makes ghosts visible.
                frame.colour[at] = uint8_t(std::min(255.f, value * 1.1f + 20));
                frame.colour[at + 1] = uint8_t(value * .7f);
                frame.colour[at + 2] = uint8_t(value * .4f);
            } else {
                float local = sx - float(time) * scene.backgroundSpeed
                    - (eye ? -.5f : .5f) * disparity(size, FarMetres)
                    - swayShift(size, scene.swaySpeed, time, FarMetres);
                value = fixture::texture(local, sy, 7, 10.f);
                metres = scene.sky && sy < size.height * .3f ? 0.f : FarMetres;
                frame.colour[at] = frame.colour[at + 1] = frame.colour[at + 2] = uint8_t(value);
            }
            frame.colour[at + 3] = 255;
            std::memcpy(&frame.depth[at], &metres, 4);
        }
    return frame;
}
}

int main(int argc, char** argv) {
    try {
        if (argc < 2)
            throw std::runtime_error("Usage: depth_test OUTPUT [EYE_WIDTH HEIGHT]");
        std::filesystem::path output(argv[1]);
        if (std::filesystem::exists(output / "summary.csv"))
            throw std::runtime_error("Use a new output directory");
        std::filesystem::create_directories(output);
        Size size { 1024, 640 };
        // DEPTH_DIAGNOSTIC=1 paints the depth decision instead of colour.
        bool diagnostic = std::getenv("DEPTH_DIAGNOSTIC") != nullptr;
        // DEPTH_SOURCE=stereo estimates depth from the eyes instead of using
        // the analytic depth (the same depth the evaluation never sees).
        bool stereo = std::getenv("DEPTH_SOURCE") && std::string(std::getenv("DEPTH_SOURCE")) == "stereo";
        if (argc >= 4)
            size = { uint32_t(std::stoul(argv[2])), uint32_t(std::stoul(argv[3])) };
        RecordingContext ctx;
        if (!ctx.meta.opticalFlowEnabled || ctx.meta.vendor != alvr::Vendor::Nvidia)
            throw std::runtime_error("NVIDIA optical flow required");
        Images gpu(ctx, size);
        gpu.initialize();
        Images depthImage(ctx, size);
        depthImage.format = vk::Format::eR32Sfloat;
        depthImage.initialize();
        std::unique_ptr<alvr::motion::StereoDepth> stereoDepth;
        alvr::motion::StereoParams stereoParams;
        if (stereo) {
            stereoDepth = std::make_unique<alvr::motion::StereoDepth>(
                ctx, vk::Extent2D { size.width(), size.height }, vk::Format::eR8G8B8A8Unorm,
                std::getenv("STEREO_LUMA_MAX") ? uint32_t(std::stoul(std::getenv("STEREO_LUMA_MAX"))) : 640u);
            float t = std::tan(1.0f);
            stereoParams.leftTans = stereoParams.rightTans = { -t, t, t, -t };
            stereoParams.ipd = depthfixture::Ipd;
        }
        std::ofstream(output / "depth_source.txt") << (stereo ? "stereo" : "analytic") << '\n';

        std::vector<depthfixture::Scene> scenes {
            { "slow_object_static_background", 6, 0 },
            { "fast_object_static_background", 16, 0 },
            { "fast_object_panning_background", 16, -4 },
            { "object_with_background_same_direction", 12, 4 },
            { "flat_object_static_background", 12, 0, 0, true },
            { "object_with_head_yaw", 12, 0, .006f },
            { "object_against_head_yaw_with_sky", -10, 0, .006f, false, true },
            { "head_yaw_only_with_sky", 0, 0, .008f, false, true },
            // 2 mm per frame = 0.14 m/s sideways: near content moves 6x the far.
            { "head_sway_static_object", 0, 0, 0, false, false, .002f },
            { "head_sway_moving_object", 8, 0, 0, false, false, .002f },
        };
        struct Mode {
            char const* name;
            bool rotation, stable;
        };
        std::vector<Mode> modes { { "mode4", false, false }, { "mode5", true, false }, { "mode7", true, true } };
        constexpr unsigned Frames = 41;
        alvr::motion::NativeMotion::Rotation identity;
        for (auto t : { identity.leftTans, identity.rightTans }) {
            t[0] = -std::tan(1.0f);
            t[1] = std::tan(1.0f);
            t[2] = std::tan(1.0f);
            t[3] = -std::tan(1.0f);
        }
        std::ofstream csv(output / "metrics.csv"), summary(output / "summary.csv");
        csv << "scene,mode,depth,frame,region,pixels,repeat_mae,synth_mae\n";
        summary << "scene,mode,depth,region,targets,repeat_mae,synth_mae\n";
        unsigned border = 16;
        // DEPTH_SCENES=name,name runs only those scenes.
        std::string only = std::getenv("DEPTH_SCENES") ? std::getenv("DEPTH_SCENES") : "";
        for (auto const& scene : scenes) {
            if (!only.empty() && ("," + only + ",").find(std::string(",") + scene.name + ",") == std::string::npos)
                continue;
            std::vector<depthfixture::Frame> frames;
            for (unsigned f = 0; f < Frames; ++f)
                frames.push_back(depthfixture::render(size, scene, double(f)));
            for (auto const& mode : modes)
                for (bool useDepth : { false, true }) {
                    alvr::motion::NativeMotion motion(
                        ctx, { size.width(), size.height }, vk::Format::eR8G8B8A8Unorm
                    );
                    motion.setGapFill(true);
                    motion.setStableResidual(mode.stable);
                    if (diagnostic && useDepth)
                        motion.setDiagnosticFlags(2);
                    // These analytic scenes evaluate the occlusion handling.
                    motion.setDepthOcclusion(std::getenv("DEPTH_OCCLUSION_OFF") == nullptr);
                    // delta frames between source and destination pictures.
                    auto rotationFor = [&](double frames) {
                        static alvr::motion::NativeMotion::Rotation r;
                        r = depthfixture::yawRotation(scene.yawSpeed * frames);
                        r.translation[0] = float(scene.swaySpeed * frames);
                        return mode.rotation ? &r : nullptr;
                    };
                    std::map<std::string, std::array<double, 3>> totals; // pixels, repeat, synth
                    unsigned targets = 0;
                    bool saved = false;
                    for (unsigned f = 0; f < Frames; ++f) {
                        uint64_t stamp = First + f * Tick;
                        if (f % 2 == 0) {
                            motion.waitIdle();
                            gpu.transfer(&frames[f].colour);
                            if (stereo && useDepth) {
                                stereoDepth->estimate(gpu.image, gpu.view, gpu.layout, depthImage.image,
                                    depthImage.view, vk::ImageLayout::eGeneral, stereoParams);
                                depthImage.layout = vk::ImageLayout::eGeneral;
                                if (f == 20 && mode.stable) {
                                    // Estimated vs exact depth on one frame: validity and error by surface.
                                    auto estimated = depthImage.transfer(nullptr);
                                    double n[2] {}, valid[2] {};
                                    std::vector<double> err[2];
                                    for (size_t i = 0; i < estimated.size(); i += 4) {
                                        float e, x;
                                        std::memcpy(&e, &estimated[i], 4);
                                        std::memcpy(&x, &frames[f].depth[i], 4);
                                        if (x <= 0) continue;
                                        int k = x < 1 ? 0 : 1;
                                        n[k]++;
                                        if (e > 0) { valid[k]++; err[k].push_back(std::abs(e - x) / x); }
                                    }
                                    // Near/far confusion within 12 px of the object edge, by side.
                                    {
                                        long c[2][2][2] {}; // side(left/right of centre), truth near, estimate near
                                        for (unsigned y = 0; y < size.height; ++y)
                                            for (unsigned x = 0; x < size.width(); ++x) {
                                                unsigned eye = x / size.eyeWidth; float ex = float(x % size.eyeWidth);
                                                bool in = depthfixture::object(size, scene, eye, ex, float(y), double(f));
                                                bool edge = false;
                                                for (int dx = -12; dx <= 12 && !edge; dx += 3)
                                                    edge = depthfixture::object(size, scene, eye, ex + dx, float(y), double(f)) != in;
                                                if (!edge) continue;
                                                size_t i = (size_t(y) * size.width() + x) * 4;
                                                float e; std::memcpy(&e, &estimated[i], 4);
                                                bool en = e > 0 && e < 1.2f;
                                                bool leftSide = !depthfixture::object(size, scene, eye, ex - 12, float(y), double(f))
                                                    && !(in && depthfixture::object(size, scene, eye, ex - 12, float(y), double(f)));
                                                c[eye][in][en]++;
                                                (void)leftSide;
                                            }
                                        for (unsigned eye = 0; eye < 2; ++eye)
                                            std::cout << "  eye " << eye << " edge band: truth near->est near " << c[eye][1][1]
                                                      << " near->far " << c[eye][1][0] << " far->near " << c[eye][0][1]
                                                      << " far->far " << c[eye][0][0] << '\n';
                                    }
                                    auto median = [](std::vector<double> v) {
                                        if (v.empty()) return 0.0;
                                        std::nth_element(v.begin(), v.begin() + v.size() / 2, v.end());
                                        return v[v.size() / 2];
                                    };
                                    std::cout << scene.name << " stereo depth frame 20: near valid "
                                              << 100 * valid[0] / n[0] << "% median rel err " << 100 * median(err[0])
                                              << "%; far valid " << 100 * valid[1] / n[1] << "% median rel err "
                                              << 100 * median(err[1]) << "%\n";
                                    writePpm(output / (std::string(scene.name) + "_stereo_depth.ppm"), [&] {
                                        Pixels v(estimated.size());
                                        for (size_t i = 0; i < estimated.size(); i += 4) {
                                            float e; std::memcpy(&e, &estimated[i], 4);
                                            uint8_t g = e <= 0 ? 0 : uint8_t(std::clamp(255.f / (1.f + e), 1.f, 255.f));
                                            v[i] = e <= 0 ? 255 : g; v[i + 1] = g; v[i + 2] = g; v[i + 3] = 255;
                                        }
                                        return v;
                                    }(), size);
                                }
                            } else
                                depthImage.transfer(&frames[f].depth);
                            alvr::motion::NativeMotion::Depth depth {
                                depthImage.image, depthImage.layout, { size.width(), size.height }
                            };
                            motion.capture(gpu.image, gpu.layout, stamp, stamp, rotationFor(f ? 2.0 : 0.0),
                                useDepth ? &depth : nullptr);
                            motion.prepare();
                            continue;
                        }
                        if (f < 9) // gap filling waits for four real pictures
                            continue;
                        require(motion.depthActive() == useDepth, "Depth activity mismatch");
                        motion.waitIdle();
                        if (!motion.predict(gpu.image, gpu.view, gpu.layout, stamp, {}, stamp,
                                rotationFor(1.0)))
                            throw std::runtime_error("Gap-fill target rejected");
                        auto image = gpu.transfer(nullptr);
                        auto const& truth = frames[f].colour;
                        auto const& latest = frames[f - 1].colour;
                        std::map<std::string, std::array<double, 3>> local;
                        for (unsigned y = border; y + border < size.height; ++y)
                            for (unsigned x = 0; x < size.width(); ++x) {
                                unsigned eye = x / size.eyeWidth, ex = x % size.eyeWidth;
                                if (ex < border || ex + border >= size.eyeWidth)
                                    continue;
                                bool before = depthfixture::object(size, scene, eye, float(ex), float(y), f - 1.0);
                                bool after = depthfixture::object(size, scene, eye, float(ex), float(y), double(f));
                                bool band = false;
                                for (int dy = -24; dy <= 24 && !band; dy += 6)
                                    for (int dx = -24; dx <= 24 && !band; dx += 6) {
                                        float px = float(ex) + dx, py = float(y) + dy;
                                        band = depthfixture::object(size, scene, eye, px, py, f - 1.0) != before
                                            || depthfixture::object(size, scene, eye, px, py, double(f)) != after;
                                    }
                                size_t at = (size_t(y) * size.width() + x) * 4;
                                double repeat = 0, synth = 0;
                                // Object colour where the truth shows the grey background: the shadow.
                                bool ghost = !after && int(image[at]) - int(image[at + 2]) > 30;
                                for (unsigned c = 0; c < 3; ++c) {
                                    repeat += std::abs(int(latest[at + c]) - int(truth[at + c]));
                                    synth += std::abs(int(image[at + c]) - int(truth[at + c]));
                                }
                                auto add = [&](char const* region) {
                                    auto& r = local[region];
                                    r[0] += 1;
                                    r[1] += repeat / 3;
                                    r[2] += synth / 3;
                                };
                                add("all");
                                if (band)
                                    add("edge_band");
                                if (before && !after)
                                    add("uncovered");
                                if (!after && band) {
                                    auto& g = local["ghost_pct_background"];
                                    g[0] += 1;
                                    g[1] += (int(latest[at]) - int(latest[at + 2]) > 30) * 100.0;
                                    g[2] += ghost * 100.0;
                                }
                                if (!before && after)
                                    add("covered");
                                if (!band)
                                    add("away_from_edges");
                            }
                        for (auto const& [region, r] : local) {
                            csv << scene.name << ',' << mode.name << ',' << useDepth << ',' << f << ','
                                << region << ',' << r[0] << ',' << r[1] / r[0] << ',' << r[2] / r[0] << '\n';
                            auto& t = totals[region];
                            t[0] += 1;
                            t[1] += r[1] / r[0];
                            t[2] += r[2] / r[0];
                        }
                        if (!saved && f >= 21) {
                            auto folder = output / scene.name / (std::string(mode.name) + (useDepth ? "_depth" : "_nodepth"));
                            std::filesystem::create_directories(folder);
                            writePpm(folder / "synth.ppm", image, size);
                            writePpm(folder / "truth.ppm", truth, size);
                            writePpm(folder / "latest.ppm", latest, size);
                            Pixels diff = truth;
                            for (size_t i = 0; i < diff.size(); ++i)
                                diff[i] = i % 4 == 3 ? 255 : uint8_t(std::min(255, 4 * std::abs(int(image[i]) - int(truth[i]))));
                            writePpm(folder / "diff_x4.ppm", diff, size);
                            saved = true;
                        }
                        ++targets;
                    }
                    for (auto const& [region, t] : totals)
                        summary << scene.name << ',' << mode.name << ',' << useDepth << ',' << region << ','
                                << t[0] << ',' << t[1] / t[0] << ',' << t[2] / t[0] << '\n';
                    std::cout << scene.name << ' ' << mode.name << " depth=" << useDepth
                              << " targets=" << targets << " edge_band synth="
                              << totals["edge_band"][2] / totals["edge_band"][0]
                              << " uncovered synth=" << totals["uncovered"][2] / totals["uncovered"][0]
                              << " repeat=" << totals["edge_band"][1] / totals["edge_band"][0] << '\n';
                }
        }
        summary.flush();
        csv.flush();
    } catch (std::exception const& e) {
        std::cerr << e.what() << '\n';
        return 1;
    }
}
