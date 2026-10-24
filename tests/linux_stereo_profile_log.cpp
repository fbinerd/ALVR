#include "platform/linux/motion/StereoProfileLog.hpp"
#include <cassert>
int main(){
    alvr::motion::StereoProfileLog log;
    alvr::motion::StereoDepthTiming t;
    for(int i=0;i<500;i++)assert(!log.add(t));
    t.profiled=true;t.totalWallMs=4;t.passWaitCalls=5;
    for(int i=0;i<359;i++)assert(!log.add(t));
    auto result=log.add(t);assert(result);
    assert(result->find("n=360")!=std::string::npos);
    assert(result->find("wall=4.000")!=std::string::npos);
    assert(result->find("input_copy_gpu=-1.000")!=std::string::npos);
    assert(result->find("pass_wait_calls=5")!=std::string::npos);
    assert(!log.add(t));
}
