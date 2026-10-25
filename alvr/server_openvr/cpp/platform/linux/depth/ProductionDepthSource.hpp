#pragma once
#include "GameDepthSource.hpp"
#include "clock_space_bridge.hpp"
#include "runtime_calibration.hpp"
#include <chrono>
#include <cstdlib>
#include <fstream>

namespace alvr::depth {
// One immutable description for the current Encoder::present. Set on its
// worker, under the same encoder mutex, before querying/capturing the picture.
struct ComposedDepthPicture {
    uint64_t timestamp{};
    mhl::depth::RuntimeObservation runtime{};
    MhlDepthPose head{{0,0,0},{0,0,0,1}};
    std::array<MhlDepthPose,2> localEyes{};
    std::array<std::array<float,4>,2> fov{},sourceUv{};
    bool unwarpedSingleLayer=false,viewsValid=false;
};
class ProductionDepthSource {
    std::optional<mhl::depth::Query> current;
    std::unique_ptr<GameDepthSource> binding;
    std::unique_ptr<mhl::depth::RuntimeCalibrationService> calibrator;
    std::optional<mhl::depth::Calibration> calibration;
    uint64_t checked=0;
    std::string directory;
public:
    explicit ProductionDepthSource(std::string testDirectory={}) : directory(std::move(testDirectory)) {
        if(directory.empty())if(auto home=std::getenv("HOME"))directory=std::string(home)+"/.config/alvr/";
    }
    template<class Encoder>
    void prepare(Encoder& encoder,mhl::depth::Device device,ComposedDepthPicture const& picture,mhl::depth::Logger log={}) {
        current.reset();if(directory.empty())return;
        auto now=uint64_t(std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now().time_since_epoch()).count());
        if(!checked||now-checked>=1000000000){
            checked=now;int enabled=0;std::ifstream(directory+"diag_depth_receiver")>>enabled;
            calibration=mhl::depth::loadCalibration(directory+"depth_calibration",now);
            if((enabled==1||enabled==2)&&!calibrator)
                calibrator=std::make_unique<mhl::depth::RuntimeCalibrationService>(directory+"depth_receiver.sock.calibrate",directory+"depth_calibration",log);
            if(enabled!=1&&enabled!=2)calibrator.reset();
            // Do not instantiate/bind at all on the normal default path.
            if(enabled==1&&!binding)binding=std::make_unique<GameDepthSource>(encoder,device,
                directory+"depth_receiver.sock",directory+"diag_depth_receiver",
                [this](uint64_t stamp)->std::optional<mhl::depth::Query>{
                    if(!current||current->targetTimestampNs!=stamp)return {};return current;
                },std::move(log));
        }
        if(calibrator)calibrator->observe(picture.runtime);
        if(!binding||!calibration||!mhl::depth::valid(*calibration,now)||!picture.viewsValid||!picture.unwarpedSingleLayer)return;
        auto display=mhl::depth::displayTime(*calibration,picture.timestamp);
        if(!display||!mhl::depth::valid(*calibration,*display))return;
        mhl::depth::Query q;q.targetTimestampNs=picture.timestamp;q.displayMonotonicNs=*display;
        q.toleranceNs=calibration->uncertaintyNs;q.producerId=calibration->producerId;
        q.clockMapped=q.fullStereoLayout=q.verifySourceUv=true;q.fov=picture.fov;q.sourceUv=picture.sourceUv;
        for(unsigned i=0;i<2;i++)q.poses[i]=mhl::depth::compose(picture.head,picture.localEyes[i]);
        current=q;
    }
};
}
