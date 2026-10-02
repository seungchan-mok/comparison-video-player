#pragma once

#include "engine/DecodeLane.h"
#include "engine/FrameCache.h"
#include "engine/VideoSource.h"

#include <array>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

namespace scvp {

// What the user is doing with one video, in that video's own frame numbers.
struct PlaybackIntent {
    int32_t playhead = 0;
    int direction = 1;   // play direction, or the last manual move while paused
    bool playing = false;
    double speed = 1.0;
    FrameRange loop{0, -1};
    bool loopActive = false;
    std::vector<int32_t> jumpDistances;  // frames (arrow-key jumps) for jump-target prefetch
    int32_t aheadLimit = -1;             // pacing against the other video, -1 = none
};

struct SlotStats {
    size_t cachedFrames = 0;
    size_t cachedBytes = 0;
    size_t budgetBytes = 0;
    int32_t capacityFrames = 0;
    size_t poolLiveBytes = 0;
    uint64_t framesDecoded = 0;
    uint64_t seeks = 0;
    uint64_t cancellations = 0;
    double indexProgress = 0;
    bool indexComplete = false;
};

// Thread-safe facade over everything that decodes. Two slots (A = 0, B = 1), each with its own
// source, frame pool, cache and decode lane; the memory budget is split between open slots in
// proportion to their frame size so both keep the same number of frames. UI code calls
// request() whenever the playhead or play state changes and frame() to fetch what to show;
// neither ever waits for decoding.
class Engine {
public:
    static constexpr int kSlots = 2;

    Engine();
    ~Engine();
    Engine(const Engine&) = delete;
    Engine& operator=(const Engine&) = delete;

    bool open(int slot, const std::string& path, const InputOptions& opts, std::string* error);
    void close(int slot);
    bool isOpen(int slot) const;
    VideoInfo info(int slot) const;
    FrameIndexPtr index(int slot) const;

    void setMemoryBudget(size_t bytes);
    size_t memoryBudget() const;

    void request(int slot, const PlaybackIntent& intent);

    FramePtr frame(int slot, int32_t n) const;
    FramePtr nearestFrame(int slot, int32_t n, int32_t maxDistance) const;
    RangeSetPtr cachedRanges(int slot) const;
    SlotStats stats(int slot) const;
    int32_t capacityFrames(int slot) const;
    std::string debugState(int slot) const;  // plan, cache ranges and lane state (diagnostics)

    // Called from decode threads after a frame was cached / from scanner threads after the
    // index grew. Keep them cheap (set a flag, post an event).
    void setFramesCallback(std::function<void(int slot)> cb);
    void setIndexCallback(std::function<void(int slot)> cb);

    static size_t defaultMemoryBudget();  // 25 % of physical memory

private:
    struct Slot {
        uint64_t epoch = 0;
        std::unique_ptr<IVideoSource> source;
        std::shared_ptr<FramePool> pool;
        std::unique_ptr<FrameCache> cache;
        std::unique_ptr<WantMailbox> mailbox;
        std::unique_ptr<DecodeLane> lane;
        VideoInfo info;
        PlaybackIntent intent;
        bool hasIntent = false;
        size_t frameBytesEstimate = 0;
        size_t frameBytesPlanned = 0;
        size_t cacheBudget = 0;
        int32_t capacity = 1;
    };

    void rebalanceLocked();
    void replanLocked(Slot& s);
    void onFrames(int slot, uint64_t epoch);
    void onIndex(int slot, uint64_t epoch);
    size_t frameBytesOf(const Slot& s) const;

    mutable std::mutex m_mutex;
    std::array<std::unique_ptr<Slot>, kSlots> m_slots;
    uint64_t m_nextEpoch = 1;
    size_t m_budget;
    std::function<void(int)> m_framesCb;
    std::function<void(int)> m_indexCb;
};

} // namespace scvp
