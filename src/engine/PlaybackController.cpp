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

PlaybackController::PlaybackController(PlaybackBackend& backend)
    : m_backend(backend)
{
}

int PlaybackController::masterSlot() const { return m_backend.isOpen(0) || !m_backend.isOpen(1) ? 0 : 1; }

int64_t PlaybackController::length() const
{
    const int s = masterSlot();
    if (!m_backend.isOpen(s))
        return 0;
    const int64_t n = m_backend.frameCount(s);
    return m_backend.frameCountFinal(s) ? n : std::max<int64_t>(n, m_backend.frameCountEstimate(s));
}

bool PlaybackController::lengthFinal() const
{
    const int s = masterSlot();
    return m_backend.isOpen(s) && m_backend.frameCountFinal(s);
}

double PlaybackController::frameRate() const
{
    const int s = masterSlot();
    const double fps = m_backend.isOpen(s) ? m_backend.frameRate(s) : 0.0;
    return fps > 0 ? fps : 25.0;
}

int64_t PlaybackController::clampPosition(int64_t p) const
{
    const int64_t n = length();
    return n > 0 ? std::clamp<int64_t>(p, 0, n - 1) : 0;
}

void PlaybackController::mediaChanged()
{
    m_position = clampPosition(m_position);
    m_shown.reset();
    m_shownNumber = -1;
    m_anchorTime = -1;
    if (hasLoop()) {
        m_loopFirst = clampPosition(m_loopFirst);
        m_loopLast = clampPosition(m_loopLast);
    }
    m_stateDirty = true;
    requestAll();
}

void PlaybackController::requestAll()
{
    const int s = masterSlot();
    if (!m_backend.isOpen(s))
        return;
    PlaybackIntent in;
    in.playhead = int32_t(m_position);
    in.direction = m_playing ? 1 : m_direction;
    in.playing = m_playing;
    in.speed = m_speed;
    if (hasLoop()) {
        in.loop = {int32_t(m_loopFirst), int32_t(m_loopLast)};
        in.loopActive = m_loopActive;
    }
    in.jumpDistances = {jumpFrames(false), jumpFrames(true)};
    m_backend.request(s, in);
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
    const int s = masterSlot();
    if (!m_backend.isOpen(s) || length() <= 0) {
        r.changed = m_shown != nullptr;
        m_shown.reset();
        m_shownNumber = -1;
        return r;
    }
    const int64_t n = length();

    if (m_playing) {
        const double fps = frameRate() * m_speed;
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
            // Advance through decoded frames only (never skip one that is missing); if the
            // next one is not there yet, stall and restart the clock from where we stopped.
            int64_t best = m_position;
            while (best < due && m_backend.frame(s, int32_t(best + 1)))
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
        if (!looping && m_position >= n - 1 && m_backend.frame(s, int32_t(n - 1))) {
            m_playing = false;  // reached the end
            requestAll();
        }
    }

    // What to present: the frame at the position, or keep the previous one until it arrives.
    FramePtr f = m_backend.frame(s, int32_t(m_position));
    if (f) {
        r.changed = f != m_shown;
        m_shown = f;
        m_shownNumber = m_position;
    }
    r.frames[size_t(s)] = {m_shown, int32_t(m_shownNumber), m_shownNumber == m_position};
    r.position = m_position;
    return r;
}

} // namespace scvp
