#pragma once

#include "engine/FrameIndex.h"

#include <array>
#include <cstdint>

namespace scvp {

enum class SyncMode {
    Frame,      // frame N of A is shown with frame N + offset of B (default)
    Timestamp,  // frames shown at the same time (different frame rates, VFR); offset in B frames
};

// Timing of one video as the mapper needs it.
struct SlotTiming {
    bool open = false;
    int64_t frames = 0;    // frame count (final or estimated)
    double fps = 25.0;
    FrameIndexPtr index;   // for exact times in timestamp mode (may be null)
};

// Maps the shared timeline to each video's own frame numbers. Timeline positions run from 0 to
// length() - 1 over the union of both videos once the B offset is applied; a video that has no
// frame at a position HOLDS its first or last frame there.
class TimelineMapper {
public:
    struct Local {
        int32_t frame = -1;  // frame to show (clamped into the video)
        bool hold = false;   // the position lies before the start or after the end of this video
    };

    void setSlot(int slot, const SlotTiming& t) { m_slots[size_t(slot)] = t; }
    void setOffset(int64_t bFramesAhead) { m_offset = bFramesAhead; }  // B shows frame A + offset
    void setMode(SyncMode m) { m_mode = m; }
    int64_t offset() const { return m_offset; }
    SyncMode mode() const { return m_mode; }

    int master() const;            // timing reference: A, or B alone
    bool bothOpen() const { return m_slots[0].open && m_slots[1].open; }
    int64_t length() const;        // timeline positions
    double fps() const;            // timeline rate (the master's)
    Local local(int slot, int64_t position) const;
    // Timeline position showing `frame` of `slot` (inverse of local, unclamped).
    int64_t positionOf(int slot, int64_t frame) const;
    // Timeline position = slot frame - shift(slot) in frame mode (for drawing cache bands).
    int64_t shift(int slot) const;

private:
    int64_t base() const;  // master frame shown at position 0
    double secondsOfMaster(int64_t masterFrame) const;
    int64_t frameAtSeconds(int slot, double s) const;

    std::array<SlotTiming, 2> m_slots;
    int64_t m_offset = 0;
    SyncMode m_mode = SyncMode::Frame;
};

} // namespace scvp
