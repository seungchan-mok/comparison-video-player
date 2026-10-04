#pragma once

#include "engine/Frame.h"

#include <cstdint>
#include <map>
#include <set>
#include <memory>
#include <mutex>
#include <vector>

namespace scvp {

struct FrameRange {
    int32_t first = 0;  // inclusive
    int32_t last = -1;  // inclusive
    bool gpu = false;   // RangeSet only: the run is cached in device memory

    bool contains(int32_t i) const { return i >= first && i <= last; }
    int32_t size() const { return last >= first ? last - first + 1 : 0; }
    bool operator==(const FrameRange& o) const { return first == o.first && last == o.last; }
    bool operator!=(const FrameRange& o) const { return !(*this == o); }
};

// Contiguous runs of cached frames (timeline display).
struct RangeSet {
    std::vector<FrameRange> ranges;  // consecutive frames in the same memory form one run
    size_t frames = 0;
    size_t bytes = 0;
    size_t gpuFrames = 0;
    size_t gpuBytes = 0;
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

// Decoded frames of one video keyed by frame number, in two tiers with separate byte budgets:
// device memory (frames decoded on the GPU kept there for zero-copy display) and system memory.
// Both tiers rank frames with the same CachePolicy, so the frames needed soonest sit in VRAM and
// the next ones in RAM. Thread-safe: decode lanes offer frames, the UI looks them up without
// waiting on decoding.
class FrameCache {
public:
    enum Tier { Ram = 0, Vram = 1 };

    explicit FrameCache(size_t budgetBytes = 0);

    void setBudget(size_t bytes);  // system memory
    size_t budget() const;
    size_t bytes() const;          // system memory in use
    size_t frames() const;         // both tiers
    void setGpuBudget(size_t bytes);
    size_t gpuBudget() const;
    size_t gpuBytes() const;
    size_t gpuFrames() const;
    // Whether a frame decoded on the GPU should stay there (VRAM tier has room for it or it is
    // needed sooner than the least needed VRAM frame).
    bool wantsGpu(int32_t frame, size_t bytes) const;

    void setPolicy(CachePolicy policy);
    CachePolicy policy() const;

    // Inserts the frame unless its tier is full and the frame would be the first to go, and
    // evicts the least needed frames of that tier to stay within budget. VRAM frames pushed out
    // that way are appended to `spilled` (when given) so the caller can copy them to system
    // memory and offer them again. Returns whether the frame was kept.
    bool offer(FramePtr frame, std::vector<FramePtr>* spilled = nullptr);

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
    // Highest cached frame number in [first, last], -1 if none.
    int32_t lastCached(int32_t first, int32_t last) const;
    // Cost of the least needed cached frame (what a new frame must beat when full).
    double worstCost() const;

    void clear();
    void dropProvisional();
    void erase(int32_t frame);

    RangeSetPtr ranges() const;   // snapshot, rebuilt only when the content changed
    uint64_t version() const;     // bumps on every insert/eviction

private:
    using Map = std::map<int32_t, FramePtr>;
    using Keys = std::set<int32_t>;
    static Tier tierOf(const Frame& f) { return f.location == FrameLocation::Cuda ? Vram : Ram; }
    void evictToBudgetLocked(Tier t, int32_t keep, std::vector<FramePtr>* spilled);
    int32_t worstLocked(Tier t) const;  // frame number, -1 if none
    void eraseLocked(Map::iterator it);
    void insertLocked(FramePtr f);
    void bumpLocked() { ++m_version; }

    mutable std::mutex m_mutex;
    Map m_frames;
    Keys m_keys[2];
    size_t m_bytes[2] = {0, 0};
    size_t m_budget[2] = {0, 0};
    CachePolicy m_policy;
    uint64_t m_version = 0;
    mutable uint64_t m_rangesVersion = ~uint64_t(0);
    mutable RangeSetPtr m_rangesSnapshot;
};

} // namespace scvp
