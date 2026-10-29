// Offline production history and duration mapping; no driver/GPU launch.
#include "alvr_server/PoseHistory.h"
#include "platform/linux/motion/PicturePoseClock.hpp"
#include <cassert>
#include <cmath>
#include <iostream>
#include <limits>

void Debug(const char*, ...) {}

int main() {
    using alvr::motion::pictureTargetTime;
    constexpr uint64_t start=1'000'000'000, tick=4'629'630;
    auto x=[](uint64_t ns) { return .25*std::sin(double(ns)*1e-9*2*std::acos(-1.)/3); };
    PoseHistory history;
    FfiDeviceMotion motion{}; motion.pose.orientation.w=1; motion.pose.position[1]=1.5;
    for (unsigned i=0;i<140;++i) {
        uint64_t t=start+i*tick;
        motion.pose.position[0]=x(t);
        history.OnPoseUpdated(t,motion);
    }
    unsigned cases=0; double worst=0;
    for (unsigned source=25;source<90;++source) for (unsigned delay=2;delay<=10;++delay) {
        uint64_t picture=start+source*tick, output=picture+delay*tick;
        // Deliberately nonintegral tracking horizons: a nearest sample can
        // introduce millimetres of parallax noise at this lateral speed.
        for (uint64_t horizon : {9'000'000ull,13'888'889ull,16'203'705ull,
                                27'777'778ull,37'000'000ull,41'666'667ull,46'000'000ull}) {
            uint64_t wireTarget=output+horizon;
            auto mapped=pictureTargetTime(picture,output,wireTarget);
            assert(mapped && *mapped==picture+horizon);
            auto p=history.GetPositionInterpolated(*mapped);
            assert(p);
            worst=std::max(worst,std::abs(double((*p)[0])-x(*mapped)));
            assert(std::abs((*p)[1]-1.5f)<1e-7 && (*p)[2]==0);
            assert(wireTarget==output+horizon); // Geometry never rewrites output.
            ++cases;
        }
    }
    assert(worst<3e-6);
    assert(!pictureTargetTime(0,start,start+tick));
    assert(!pictureTargetTime(start,0,start+tick));
    assert(!pictureTargetTime(start,start,start-1));
    assert(!pictureTargetTime(start,start,start+100'000'001));
    assert(!pictureTargetTime(std::numeric_limits<uint64_t>::max()-2,start,start+3));
    assert(!history.GetPositionInterpolated(start-1));
    assert(!history.GetPositionInterpolated(start+140*tick));
    auto exact=history.GetPositionInterpolated(start+50*tick);
    assert(exact && (*exact)[0]==float(x(start+50*tick)));
    PoseHistory broken;
    broken.OnPoseUpdated(start,motion); broken.OnPoseUpdated(start+30'000'000,motion);
    assert(!broken.GetPositionInterpolated(start+15'000'000));
    motion.pose.position[0]=std::numeric_limits<float>::quiet_NaN();
    broken.OnPoseUpdated(start+40'000'000,motion);
    assert(!broken.GetPositionInterpolated(start+35'000'000));
    // Fixed orientation: the wire matcher still chooses newest, while the
    // picture association identifies actual position and rejects still ties.
    vr::HmdMatrix34_t submitted{{{1,0,0,float(x(start+50*tick))},{0,1,0,1.5},{0,0,1,0}}};
    auto unique=history.GetUniqueDepthPoseMatch(submitted);
    // Use a recent source inside the matcher's deliberately narrow window.
    submitted.m[0][3]=x(start+110*tick);
    unique=history.GetUniqueDepthPoseMatch(submitted);
    assert(unique && unique->targetTimestampNs==start+110*tick);
    assert(history.GetBestPoseMatch(submitted)->targetTimestampNs==start+139*tick);
    motion.pose.position[0]=submitted.m[0][3];
    history.OnPoseUpdated(start+140*tick,motion);
    assert(!history.GetUniqueDepthPoseMatch(submitted));
    std::cout<<cases<<" variable-delay picture/slot cases, max interpolation error "
             <<worst*1e6<<" um; bounded/invalid/ambiguous negatives and unchanged wire matcher passed\n";
}
