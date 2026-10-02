#pragma once

#include "engine/FrameCache.h"
#include "engine/Mailbox.h"
#include "engine/VideoSource.h"

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <set>
#include <string>
#include <thread>

namespace scvp {

struct LaneStats {
    std::atomic<uint64_t> framesDecoded{0};  // frames the decoder returned
    std::atomic<uint64_t> framesCached{0};   // of those, kept by the cache
    std::atomic<uint64_t> seeks{0};
    std::atomic<uint64_t> cancellations{0};
    std::atomic<uint64_t> errors{0};
};

// Worker thread that keeps one video's cache filled with what the latest WantList asks for.
// For each want (in priority order) it finds the first missing frame, then either keeps
// decoding from where the decoder is or seeks to the frame's key packet, whichever needs fewer
// packets; frames before the wanted one are decoded with AVDISCARD_NONREF. A newer WantList
// cancels the current decode within a frame; since the decoder state survives, the next pick
// continues without seeking whenever that is cheaper.
class DecodeLane {
public:
    using FramesFn = std::function<void()>;

    DecodeLane(IVideoSource& source, FrameCache& cache, WantMailbox& mailbox, SessionConfig cfg, FramesFn onFrames);
    ~DecodeLane();
    DecodeLane(const DecodeLane&) = delete;
    DecodeLane& operator=(const DecodeLane&) = delete;

    bool start(std::string* error);
    void stop();

    const LaneStats& stats() const { return m_stats; }
    size_t frameBytes() const { return m_frameBytes.load(std::memory_order_relaxed); }
    // Human-readable state (current task, decoder position) for stall diagnostics.
    std::string describe() const;

private:
    struct Task {
        SeekPoint key;
        int32_t first = 0;  // frames to obtain, inclusive
        int32_t last = 0;
        bool seek = false;
        WantClass cls = WantClass::Window;
        int32_t keepFrom = 0;  // frames >= keepFrom that are not cached are worth decoding fully
    };

    void run();
    std::optional<Task> pick(const WantList& w, const FrameIndex& idx);
    void exec(const Task& t, const FrameIndex& idx, uint64_t generation);
    int32_t firstMissing(int32_t first, int32_t last) const;
    int32_t lastMissing(int32_t first, int32_t last) const;
    bool continueBeatsSeek(const SeekPoint& key, int32_t frame, const FrameIndex& idx) const;
    void markUnreachable(int32_t first, int32_t last);

    IVideoSource& m_source;
    FrameCache& m_cache;
    WantMailbox& m_mailbox;
    SessionConfig m_cfg;
    FramesFn m_onFrames;
    std::unique_ptr<IDecodeSession> m_session;
    std::thread m_thread;
    std::atomic<bool> m_stop{false};
    LaneStats m_stats;
    std::atomic<size_t> m_frameBytes{0};
    bool m_forceSeek = false;
    // Frames the decoder cannot produce (corrupt or dropped); skipped until the index changes.
    std::set<int32_t> m_unreachable;
    FrameIndexPtr m_unreachableFor;
    mutable std::mutex m_describeMutex;
    std::string m_description;
};

} // namespace scvp
