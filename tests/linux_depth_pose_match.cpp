// Offline: depth clocks cannot use a rotation-only match during translation.
#include "alvr_server/PoseHistory.h"
#include <cassert>
#include <cmath>
#include <iostream>
void Debug(const char*,...){}
int main(){
    PoseHistory history;vr::HmdMatrix34_t origin{{{1,0,0,2},{0,1,0,.3f},{0,0,1,-1}}};
    history.SetTransform(origin);
    FfiDeviceMotion m{};m.pose.orientation.w=1;m.pose.position[1]=1.5f;
    for(unsigned i=0;i<3;i++){m.pose.position[0]=float(i)*.01f;history.OnPoseUpdated(1000000000ull+i*13888889ull,m);}
    vr::HmdMatrix34_t submitted=origin;submitted.m[0][3]+=.01f;submitted.m[1][3]+=1.5f;
    auto exact=history.GetUniqueDepthPoseMatch(submitted);assert(exact&&exact->targetTimestampNs==1013888889ull);
    // The existing synthesis matcher still chooses the latest rotational tie.
    assert(history.GetBestPoseMatch(submitted)->targetTimestampNs==1027777778ull);
    m.pose.position[0]=.01f;history.OnPoseUpdated(1041666667ull,m);
    assert(!history.GetUniqueDepthPoseMatch(submitted)); // Ambiguous still pose.
    submitted.m[0][3]=NAN;assert(!history.GetUniqueDepthPoseMatch(submitted));
    std::cout<<"Unique translation-aware depth association, ambiguous/NaN rejection and unchanged synthesis matcher passed\n";
}
