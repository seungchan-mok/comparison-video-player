#include "engine/PlaybackController.h"

#include <algorithm>
#include <cmath>

namespace scvp {

int32_t EngineBackend::frameCount(int slot) const
{
    FrameIndexPtr idx = m_engine.index(slot);
    return idx ? idx->frameCount() : 0;
}

bool EngineBackend::frameCountFinal(int slot) const
{
    FrameIndexPtr idx = m_engine.index(slot);
    return idx && idx->complete();
}

int32_t EngineBackend::frameCountEstimate(int slot) const
{
    const VideoInfo info = m_engine.info(slot);
    return int32_t(std::llround(info.durationSeconds * (info.nominalFrameRate > 0 ? info.nominalFrameRate : 25.0)));
}

double EngineBackend::frameRate(int slot) const
{
    FrameIndexPtr idx = m_engine.index(slot);
    return idx && idx->frameRate() > 0 ? idx->frameRate() : 25.0;
}

int32_t PlaybackBackend::cachedRunEnd(int slot, int32_t from) const
{
    int32_t last = from - 1;
    while (last < from + 4096 && frame(slot, last + 1))
        ++last;
    return last;
}

int32_t EngineBackend::cachedRunEnd(int slot, int32_t from) const
{
    RangeSetPtr rs = m_engine.cachedRanges(slot);
    if (!rs)
        return from - 1;
    const auto& r = rs->ranges;
    auto it = std::upper_bound(r.begin(), r.end(), from, [](int32_t v, const FrameRange& x) { return v < x.first; });
    if (it == r.begin() || !std::prev(it)->contains(from))
        return from - 1;
    --it;
    int32_t last = it->last;
    // Runs split where frames move between memory tiers; they are still contiguous.
    for (++it; it != r.end() && it->first == last + 1; ++it)
        last = it->last;
    return last;
}

PlaybackController::PlaybackController(PlaybackBackend& backend)
    : m_backend(backend)
{
}

int PlaybackController::masterSlot() const { return m_backend.isOpen(0) || !m_backend.isOpen(1) ? 0 : 1; }

TimelineMapper PlaybackController::mapper() const
{
    TimelineMapper m;
    for (int s = 0; s < 2; ++s) {
        SlotTiming t;
        t.open = m_backend.isOpen(s);
        if (t.open) {
            const int64_t n = m_backend.frameCount(s);
            t.frames = m_backend.frameCountFinal(s) ? n : std::max<int64_t>(n, m_backend.frameCountEstimate(s));
            t.fps = m_backend.frameRate(s) > 0 ? m_backend.frameRate(s) : 25.0;
            if (m_syncMode == SyncMode::Timestamp)
                t.index = m_backend.index(s);
        }
        m.setSlot(s, t);
    }
    m.setOffset(m_offset);
    m.setMode(m_syncMode);
    return m;
}

int64_t PlaybackController::length() const { return mapper().length(); }

bool PlaybackController::lengthFinal() const
{
    for (int s = 0; s < 2; ++s)
        if (m_backend.isOpen(s) && !m_backend.frameCountFinal(s))
            return false;
    return m_backend.isOpen(0) || m_backend.isOpen(1);
}

double PlaybackController::frameRate() const { return mapper().fps(); }

void PlaybackController::setOffset(int64_t offset)
{
    if (offset == m_offset)
        return;
    const TimelineMapper before = mapper();
    const int m = before.master();
    const int64_t masterFrame = before.local(m, m_position).frame;
    auto masterLoop = [&](int64_t p) { return p >= 0 ? before.local(m, p).frame : int64_t(-1); };
    const int64_t lf = masterLoop(m_loopFirst), ll = masterLoop(m_loopLast);
    m_offset = offset;
    const TimelineMapper after = mapper();
    m_position = clampPosition(after.positionOf(m, masterFrame));
    if (hasLoop()) {
        m_loopFirst = clampPosition(after.positionOf(m, lf));
        m_loopLast = clampPosition(after.positionOf(m, ll));
    }
    m_anchorTime = -1;
    m_stateDirty = true;
    requestAll();
}

void PlaybackController::setSyncMode(SyncMode mode)
{
    if (mode == m_syncMode)
        return;
    m_syncMode = mode;
    m_position = clampPosition(m_position);
    m_anchorTime = -1;
    m_stateDirty = true;
    requestAll();
}

int64_t PlaybackController::clampPosition(int64_t p) const
{
    const int64_t n = length();
    return n > 0 ? std::clamp<int64_t>(p, 0, n - 1) : 0;
}

void PlaybackController::mediaChanged()
{
    m_position = clampPosition(m_position);
    m_shown = {};
    m_shownLocal = {};
    m_shownNumber = -1;
    m_anchorTime = -1;
    if (hasLoop()) {
        m_loopFirst = clampPosition(m_loopFirst);
        m_loopLast = clampPosition(m_loopLast);
    }
    m_stateDirty = true;
    requestAll();
}

// Leader/follower pacing while two videos play: playback needs both frames of a position, so
// decoding much further ahead in one video than the other is decoded only takes decoder time
// and cache from the slower one. The faster video prefetches up to a second past the
// position the slower one has decoded to.
std::array<int32_t, 2> PlaybackController::paceLimits(const TimelineMapper& m) const
{
    std::array<int32_t, 2> limits{-1, -1};
    if (!m_playing || !m.bothOpen())
        return limits;
    const int64_t n = m.length();
    std::array<int64_t, 2> readyTo{};
    for (int s = 0; s < 2; ++s) {
        const TimelineMapper::Local l = m.local(s, m_position);
        const int64_t endFrame = m_backend.cachedRunEnd(s, l.frame);
        const int32_t lastFrame = m.local(s, n - 1).frame;
        // Decoded to its last frame: held from there on, nothing more to decode for the timeline.
        readyTo[size_t(s)] = endFrame >= lastFrame ? n : std::max<int64_t>(m_position, m.positionOf(s, endFrame));
    }
    const int64_t lead = std::max<int64_t>(8, int64_t(std::ceil(m.fps() * m_speed)));
    for (int s = 0; s < 2; ++s) {
        const int64_t follower = readyTo[size_t(1 - s)];
        if (follower >= n)
            continue;
        const TimelineMapper::Local l = m.local(s, std::min(n - 1, follower + lead));
        if (!l.hold || l.frame > m.local(s, m_position).frame)
            limits[size_t(s)] = l.frame;
    }
    return limits;
}

void PlaybackController::requestAll()
{
    const TimelineMapper m = mapper();
    const std::array<int32_t, 2> limits = paceLimits(m);
    for (int s = 0; s < 2; ++s) {
        if (!m_backend.isOpen(s))
            continue;
        PlaybackIntent in;
        in.playhead = m.local(s, m_position).frame;
        in.direction = m_playing ? 1 : m_direction;
        in.playing = m_playing;
        in.speed = m_speed;
        if (hasLoop()) {
            in.loop = {m.local(s, m_loopFirst).frame, m.local(s, m_loopLast).frame};
            in.loopActive = m_loopActive;
        }
        in.jumpDistances = {jumpFrames(false), jumpFrames(true)};
        in.aheadLimit = limits[size_t(s)];
        m_backend.request(s, in);
    }
}

// Every open video has its frame for `position` (lockstep).
bool PlaybackController::ready(const TimelineMapper& m, int64_t position) const
{
    for (int s = 0; s < 2; ++s)
        if (m_backend.isOpen(s) && !m_backend.frame(s, m.local(s, position).frame))
            return false;
    return true;
}

void PlaybackController::setPosition(int64_t p, int direction)
{
    p = clampPosition(p);
    if (direction)
        m_direction = direction > 0 ? 1 : -1;
    if (p == m_position && !m_stateDirty)
        return;
    m_position = p;
    m_stateDirty = false;
    requestAll();
}

void PlaybackController::play()
{
    if (length() <= 0)
        return;
    if (m_position >= length() - 1 && !(m_loopActive && hasLoop()))
        m_position = 0;  // play from the start again
    m_playing = true;
    m_direction = 1;
    m_anchorTime = -1;  // anchored on the next tick
    requestAll();
}

void PlaybackController::pause()
{
    if (!m_playing)
        return;
    m_playing = false;
    m_stallSince = -1;
    // Stay on what is on screen.
    if (m_shownNumber >= 0)
        m_position = m_shownNumber;
    requestAll();
}

void PlaybackController::togglePlay() { m_playing ? pause() : play(); }

void PlaybackController::setSpeed(double speed)
{
    m_speed = std::clamp(speed, 1.0 / 16.0, 16.0);
    m_anchorTime = -1;
    requestAll();
}

void PlaybackController::step(int frames)
{
    if (!frames)
        return;
    if (m_playing)
        pause();
    // Steps accumulate from the requested position, so fast taps are never lost.
    setPosition(m_position + frames, frames);
}

int32_t PlaybackController::jumpFrames(bool large) const
{
    const double v = large ? m_jump.large : m_jump.small;
    const double frames = m_jump.inFrames ? v : v * frameRate();
    return std::max<int32_t>(1, int32_t(std::llround(frames)));
}

void PlaybackController::jump(int direction, bool large)
{
    const int64_t delta = int64_t(jumpFrames(large)) * (direction >= 0 ? 1 : -1);
    if (m_playing) {
        m_position = clampPosition(m_position + delta);
        m_anchorTime = -1;
        requestAll();
        return;
    }
    setPosition(m_position + delta, direction);
}

void PlaybackController::seek(int64_t position)
{
    const int dir = position >= m_position ? 1 : -1;
    if (m_playing) {
        m_position = clampPosition(position);
        m_anchorTime = -1;
        requestAll();
        return;
    }
    setPosition(position, dir);
}

void PlaybackController::setLoopStart()
{
    m_loopFirst = m_position;
    if (m_loopLast < m_loopFirst)
        m_loopLast = std::max<int64_t>(m_loopFirst, length() - 1);
    m_loopActive = true;
    requestAll();
}

void PlaybackController::setLoopEnd()
{
    m_loopLast = m_position;
    if (m_loopFirst < 0 || m_loopFirst > m_loopLast)
        m_loopFirst = 0;
    m_loopActive = true;
    requestAll();
}

void PlaybackController::setLoop(int64_t first, int64_t last)
{
    m_loopFirst = clampPosition(std::min(first, last));
    m_loopLast = clampPosition(std::max(first, last));
    m_loopActive = true;
    requestAll();
}

void PlaybackController::toggleLoop()
{
    if (!hasLoop())
        return;
    m_loopActive = !m_loopActive;
    requestAll();
}

void PlaybackController::clearLoop()
{
    m_loopFirst = m_loopLast = -1;
    m_loopActive = false;
    requestAll();
}

void PlaybackController::setJumpSettings(const JumpSettings& j)
{
    m_jump = j;
    requestAll();
}

TickResult PlaybackController::tick(double now)
{
    TickResult r;
    const TimelineMapper map = mapper();
    const int64_t n = map.length();
    if (n <= 0) {
        r.changed = m_shown[0] || m_shown[1];
        m_shown = {};
        m_shownLocal = {};
        m_shownNumber = -1;
        return r;
    }

    if (m_playing) {
        const double fps = map.fps() * m_speed;
        if (m_anchorTime < 0) {
            m_anchorTime = now;
            m_anchorPos = m_position;
        }
        int64_t due = m_anchorPos + int64_t(std::floor((now - m_anchorTime) * fps + 1e-6));
        const bool looping = m_loopActive && hasLoop() && m_position >= m_loopFirst && m_position <= m_loopLast;
        if (looping && due > m_loopLast) {
            // Wrap, keeping the fractional phase so the loop length stays exact.
            const int64_t loopLen = m_loopLast - m_loopFirst + 1;
            due = m_loopFirst + (due - m_loopFirst) % loopLen;
            m_anchorPos = due;
            m_anchorTime = now;
            m_position = due;
            requestAll();
        } else if (due >= n) {
            due = n - 1;
        }
        if (due > m_position) {
            // Advance through positions whose frames are all decoded (never skip one that is
            // missing); if the next one is not there yet, stall and restart the clock there.
            int64_t best = m_position;
            while (best < due && ready(map, best + 1))
                ++best;
            if (best == m_position) {
                r.stalled = true;
                if (m_stallSince < 0)
                    m_stallSince = now;
                r.stalledSeconds = now - m_stallSince;
                m_anchorPos = m_position;
                m_anchorTime = now;
            } else {
                if (best < due) {
                    m_anchorPos = best;
                    m_anchorTime = now;
                }
                m_stallSince = -1;
                m_position = best;
                requestAll();
            }
        }
        if (!looping && m_position >= n - 1 && ready(map, n - 1)) {
            m_playing = false;  // reached the end
            requestAll();
        }
    }

    // What to present: the pair at the position, or keep the previous pair until both arrive.
    if (ready(map, m_position)) {
        for (int s = 0; s < 2; ++s) {
            FramePtr f;
            TimelineMapper::Local l;
            if (m_backend.isOpen(s)) {
                l = map.local(s, m_position);
                f = m_backend.frame(s, l.frame);
            }
            r.changed |= f != m_shown[size_t(s)];
            m_shown[size_t(s)] = f;
            m_shownLocal[size_t(s)] = l;
        }
        m_shownNumber = m_position;
    }
    for (int s = 0; s < 2; ++s)
        r.frames[size_t(s)] = {m_shown[size_t(s)], m_shownLocal[size_t(s)].frame, m_shownNumber == m_position,
                               m_shownLocal[size_t(s)].hold};
    r.position = m_position;
    return r;
}

} // namespace scvp
