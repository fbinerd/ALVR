// TEST EXECUTABLE ONLY. No estimator is executed. Production NativeMotion::capture
// is compiled unchanged; replace just the NVIDIA-only estimator construction so
// the capture/copy/retention path can be checked offscreen on Intel.
#include "platform/linux/motion/NativeOpticalFlow.hpp"
namespace alvr::motion {
struct NativeOpticalFlow::State {};
int depthTestOpticalExecutions=0;
NativeOpticalFlow::NativeOpticalFlow(Context const&,uint32_t,uint32_t,VkOpticalFlowPerformanceLevelNV,bool):state(std::make_unique<State>()){}
NativeOpticalFlow::~NativeOpticalFlow()=default;
bool NativeOpticalFlow::execute(ImageResource const&,ImageResource const&,ImageResource&,ImageResource&,std::string& error,VkImageLayout,ImageResource*,ImageResource*) noexcept {
    ++depthTestOpticalExecutions;error="optical flow forbidden in capture-only Intel test";return false;
}
bool NativeOpticalFlow::quiesce(std::string&) noexcept{return true;}
bool NativeOpticalFlow::faulted()const{return false;}
double NativeOpticalFlow::opticalGpuMilliseconds()const{return -1;}
}
