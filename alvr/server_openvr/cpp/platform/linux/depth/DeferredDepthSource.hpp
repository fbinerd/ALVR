#pragma once
#include <cstdint>

namespace alvr::depth {
// Present-worker only. A late packet may attach only to the retained picture
// that requested it. Never relabel it with the following display slot's tag.
class DeferredDepthSource {
    uint64_t pending=0;
public:
    void reset(){pending=0;}
    // Keep one correction contract throughout a gap: no mid-gap flow rebuild.
    bool predictionBoundary(bool produced){if(!produced)return false;bool missed=pending!=0;pending=0;return missed;}
    void captured(uint64_t stamp,bool hasDepth,bool eligible){pending=eligible&&!hasDepth?stamp:0;}
    template<class Source,class Motion>
    bool retry(Source const& source,Motion* motion){
        if(!pending||!source||!motion)return false;
        auto depth=source(pending);
        if(!depth)return false;
        // The caller has not captured another picture: attachDepth targets the
        // latest retained capture. Its completed copy respects source lifetime.
        pending=0;return motion->attachDepth(*depth);
    }
};
}
