#pragma once

#include "engine/FrameCache.h"

#include <cstdint>

namespace scvp::ui {

// What the timeline shows and drives; implemented by Player, faked in tests.
class TimelineModel {
public:
    virtual ~TimelineModel() = default;

    virtual int64_t length() const = 0;          // frames (estimated while indexing)
    virtual int64_t indexedFrames() const = 0;   // frames whose numbering is final
    virtual int64_t position() const = 0;
    virtual double frameRate() const = 0;
    virtual bool playing() const = 0;
    virtual bool slotOpen(int slot) const = 0;
    virtual RangeSetPtr cachedRanges(int slot) const = 0;  // in timeline frames
    virtual int64_t slotOffset(int slot) const = 0;        // timeline frame = slot frame - offset

    virtual bool hasLoop() const = 0;
    virtual bool loopActive() const = 0;
    virtual int64_t loopFirst() const = 0;
    virtual int64_t loopLast() const = 0;

    virtual void scrubBegin() = 0;
    virtual void scrubTo(int64_t frame) = 0;
    virtual void scrubEnd(int64_t frame) = 0;
    virtual void setLoop(int64_t first, int64_t last) = 0;
};

} // namespace scvp::ui
