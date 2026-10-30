#pragma once
// CPU-only world-point oracle: does decoded geometry stay world locked after
// client reprojection? Does not invoke optical flow, Vulkan or SteamVR.
#include "alvr_server/Utils.h"
#include "alvr_server/include/openvr_math.h"
#include "platform/linux/motion/FramePoseContract.hpp"
#include "platform/linux/motion/PicturePoseClock.hpp"
#include <algorithm>
#include <cassert>
#include <iostream>
#include <limits>

namespace headposecheck {
using V = std::array<double,3>;
inline V rotate(vr::HmdQuaternion_t q, V p) {
    vr::HmdMatrix34_t r{};
    HmdMatrix_QuatToMat(q.w,q.x,q.y,q.z,&r);
    V out{};
    for (unsigned i=0;i<3;++i) for(unsigned j=0;j<3;++j) out[i]+=r.m[i][j]*p[j];
    return out;
}
inline V shaderRotate(vr::HmdQuaternion_t q, V v) {
    // Independent GLSL quaternion expression, checked against the CPU matrix.
    auto cross=[](V a,V b) { return V{a[1]*b[2]-a[2]*b[1],a[2]*b[0]-a[0]*b[2],a[0]*b[1]-a[1]*b[0]}; };
    V xyz{q.x,q.y,q.z};auto c=cross(xyz,v);
    for(unsigned i=0;i<3;++i)c[i]+=q.w*v[i];
    c=cross(xyz,c);
    for(unsigned i=0;i<3;++i)v[i]+=2*c[i];
    return v;
}
inline vr::HmdQuaternion_t quaternion(unsigned axis, double t) {
    double angle=.25*std::sin(2*std::acos(-1.)*t/1.7);
    vr::HmdQuaternion_t q{std::cos(angle/2),0,0,0};
    (axis==0?q.x:axis==1?q.y:q.z)=std::sin(angle/2);
    return q;
}
inline double run() {
    double worstFixed=0;
    unsigned cases=0;
    for(unsigned axis=0;axis<3;++axis) {
        double legacyMax=0,fixedMax=0,legacyJudder=0,fixedJudder=0;
        std::array<double,2> oldError{},olderError{},oldFixed{},olderFixed{};
        for(unsigned frame=0;frame<180;++frame) {
            double t=frame/72.;
            // Actual compositor pose can lie between tracking samples. The
            // delay changes, as prediction and nearest-orientation matches do.
            auto source=quaternion(axis,t+.004+.003*std::sin(frame*.31));
            auto label=quaternion(axis,t);
            if(frame%3) source=label; // Synthesized slots target the label pose.
            vr::HmdMatrix34_t submitted{};
            HmdMatrix_QuatToMat(source.w,source.x,source.y,source.z,&submitted);
            auto actual=vrmath::quaternionFromRotationMatrix(submitted);
            auto packet=alvr::motion::encodedFramePose(actual,{0,1.6f,0});
            assert(packet);
            auto p=packet->orientation;
            vr::HmdQuaternion_t fixed{p.w,p.x,p.y,p.z};
            for(unsigned eye=0;eye<2;++eye) {
                double left=eye?-0.83:-1.28,right=eye?1.31:.86;
                double up=eye?1.02:1.08,down=eye?-1.00:-.94;
                for(V world: {V{.2,.3,-2},V{-.5,-.4,-3},V{.6,.5,-4}}) {
                    auto ray=rotate(vrmath::quaternionConjugate(source),world);
                    auto shader=shaderRotate(vrmath::quaternionConjugate(source),world);
                    for(unsigned c=0;c<3;++c)assert(std::abs(ray[c]-shader[c])<1e-6);
                    // Reconstruct the world direction from the image ray and
                    // the packet pose, then project to a fixed display view.
                    auto wrong=rotate(label,ray),correct=rotate(fixed,ray);
                    std::array<double,2> e{
                        ((wrong[0]/-wrong[2])-(world[0]/-world[2]))*1440/(right-left),
                        ((wrong[1]/-wrong[2])-(world[1]/-world[2]))*1504/(down-up)};
                    std::array<double,2> f{
                        ((correct[0]/-correct[2])-(world[0]/-world[2]))*1440/(right-left),
                        ((correct[1]/-correct[2])-(world[1]/-world[2]))*1504/(down-up)};
                    legacyMax=std::max(legacyMax,std::hypot(e[0],e[1]));
                    fixedMax=std::max(fixedMax,std::hypot(f[0],f[1]));
                    if(eye==0 && world[0]==.2) {
                        if(frame>=2) for(unsigned c=0;c<2;++c) {
                            legacyJudder=std::max(legacyJudder,std::abs(oldError[c]-(olderError[c]+e[c])/2));
                            fixedJudder=std::max(fixedJudder,std::abs(oldFixed[c]-(olderFixed[c]+f[c])/2));
                        }
                        olderError=oldError;oldError=e;olderFixed=oldFixed;oldFixed=f;
                    }
                    ++cases;
                }
            }
        }
        worstFixed=std::max(worstFixed,fixedMax);
        std::cout<<"axis="<<(axis==0?"pitch":axis==1?"yaw":"roll")
            <<" legacy_max_px="<<legacyMax<<" explicit_max_px="<<fixedMax
            <<" legacy_phase_judder_px="<<legacyJudder<<" explicit_phase_judder_px="<<fixedJudder<<'\n';
        assert(legacyMax>.25 && legacyJudder>.25 && fixedMax<.001 && fixedJudder<.001);
    }
    // A rebased translation query is valid geometry, but it is NOT the
    // translation in the output tracking entry. Packet position must follow it.
    auto mapped=alvr::motion::pictureTargetTime(1'000'000'000,1'020'000'000,1'033'888'889);
    assert(mapped && *mapped==1'013'888'889);
    auto q=quaternion(1,1);
    auto a=alvr::motion::encodedFramePose(q,{.01f,1.6f,0});
    auto b=alvr::motion::encodedFramePose(q,{.03f,1.6f,0});
    assert(a && b && std::abs(a->position[0]-b->position[0])>.019);
    assert(!alvr::motion::encodedFramePose({0,0,0,0},{0,0,0}));
    assert(!alvr::motion::encodedFramePose({1,0,0,0},{std::numeric_limits<float>::quiet_NaN(),0,0}));
    assert(!alvr::motion::encodedFramePose({1,std::numeric_limits<double>::infinity(),0,0},{0,0,0}));
    auto flipped=alvr::motion::encodedFramePose({-q.w,-q.x,-q.y,-q.z},{0,0,0});
    auto normal=alvr::motion::encodedFramePose(q,{0,0,0});
    assert(flipped && normal && alvr::motion::orientationErrorDegrees(flipped->orientation,normal->orientation)<1e-5);
    std::cout<<cases<<" asymmetric stereo world-point samples; no GPU; bounded invalid/translation/antipodal checks passed\n";
    return worstFixed;
}
}
