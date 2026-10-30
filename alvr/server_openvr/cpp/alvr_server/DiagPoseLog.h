#pragma once
// Diagnostic CSV of head poses and sent frames for headset measurements.
//
// Off unless ~/.config/alvr/diag_pose_log exists; its first line is the CSV
// path to append to (re-checked once a second, so it can be switched live).
// Lines, with the wall clock (CLOCK_REALTIME) in ns first:
//   P,<wall>,<target ns>,<x>,<y>,<z>,<qx>,<qy>,<qz>,<qw>,<sx>,<sy>,<sz>,<sqx>,<sqy>,<sqz>,<sqw>
//     a head pose from the client (x..qw) and the pose SteamVR got after the
//     diagnostic sway (sx..sqw; equal without diag_pose_sway);
//   F,<wall>,<target ns>,<kind>,<bytes>,<idr>
//     an encoded frame sent to the client, kind R = real or held picture,
//     S = synthesized fill; bytes match the packet in a server recording.
// The client reprojects each frame from its own pose for <target ns>, so the
// P lines give the pose every F line is displayed against.

#include "bindings.h"

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <ctime>
#include <fstream>
#include <mutex>
#include <string>

namespace alvr {

class DiagPoseLog {
public:
    static DiagPoseLog& get() {
        static DiagPoseLog instance;
        return instance;
    }

    void pose(uint64_t targetNs, FfiDeviceMotion const& real, FfiDeviceMotion const& sent) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!open())
            return;
        auto const& a = real.pose;
        auto const& b = sent.pose;
        std::fprintf(
            file, "P,%llu,%llu,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f,%.6f\n",
            wall(), (unsigned long long)targetNs, a.position[0], a.position[1], a.position[2],
            a.orientation.x, a.orientation.y, a.orientation.z, a.orientation.w, b.position[0],
            b.position[1], b.position[2], b.orientation.x, b.orientation.y, b.orientation.z,
            b.orientation.w
        );
    }

    void frame(uint64_t targetNs, char kind, int bytes, bool idr) {
        std::lock_guard<std::mutex> lock(mutex);
        if (!open())
            return;
        std::fprintf(
            file, "F,%llu,%llu,%c,%d,%d\n", wall(), (unsigned long long)targetNs, kind, bytes, idr ? 1 : 0
        );
    }

private:
    static unsigned long long wall() {
        timespec ts {};
        clock_gettime(CLOCK_REALTIME, &ts);
        return (unsigned long long)ts.tv_sec * 1000000000ull + ts.tv_nsec;
    }

    bool open() {
        auto const now = std::chrono::steady_clock::now();
        if (now - lastCheck > std::chrono::seconds(1)) {
            lastCheck = now;
            char const* home = std::getenv("HOME");
            std::ifstream config(std::string(home ? home : "") + "/.config/alvr/diag_pose_log");
            std::string target;
            if (!std::getline(config, target))
                target.clear();
            if (target != path) {
                if (file)
                    std::fclose(file);
                file = target.empty() ? nullptr : std::fopen(target.c_str(), "a");
                path = file ? target : std::string();
            } else if (file) {
                std::fflush(file);
            }
        }
        return file != nullptr;
    }

    std::mutex mutex;
    std::chrono::steady_clock::time_point lastCheck {};
    std::string path;
    std::FILE* file = nullptr;
};

}
