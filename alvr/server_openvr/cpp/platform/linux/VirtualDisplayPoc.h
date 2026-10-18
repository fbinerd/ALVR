#pragma once

#include <cstdlib>
#include <fstream>
#include <string>

inline bool IsLinuxVirtualDisplayPocEnabled() {
    const char* enabledEnv = std::getenv("ALVR_LINUX_VIRTUAL_DISPLAY_POC");
    if (enabledEnv && std::string(enabledEnv) == "1") {
        return true;
    }

    const char* home = std::getenv("HOME");
    if (!home) {
        return false;
    }

    std::ifstream config(std::string(home) + "/.config/alvr/virtual_display_poc");
    std::string value;
    config >> value;
    return value == "1";
}
