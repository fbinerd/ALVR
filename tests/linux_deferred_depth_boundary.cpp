// A packet arriving between two filled slots must not change their contract.
#include "platform/linux/depth/DeferredDepthSource.hpp"
#include <cassert>
#include <functional>
#include <optional>
#include <iostream>
struct Depth{uint64_t stamp;};
struct Motion{uint64_t latest;unsigned attached=0;bool attachDepth(Depth d){assert(d.stamp==latest);++attached;return true;}};
int main(){
    alvr::depth::DeferredDepthSource pending;Motion motion{101};bool ready=false;
    std::function<std::optional<Depth>(uint64_t)> source=[&](uint64_t stamp)->std::optional<Depth>{return ready?std::optional<Depth>{{stamp}}:std::nullopt;};
    pending.captured(101,false,true);assert(!pending.retry(source,&motion));
    pending.predictionBoundary();ready=true;
    assert(!pending.retry(source,&motion)&&motion.attached==0); // Late after first filled slot.
    motion.latest=202;pending.captured(202,false,true);
    assert(pending.retry(source,&motion)&&motion.attached==1); // New capture has a new deadline.
    assert(!pending.retry(source,&motion));
    pending.captured(303,false,true);pending.reset();assert(!pending.retry(source,&motion));
    std::cout<<"Late-gap rejection, next-capture attachment and source-reset cancellation passed\n";
}
