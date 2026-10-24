#pragma once

#include <chrono>
#include <cstdint>

namespace alvr {

// Decides whether all textures of one frame are finished, i.e. whether
// SteamVR has signalled each texture's timeline semaphore to the frame's
// value. Returns true only if every texture is confirmed.
//
// - `waits` lists the frame's textures; an entry with semaphore == 0 has no
//   timeline and is skipped here (the caller handles it with implicit fences).
// - `wait(texture, timeoutNs)` imports the semaphore if needed and waits up to
//   timeoutNs; it returns false on timeout or when the import fails.
// - `budget` is one deadline for the whole frame, shared by all its eyes and
//   layers, not a separate budget per texture. A texture checked after the
//   deadline still gets a zero-time check, so an already-finished one passes.
// - The first texture that is not confirmed rejects the frame at once.
// - `now` returns the current time; tests pass a fake clock so deadlines can
//   be checked without sleeping.
template <typename Waits, typename Wait, typename Now>
bool waitForTextureTimelines(
    Waits const& waits, std::chrono::nanoseconds budget, Wait&& wait, Now&& now
) {
    auto const deadline = now() + budget;
    for (auto const& texture : waits) {
        if (texture.semaphore == 0) {
            continue;
        }
        auto const remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(
            deadline - now()
        ).count();
        if (!wait(texture, remaining > 0 ? static_cast<uint64_t>(remaining) : 0)) {
            return false;
        }
    }
    return true;
}

template <typename Waits, typename Wait>
bool waitForTextureTimelines(
    Waits const& waits, std::chrono::nanoseconds budget, Wait&& wait
) {
    return waitForTextureTimelines(
        waits, budget, wait, [] { return std::chrono::steady_clock::now(); }
    );
}

} // namespace alvr
