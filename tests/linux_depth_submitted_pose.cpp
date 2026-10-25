// Offline-only: cached standing/raw origin must include translation as well
// as rotation. Never changes the driver's normal pose matcher.
#include "alvr_server/PoseHistory.h"
#include "alvr_server/Utils.h"
#include <cassert>
#include <cmath>
#include <iostream>
void Debug(const char*,...){}
int main(){
    PoseHistory history;vr::HmdMatrix34_t origin{},raw{},submitted{};
    HmdMatrix_QuatToMat(std::cos(.2),0,std::sin(.2),0,&origin);
    origin.m[0][3]=3;origin.m[1][3]=.2;origin.m[2][3]=-1;
    HmdMatrix_QuatToMat(std::cos(.4),0,std::sin(.4),0,&raw);
    raw.m[0][3]=1;raw.m[1][3]=1.5;raw.m[2][3]=-2;
    for(unsigned i=0;i<3;i++){
        for(unsigned j=0;j<3;j++)for(unsigned k=0;k<3;k++)submitted.m[i][j]+=origin.m[i][k]*raw.m[k][j];
        submitted.m[i][3]=origin.m[i][3];for(unsigned k=0;k<3;k++)submitted.m[i][3]+=origin.m[i][k]*raw.m[k][3];
    }
    history.SetTransform(origin);auto recovered=history.TrackingFromSubmitted(submitted);
    for(unsigned i=0;i<3;i++)for(unsigned j=0;j<4;j++)assert(std::abs(recovered.m[i][j]-raw.m[i][j])<1e-5);
    std::cout<<"Nonidentity submitted-to-driver rotation and origin roundtrip passed\n";
}
