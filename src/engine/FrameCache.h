#pragma once

#include "engine/Frame.h"

#include <cstdint>
#include <map>
#include <memory>
#include <mutex>
#include <vector>

namespace scvp {

struct FrameRange {
    int32_t first = 0;  // inclusive
    int32_t last = -1;  // inclusive

    bool contains(int32_t i) const { return i >= first && i <= last; }
    int32_t size() const { return last >= first ? last - first + 1 : 0; }
    bool operator==(const FrameRange& o) const { return first == o.first && last == o.last; }
};

// Contiguous runs of cached frames (timeline display).
struct RangeSet {
    std::vector<FrameRange> ranges;
    size_t frames = 0;
    size_t bytes = 0;
};
using RangeSetPtr = std::shared_ptr<const RangeSet>;

// Which frames matter most: a frame's cost is the time until playback needs it. Ahead of the
// playhead (in play direction) that is distance/(fps*speed); behind it the distance is weighted
// by behindWeight (1 when paused, ~4 when playing). With an active loop and forward playback,
// frames before the playhead inside the loop are reached by wrapping around and frames outside
// the loop are not needed at all.
struct CachePolicy {
    int32_t playhead = 0;
    int direction = 1;          // +1 forward, -1 backward
    double behindWeight = 1.0;
    FrameRange loop{0, -1};     // empty = no loop
    bool loopWraps = false;     // loop active and playing forward
    std::vector<FrameRange> protect;  // never evicted (target frame, pinned loop, ...)

    double cost(int32_t frame) const;
    bool isProtected(int32_t frame) const;
};

// Decoded frames of one video keyed by frame number, bounded by a byte budget. Thread-safe:
// decode lanes offer frames, the UI looks them up without waiting on decoding.
class FrameCache {
public:
    explicit FrameCache(size_t budgetBytes = 0);

    void setBudget(size_t bytes);
    size_t budget() const;
    size_t bytes() const;
    size_t frames() const;

    void setPolicy(CachePolicy policy);
    CachePolicy policy() const;

    // Inserts the frame unless the cache is full and the frame would be the first to go.
    // Evicts the least needed frames to stay within budget. Returns whether it was kept.
    bool offer(FramePtr frame);

    FramePtr get(int32_t frame) const;
    bool contains(int32_t frame) const;
    // Closest cached frame within maxDistance (ties prefer the earlier frame); null if none.
    FramePtr nearest(int32_t frame, int32_t maxDistance) const;
    // First/last frame number in [first, last] that is not cached, -1 if all are.
    int32_t firstMissing(int32_t first, int32_t last) const;
    int32_t lastMissing(int32_t first, int32_t last) const;
    // End (inclusive) of the run of missing frames starting at `from`, not beyond `limit`.
    int32_t missingRunEnd(int32_t from, int32_t limit) const;
    int32_t countCached(int32_t first, int32_t last) const;
    // Cost of the least needed cached frame (what a new frame must beat when full).
    double worstCost() const;

    void clear();
    void dropProvisional();
    void erase(int32_t frame);

    RangeSetPtr ranges() const;   // snapshot, rebuilt only when the content changed
    uint64_t version() const;     // bumps on every insert/eviction

private:
    using Map = std::map<int32_t, FramePtr>;
    void evictToBudgetLocked(int32_t keep);
    Map::iterator worstLocked();
    void bumpLocked() { ++m_version; }

    mutable std::mutex m_mutex;
    Map m_frames;
    size_t m_bytes = 0;
    size_t m_budget = 0;
    CachePolicy m_policy;
    uint64_t m_version = 0;
    mutable uint64_t m_rangesVersion = ~uint64_t(0);
    mutable RangeSetPtr m_rangesSnapshot;
};

} // namespace scvp
