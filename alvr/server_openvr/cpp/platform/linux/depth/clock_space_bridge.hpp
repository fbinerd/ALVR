#pragma once
#include "gpu_transport.hpp"
#include <cmath>
#include <algorithm>
#include <cstring>
#include <fcntl.h>
#include <sys/stat.h>
#include <unistd.h>

namespace mhl::depth {
// Separate calibration ABI, not a change to packet v1. Written atomically by
// a trusted calibrator after paired clock and tracking-origin measurements.
struct Calibration {
    uint32_t magic=0x4344484d,version=1,size=sizeof(Calibration),reserved=0;
    uint64_t producerId{},referenceSpace{},monotonicAnchor{},targetAnchor{};
    uint64_t validFrom{},validUntil{},uncertaintyNs{};
    MhlDepthPose trackingFromReference{{0,0,0},{0,0,0,1}};
};
inline bool valid(Calibration const& c,uint64_t now) {
    float norm=0;
    for(float x:c.trackingFromReference.position)if(!std::isfinite(x))return false;
    for(float x:c.trackingFromReference.orientation){if(!std::isfinite(x))return false;norm+=x*x;}
    return c.magic==0x4344484d && (c.version==1||c.version==2||c.version==3) && c.size==sizeof(c) && !c.reserved &&
        c.producerId && c.referenceSpace && c.monotonicAnchor && c.targetAnchor &&
        c.validFrom<=c.monotonicAnchor && c.monotonicAnchor<=c.validUntil &&
        c.validFrom<=now && now<=c.validUntil && c.validUntil-c.validFrom<=60000000000ull &&
        c.uncertaintyNs<=1000000 && std::abs(norm-1.f)<.001f;
}
// Samples must be genuinely paired observations of the SAME intended display
// instant, not packet arrival times. Both clocks are nanoseconds at unit rate.
struct ClockPair { uint64_t target{},monotonic{},uncertainty{}; };
inline std::optional<Calibration> calibrate(uint64_t producer,uint64_t reference,
    ClockPair a,ClockPair b,MhlDepthPose transform,uint64_t validUntil) {
    if(!a.target||!a.monotonic||b.target<=a.target||b.monotonic<=a.monotonic||
       b.monotonic-a.monotonic<10000000||validUntil<b.monotonic||
       validUntil-a.monotonic>60000000000ull||a.uncertainty>1000000||b.uncertainty>1000000)return {};
    __int128 drift=(__int128(b.monotonic)-a.monotonic)-(__int128(b.target)-a.target);
    uint64_t residual=uint64_t(drift<0?-drift:drift);
    if(residual>1000000)return {};
    // Conservative 100 ppm extrapolation allowance, capped by the 1 ms match
    // contract. Recalibrate frequently; never extend an expired fit by arrival.
    uint64_t uncertainty=std::max(a.uncertainty,b.uncertainty)+residual+(validUntil-a.monotonic)/10000;
    if(uncertainty>1000000)return {};
    Calibration c;c.producerId=producer;c.referenceSpace=reference;
    c.monotonicAnchor=b.monotonic;c.targetAnchor=b.target;c.validFrom=a.monotonic;
    c.validUntil=validUntil;c.uncertaintyNs=uncertainty;c.trackingFromReference=transform;
    if(!valid(c,b.monotonic))return {};return c;
}
inline std::optional<Calibration> loadCalibration(std::string const& path,uint64_t now) {
    int fd=open(path.c_str(),O_RDONLY|O_CLOEXEC|O_NOFOLLOW|O_NONBLOCK);
    if(fd<0)return {};
    struct stat s{};Calibration c{};
    bool ok=fstat(fd,&s)==0 && S_ISREG(s.st_mode) && s.st_uid==getuid() &&
        !(s.st_mode&0077) && s.st_size==sizeof(c) && read(fd,&c,sizeof(c))==sizeof(c);
    close(fd);if(!ok||!valid(c,now))return {};return c;
}
inline std::optional<uint64_t> mapCalibrationClock(Calibration const& c,uint64_t target) {
    if(target>=c.targetAnchor){auto d=target-c.targetAnchor;if(d>UINT64_MAX-c.monotonicAnchor)return {};return c.monotonicAnchor+d;}
    auto d=c.targetAnchor-target;if(d>=c.monotonicAnchor)return {};return c.monotonicAnchor-d;
}
// Version 1 maps a genuinely paired client tag. Version 2 maps the driver's
// public-runtime predicted-display monotonic clock. Never mix those domains.
inline std::optional<uint64_t> displayTime(Calibration const& c,uint64_t target) {
    return c.version==1?mapCalibrationClock(c,target):std::nullopt;
}
inline std::optional<uint64_t> runtimeDisplayTime(Calibration const& c,uint64_t predicted) {
    return c.version==2?mapCalibrationClock(c,predicted):std::nullopt;
}
// Version 3 is a frame association fitted from a unique full pose-history
// tag and public OpenXR display time. It never accepts a rotational tie.
inline std::optional<uint64_t> poseDisplayTime(Calibration const& c,uint64_t poseTag) {
    return c.version==3&&poseTag?mapCalibrationClock(c,poseTag):std::nullopt;
}
inline MhlDepthPose compose(MhlDepthPose const& a,MhlDepthPose const& b) {
    MhlDepthPose r{};auto q=a.orientation;auto v=b.position;
    float t[3]={2*(q[1]*v[2]-q[2]*v[1]),2*(q[2]*v[0]-q[0]*v[2]),2*(q[0]*v[1]-q[1]*v[0])};
    r.position[0]=a.position[0]+v[0]+q[3]*t[0]+q[1]*t[2]-q[2]*t[1];
    r.position[1]=a.position[1]+v[1]+q[3]*t[1]+q[2]*t[0]-q[0]*t[2];
    r.position[2]=a.position[2]+v[2]+q[3]*t[2]+q[0]*t[1]-q[1]*t[0];
    auto p=b.orientation;
    r.orientation[0]=q[3]*p[0]+q[0]*p[3]+q[1]*p[2]-q[2]*p[1];
    r.orientation[1]=q[3]*p[1]-q[0]*p[2]+q[1]*p[3]+q[2]*p[0];
    r.orientation[2]=q[3]*p[2]+q[0]*p[1]-q[1]*p[0]+q[2]*p[3];
    r.orientation[3]=q[3]*p[3]-q[0]*p[0]-q[1]*p[1]-q[2]*p[2];return r;
}
inline bool align(MhlDepthPacket& p,Calibration const& c,uint64_t referenceSpace) {
    if(!valid(c,p.display_time_ns)||p.producer_id!=c.producerId||referenceSpace!=c.referenceSpace)return false;
    for(auto& eye:p.eyes)eye.pose=compose(c.trackingFromReference,eye.pose);
    p.flags|=MHL_TRACKING_ALIGNED;p.pose_space=MHL_SPACE_TRACKING;return true;
}
}
