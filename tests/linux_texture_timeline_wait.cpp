// Regression test for alvr::waitForTextureTimelines (TextureTimelineWait.h),
// the rule the Linux encode worker uses before reading SteamVR's textures.
// It runs on the CPU only: no Vulkan device, GPU or SteamVR is needed. The
// wait callback and the clock are fakes, so deadlines are checked without
// sleeping. The 28 ms budget is an example value; the streamer's real
// budget is TimelineWaitCapFrames display intervals.
//
// Build and run from the repository root (one command):
//   c++ -std=c++17 -Wall -Wextra -Werror tests/linux_texture_timeline_wait.cpp -o /tmp/alvr-texture-wait-test && /tmp/alvr-texture-wait-test
#include "../alvr/server_openvr/cpp/platform/linux/TextureTimelineWait.h"

#include <cassert>
#include <iostream>
#include <vector>

using namespace std::chrono;
struct Texture { uint64_t semaphore; uint64_t value; };

int main() {
    auto const budget = nanoseconds(28000000);
    std::vector<Texture> textures { { 0, 0 }, { 1, 7 }, { 2, 9 }, { 3, 11 } };
    steady_clock::time_point time {};
    auto now = [&] { return time; };
    std::vector<uint64_t> caps;

    // Ready frames proceed immediately; the absent timeline is never imported.
    bool ok = alvr::waitForTextureTimelines(textures, budget,
        [&](auto const& texture, uint64_t cap) {
            assert(texture.semaphore != 0);
            assert(texture.value == texture.semaphore * 2 + 5);
            caps.push_back(cap);
            return true;
        }, now);
    assert(ok && caps.size() == 3);
    for (auto cap : caps) { assert(cap == uint64_t(budget.count())); }

    // Multiple eyes/layers share the budget, including a zero-time readiness
    // check at expiry. No extra frame intervals are granted to later layers.
    caps.clear();
    ok = alvr::waitForTextureTimelines(textures, budget,
        [&](auto const&, uint64_t cap) {
            caps.push_back(cap);
            time += nanoseconds(20000000);
            return true;
        }, now);
    assert(ok && (caps == std::vector<uint64_t> { 28000000, 8000000, 0 }));

    // Timeout/import failure both reject the job immediately. Repeated failed
    // jobs never turn into a read-anyway fallback; a later good job recovers.
    for (int job = 0; job < 100; ++job) {
        int calls = 0;
        ok = alvr::waitForTextureTimelines(textures, budget,
            [&](auto const&, uint64_t) { ++calls; return false; }, now);
        assert(!ok && calls == 1);
    }
    int calls = 0;
    ok = alvr::waitForTextureTimelines(textures, budget,
        [&](auto const&, uint64_t) { ++calls; return calls != 2; }, now);
    assert(!ok && calls == 2);
    ok = alvr::waitForTextureTimelines(textures, budget,
        [](auto const&, uint64_t) { return true; }, now);
    assert(ok);

    // No timeline: leave the implicit-fence fallback to its existing caller.
    calls = 0;
    ok = alvr::waitForTextureTimelines(std::vector<Texture> { { 0, 0 } }, budget,
        [&](auto const&, uint64_t) { ++calls; return false; }, now);
    assert(ok && calls == 0);

    // A zero budget cannot cause an unbounded wait or authorize a failed read.
    ok = alvr::waitForTextureTimelines(textures, nanoseconds(0),
        [](auto const&, uint64_t cap) { assert(cap == 0); return false; }, now);
    assert(!ok);
    std::cout << "Texture timeline policy: ready, shared deadline, exhaustion, "
                 "failure, repeated failure, recovery and fallback checks passed\n";
}
