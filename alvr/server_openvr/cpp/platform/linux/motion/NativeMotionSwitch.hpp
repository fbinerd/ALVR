#pragma once

#include <cstdlib>
#include <fstream>
#include <string>
#include <string_view>

namespace alvr::motion {

// Opt-in for native motion synthesis: 0 off, 1 on, 2 on with bidirectional
// optical flow. ALVR_NATIVE_MOTION=1 (plus ALVR_NATIVE_MOTION_BIDIRECTIONAL=1)
// still works when vrserver inherits it, but when the dashboard starts
// SteamVR through a running Steam client the variables do not reach vrserver.
// So the file ~/.config/alvr/diag_native_motion (containing 1 or 2) is read
// too. It is read when the Vulkan context and the encoder are created, so a
// change applies after a SteamVR restart. Absent file and variables: off.
inline int nativeMotionMode() {
    auto equals = [](char const* name, std::string_view value) {
        char const* v = std::getenv(name);
        return v && std::string_view(v) == value;
    };
    if (equals("ALVR_NATIVE_MOTION", "1")) {
        return equals("ALVR_NATIVE_MOTION_BIDIRECTIONAL", "1") ? 2 : 1;
    }
    char const* home = std::getenv("HOME");
    if (!home) {
        return 0;
    }
    std::ifstream file(std::string(home) + "/.config/alvr/diag_native_motion");
    int mode = 0;
    if (!(file >> mode) || mode < 0 || mode > 2) {
        return 0;
    }
    return mode;
}

}
