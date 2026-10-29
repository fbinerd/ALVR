#pragma once
#include "gpu_transport.hpp"
#include "../motion/NativeMotion.hpp"
#include <utility>

namespace alvr::depth {
// Owner creates/destroys this on the present thread, AFTER/BEFORE the driver
// device respectively. Device/queue must be the encoder's, never a private
// importer device. No Encoder or synthesis implementation is changed here.
class GameDepthSource {
public:
    using Depth=motion::NativeMotion::Depth;
    using QueryProvider=std::function<std::optional<mhl::depth::Query>(uint64_t)>;
    template<class Encoder>
    GameDepthSource(Encoder& encoder,mhl::depth::Device device,std::string socket,
                    std::string enableFile,QueryProvider query,mhl::depth::Logger log={})
        :receiver(std::make_shared<mhl::depth::Receiver>(device,std::move(socket),std::move(enableFile),std::move(log))) {
        std::weak_ptr<mhl::depth::Receiver> weak=receiver;
        encoder.setDepthSource([weak,query=std::move(query)](uint64_t timestamp)->std::optional<Depth>{
            auto owner=weak.lock();if(!owner)return {};
            auto expected=query(timestamp);
            if(!expected){owner->pollIngress();return {};}
            mhl::depth::Query q=*expected;
            // Never allow a replay's unaligned-space exemption into the driver.
            q.requireTrackingAligned=true;q.poseSpace=MHL_SPACE_TRACKING;
            if(q.targetTimestampNs!=timestamp)q.clockMapped=false;
            auto value=owner->acquire(q);if(!value)return {};
            return Depth{vk::Image(value->image),static_cast<vk::ImageLayout>(value->layout),vk::Extent2D{value->extent.width,value->extent.height}};
        });
        detach=[&encoder]{encoder.setDepthSource({});};
    }
    ~GameDepthSource(){detach();receiver.reset();}
    GameDepthSource(GameDepthSource const&)=delete;
    GameDepthSource& operator=(GameDepthSource const&)=delete;
private:
    std::shared_ptr<mhl::depth::Receiver> receiver;
    std::function<void()> detach;
};
}
