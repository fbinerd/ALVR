#pragma once
#include "StereoDepth.hpp"
#include <array>
#include <iomanip>
#include <optional>
#include <sstream>
#include <string>

namespace alvr::motion {
// Optional stage diagnostics; no formatting/allocation when profiling is off.
class StereoProfileLog {
    using T=StereoDepthTiming;
    inline static constexpr std::array<double T::*,16> fields={
        &T::totalWallMs,&T::coarseWallMs,&T::lumaWallMs,&T::fineDepthWallMs,&T::mergeWallMs,
        &T::passWaitCpuMs,&T::flowWaitCpuMs,&T::flowRecordCpuMs,&T::flowSubmitCpuMs,
        &T::flowInputCopyGpuMs,&T::flowOutputCopyGpuMs,&T::lumaGpuMs,&T::fineDepthGpuMs,
        &T::coarseLumaGpuMs,&T::coarseDepthGpuMs,&T::mergeGpuMs};
    inline static constexpr std::array<char const*,16> names={
        "wall","coarse","luma","fine_depth","merge","pass_wait_cpu","nv_wait_cpu",
        "nv_record_cpu","nv_submit_cpu","input_copy_gpu","output_copy_gpu","luma_gpu",
        "fine_depth_gpu","coarse_luma_gpu","coarse_depth_gpu","merge_gpu"};
    std::array<double,16> sum{};
    std::array<bool,16> unknown{};
    uint64_t count=0;
public:
    std::optional<std::string> add(T const& t){
        if(!t.profiled)return {};
        for(size_t i=0;i<fields.size();++i){double v=t.*fields[i];if(v<0)unknown[i]=true;else sum[i]+=v;}
        if(++count<360)return {};
        std::ostringstream out;out<<std::fixed<<std::setprecision(3)<<"Stereo profile mean_ms n="<<count;
        for(size_t i=0;i<fields.size();++i)out<<' '<<names[i]<<'='<<(unknown[i]?-1:sum[i]/count);
        out<<" completion_deferred="<<t.completionDeferred<<" pass_wait_calls="<<t.passWaitCalls<<" optical_includes_copies="<<t.flowGpuIncludesCopies;
        count=0;sum.fill(0);unknown.fill(false);return out.str();
    }
};
}
