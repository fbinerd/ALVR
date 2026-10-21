// CPU-only regression for pose-timestamp eligibility; no Vulkan or device use.
#include "platform/linux/motion/NativeMotionTiming.hpp"
#include <cassert>
#include <cmath>
#include <limits>

int main() {
    using alvr::motion::predictionTime;
    constexpr uint64_t a = 1'000'000'000, period = 27'777'778;
    auto half = predictionTime(a, a + period, a + period + period / 2);
    assert(half && std::abs(*half - 1.5f) < 1e-6f);
    assert(predictionTime(a, a + period, a + period + period * 54 / 100));
    assert(!predictionTime(a, a + period, a + 2 * period));
    assert(!predictionTime(0, a, a + 1));
    assert(!predictionTime(a, a, a + 1));
    assert(!predictionTime(a + 1, a, a + 2));
    assert(!predictionTime(a, a + period, a + period));
    assert(!predictionTime(a, a + period, a));
    assert(!predictionTime(a, a + 100'000'001, a + 100'000'002));
    constexpr uint64_t end = std::numeric_limits<uint64_t>::max();
    assert(predictionTime(end - 300, end - 100, end));
    assert(!predictionTime(a, a + period, end));
}
