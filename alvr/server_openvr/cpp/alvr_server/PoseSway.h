#pragma once
// Diagnostic head motion for headset tests (off unless the file exists).
//
// ~/.config/alvr/diag_pose_sway holds "<mode> <amplitude> <period seconds>",
// mode one of: lateral (metres, along the head's horizontal right), vertical
// (metres), yaw (degrees, about world up), pitch (degrees, about the head's
// right). It is re-read four times a second; removing the file stops the
// motion. The amplitude fades in and out over a second so no change jumps.
//
// The sway is added to the pose SteamVR renders with and to the pose history
// the driver matches frames against, but not to the client's own pose
// history: the headset reprojects each frame from the real pose it recorded
// for that frame's timestamp, so the user sees the scene move while keeping
// the head still. This lets frame synthesis be judged one motion at a time.

#include "bindings.h"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdlib>
#include <fstream>
#include <mutex>
#include <string>

namespace alvr {

class PoseSway {
public:
    void apply(FfiDeviceMotion& motion) {
        std::lock_guard<std::mutex> lock(mutex);
        auto const now = std::chrono::steady_clock::now();
        if (now - lastRead > std::chrono::milliseconds(250)) {
            lastRead = now;
            readConfig();
        }
        double const t = std::chrono::duration<double>(now - start).count();
        double const dt = lastApply.time_since_epoch().count() ? std::chrono::duration<double>(now - lastApply).count() : 0.;
        lastApply = now;
        double const target = enabled ? 1. : 0.;
        envelope += std::clamp(target - envelope, -dt, dt);
        if (envelope <= 0.)
            return;
        double const wave = envelope * amplitude * std::sin(2. * M_PI * t / period);
        auto& q = motion.pose.orientation;
        // Head right vector (x axis of the rotation), projected horizontally.
        double rx = 1 - 2 * (q.y * q.y + q.z * q.z);
        double rz = 2 * (q.x * q.z - q.w * q.y);
        double const norm = std::sqrt(rx * rx + rz * rz);
        if (norm > 1e-6) {
            rx /= norm;
            rz /= norm;
        } else {
            rx = 1;
            rz = 0;
        }
        if (mode == "lateral") {
            motion.pose.position[0] += float(rx * wave);
            motion.pose.position[2] += float(rz * wave);
        } else if (mode == "vertical") {
            motion.pose.position[1] += float(wave);
        } else if (mode == "yaw") {
            double const half = wave * M_PI / 360.;
            q = multiply({ 0, float(std::sin(half)), 0, float(std::cos(half)) }, q);
        } else if (mode == "pitch") {
            double const half = wave * M_PI / 360.;
            q = multiply(q, { float(std::sin(half)), 0, 0, float(std::cos(half)) });
        }
    }

private:
    static FfiQuat multiply(FfiQuat a, FfiQuat b) {
        return {
            a.w * b.x + a.x * b.w + a.y * b.z - a.z * b.y,
            a.w * b.y - a.x * b.z + a.y * b.w + a.z * b.x,
            a.w * b.z + a.x * b.y - a.y * b.x + a.z * b.w,
            a.w * b.w - a.x * b.x - a.y * b.y - a.z * b.z,
        };
    }

    void readConfig() {
        char const* home = std::getenv("HOME");
        std::ifstream file(std::string(home ? home : "") + "/.config/alvr/diag_pose_sway");
        std::string newMode;
        double newAmplitude = 0, newPeriod = 0;
        if (file >> newMode >> newAmplitude >> newPeriod && newPeriod > .5
            && (newMode == "lateral" || newMode == "vertical" || newMode == "yaw" || newMode == "pitch")) {
            // A new mode starts only after the previous one has faded out.
            if (newMode != mode && envelope > 0.) {
                enabled = false;
                return;
            }
            mode = newMode;
            amplitude = std::min(std::abs(newAmplitude), mode == "yaw" || mode == "pitch" ? 20. : .3);
            period = newPeriod;
            enabled = true;
        } else {
            enabled = false;
        }
    }

    std::mutex mutex;
    std::chrono::steady_clock::time_point const start = std::chrono::steady_clock::now();
    std::chrono::steady_clock::time_point lastRead {};
    std::chrono::steady_clock::time_point lastApply {};
    std::string mode;
    double amplitude = 0, period = 4, envelope = 0;
    bool enabled = false;
};

}
