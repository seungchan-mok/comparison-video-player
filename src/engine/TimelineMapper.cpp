#include "engine/TimelineMapper.h"

#include <algorithm>
#include <cmath>

namespace scvp {

int TimelineMapper::master() const { return m_slots[0].open || !m_slots[1].open ? 0 : 1; }

double TimelineMapper::fps() const
{
    const double f = m_slots[size_t(master())].fps;
    return f > 0 ? f : 25.0;
}

double TimelineMapper::secondsOfMaster(int64_t f) const
{
    const SlotTiming& m = m_slots[size_t(master())];
    if (m.index && f >= 0 && f < m.index->frameCount() && m.index->ordering() == FrameIndex::Ordering::Pts)
        return m.index->secondsOf(int32_t(f));
    return double(f) / fps();
}

int64_t TimelineMapper::frameAtSeconds(int slot, double s) const
{
    const SlotTiming& t = m_slots[size_t(slot)];
    if (t.index && t.index->frameCount() > 0 && t.index->ordering() == FrameIndex::Ordering::Pts && s >= 0 &&
        s <= t.index->secondsOf(t.index->frameCount() - 1))
        return t.index->presAtSeconds(s);
    return int64_t(std::floor(s * (t.fps > 0 ? t.fps : 25.0) + 1e-6));
}

// Local frame of B for master frame m (unclamped).
static int64_t bFrameFrame(int64_t m, int64_t off) { return m + off; }

int64_t TimelineMapper::base() const
{
    if (!bothOpen())
        return 0;
    if (m_mode == SyncMode::Frame)
        return std::min<int64_t>(0, -m_offset);
    // Timestamp mode: B frame 0 shown at master frame whose time equals B's time of frame -offset.
    const double bFps = m_slots[1].fps > 0 ? m_slots[1].fps : 25.0;
    const int64_t firstB = int64_t(std::floor(-double(m_offset) / bFps * fps() + 1e-6));
    return std::min<int64_t>(0, firstB);
}

int64_t TimelineMapper::length() const
{
    const int m = master();
    const int64_t nm = m_slots[size_t(m)].open ? m_slots[size_t(m)].frames : 0;
    if (!bothOpen())
        return nm;
    const int64_t b0 = base();
    int64_t endMaster = nm;  // exclusive end in master frames
    if (m_mode == SyncMode::Frame) {
        endMaster = std::max<int64_t>(endMaster, m_slots[1].frames - m_offset);
    } else {
        const double bFps = m_slots[1].fps > 0 ? m_slots[1].fps : 25.0;
        endMaster = std::max<int64_t>(endMaster, int64_t(std::ceil(double(m_slots[1].frames - m_offset) / bFps * fps() - 1e-6)));
    }
    return std::max<int64_t>(0, endMaster - b0);
}

TimelineMapper::Local TimelineMapper::local(int slot, int64_t position) const
{
    Local r;
    const SlotTiming& t = m_slots[size_t(slot)];
    if (!t.open || t.frames <= 0)
        return r;
    const int64_t m = position + base();  // master frame (may be outside the master)
    int64_t f;
    if (slot == master() || !bothOpen()) {
        f = m;
    } else if (m_mode == SyncMode::Frame) {
        f = bFrameFrame(m, m_offset);
    } else {
        f = frameAtSeconds(1, secondsOfMaster(m)) + m_offset;
    }
    r.hold = f < 0 || f >= t.frames;
    r.frame = int32_t(std::clamp<int64_t>(f, 0, t.frames - 1));
    return r;
}

int64_t TimelineMapper::positionOf(int slot, int64_t frame) const
{
    if (slot == master() || !bothOpen())
        return frame - base();
    if (m_mode == SyncMode::Frame)
        return frame - m_offset - base();
    const double bFps = m_slots[1].fps > 0 ? m_slots[1].fps : 25.0;
    return int64_t(std::floor(double(frame - m_offset) / bFps * fps() + 1e-6)) - base();
}

int64_t TimelineMapper::shift(int slot) const
{
    if (slot == master() || !bothOpen())
        return base();
    if (m_mode == SyncMode::Frame)
        return base() + m_offset;
    // Approximation for drawing only (equal rates assumed).
    return base() + m_offset;
}

} // namespace scvp
