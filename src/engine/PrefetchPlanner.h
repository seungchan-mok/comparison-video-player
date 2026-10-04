#pragma once

#include "engine/FrameCache.h"

#include <cstdint>
#include <vector>

namespace scvp {

enum class WantClass : uint8_t {
    Target,  // the frame that must be shown now
    Near,    // the next few frames in either direction (steps, start of playback)
    Window,  // the rest of the prefetch window (and a pinned loop)
    Island,  // a few frames at the next jump targets (instant arrow-key jumps)
};

// A range of frames a decode lane should make sure is cached. `fill` gives the order:
// +1 from the first missing frame upwards, -1 from the last missing frame downwards
// (nearest to the playhead first when the range lies behind it).
struct Want {
    FrameRange range;
    WantClass cls = WantClass::Window;
    int fill = 1;
};

struct WantList {
    uint64_t generation = 0;
    int32_t target = -1;
    int direction = 1;
    bool playing = false;
    std::vector<Want> wants;  // priority order
    CachePolicy policy;       // eviction policy consistent with the wants
};

struct PlannerInput {
    int32_t playhead = 0;
    int32_t frameCount = 0;
    int direction = 1;        // play direction, or the last manual move while paused
    bool playing = false;
    double speed = 1.0;
    double fps = 30.0;
    int32_t capacity = 0;     // frames that fit in this video's cache budget
    int32_t gop = 0;          // typical GOP length around the playhead (0 = unknown)
    FrameRange loop{0, -1};
    bool loopActive = false;
    std::vector<int32_t> jumpDistances;  // frames; islands are planned at playhead +- each
    int32_t aheadLimit = -1;  // leader pacing: no prefetch beyond this frame (-1 = none)
};

// Splits the cache capacity into what to decode next. Paused: 60 % towards the last move,
// 40 % the other way, at least the previous GOP + 8 behind when two GOPs fit (instant backward
// steps across a GOP boundary). Playing: 80 % ahead, 20 % behind. The returned CachePolicy uses
// the same proportions so the cache keeps exactly what is planned.
WantList planPrefetch(const PlannerInput& in);

} // namespace scvp
