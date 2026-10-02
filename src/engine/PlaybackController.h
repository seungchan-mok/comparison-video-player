#pragma once

#include "engine/Engine.h"
#include "engine/TimelineMapper.h"

#include <array>
#include <cstdint>

namespace scvp {

// What the controller needs from the decoding side; Engine in the app, a fake in tests.
class PlaybackBackend {
public:
    virtual ~PlaybackBackend() = default;
    virtual bool isOpen(int slot) const = 0;
    virtual int32_t frameCount(int slot) const = 0;  // frames indexed so far
    // Whether frameCount() is final; while a file is still being indexed, positions may go up
    // to frameCountEstimate() (duration x frame rate) so early seeks are not clamped.
    virtual bool frameCountFinal(int slot) const = 0;
    virtual int32_t frameCountEstimate(int slot) const = 0;
    virtual double frameRate(int slot) const = 0;
    virtual FramePtr frame(int slot, int32_t n) const = 0;
    virtual void request(int slot, const PlaybackIntent& intent) = 0;
    virtual FrameIndexPtr index(int) const { return nullptr; }  // exact times (timestamp sync)
    // Last frame of the run of decoded frames starting at `from` (from - 1 if it is missing).
    virtual int32_t cachedRunEnd(int slot, int32_t from) const;
};

class EngineBackend final : public PlaybackBackend {
public:
    explicit EngineBackend(Engine& e)
        : m_engine(e)
    {
    }
    bool isOpen(int slot) const override { return m_engine.isOpen(slot); }
    int32_t frameCount(int slot) const override;
    bool frameCountFinal(int slot) const override;
    int32_t frameCountEstimate(int slot) const override;
    double frameRate(int slot) const override;
    FramePtr frame(int slot, int32_t n) const override { return m_engine.frame(slot, n); }
    void request(int slot, const PlaybackIntent& intent) override { m_engine.request(slot, intent); }
    FrameIndexPtr index(int slot) const override { return m_engine.index(slot); }
    int32_t cachedRunEnd(int slot, int32_t from) const override;

private:
    Engine& m_engine;
};

// Arrow-key jump sizes, in seconds or frames.
struct JumpSettings {
    double small = 5.0;
    double large = 30.0;
    bool inFrames = false;
};

struct DisplayFrame {
    FramePtr frame;
    int32_t number = -1;  // frame number of `frame`
    bool exact = false;   // frame == the requested position (false while it is still decoding)
    bool hold = false;    // the timeline is outside this video: its first or last frame is held
};

struct TickResult {
    std::array<DisplayFrame, 2> frames;
    int64_t position = 0;
    bool changed = false;    // something different should be presented
    bool stalled = false;    // playing but the next frame is not decoded yet
    double stalledSeconds = 0;
};

// Transport logic on the UI thread: play/pause, frame steps, jumps, seeks, A-B loop and the
// per-vsync tick that decides which frames to present. Positions are on the shared timeline
// (TimelineMapper): with one video they are its frame numbers; with two, B is shown at A + offset
// and a video outside the timeline holds its first or last frame. Lockstep: a position is shown
// only when the frames of both videos are decoded, so a mismatched pair never reaches the
// screen. Never blocks: when the next frame is not decoded yet, playback stalls and resumes from
// there (re-anchored, no catch-up burst), and stepping keeps showing the previous pair until the
// requested one arrives.
class PlaybackController {
public:
    explicit PlaybackController(PlaybackBackend& backend);

    void mediaChanged();  // a video was opened or closed

    void play();
    void pause();
    void togglePlay();
    bool playing() const { return m_playing; }
    void setSpeed(double speed);
    double speed() const { return m_speed; }

    void step(int frames);                // +-n frames, pauses
    void jump(int direction, bool large);  // arrow keys: +-small or +-large jump
    void seek(int64_t position);
    void goToStart() { seek(0); }
    void goToEnd() { seek(length() - 1); }

    void setLoopStart();
    void setLoopEnd();
    void setLoop(int64_t first, int64_t last);
    void toggleLoop();
    void clearLoop();
    bool loopActive() const { return m_loopActive; }
    bool hasLoop() const { return m_loopLast >= m_loopFirst && m_loopFirst >= 0; }
    int64_t loopFirst() const { return m_loopFirst; }
    int64_t loopLast() const { return m_loopLast; }

    void setJumpSettings(const JumpSettings& j);
    JumpSettings jumpSettings() const { return m_jump; }
    int32_t jumpFrames(bool large) const;

    TickResult tick(double nowSeconds);

    int64_t position() const { return m_position; }
    int64_t length() const;        // frames, final or estimated
    bool lengthFinal() const;
    double frameRate() const;
    int masterSlot() const;

    // Two-video sync: B shows frame (A + offset). Changing it keeps the master frame on screen.
    void setOffset(int64_t offset);
    int64_t offset() const { return m_offset; }
    void setSyncMode(SyncMode m);
    SyncMode syncMode() const { return m_syncMode; }
    TimelineMapper mapper() const;  // current mapping (lengths change while indexing)
    // True when the frame at position() is on screen: key repeat waits for this.
    bool positionShown() const { return m_shownNumber == m_position; }
    int lastDirection() const { return m_direction; }

private:
    void requestAll();
    void setPosition(int64_t p, int direction);
    int64_t clampPosition(int64_t p) const;
    bool ready(const TimelineMapper& m, int64_t position) const;
    std::array<int32_t, 2> paceLimits(const TimelineMapper& m) const;

    PlaybackBackend& m_backend;
    int64_t m_position = 0;
    int64_t m_shownNumber = -1;
    std::array<FramePtr, 2> m_shown;
    std::array<TimelineMapper::Local, 2> m_shownLocal;
    bool m_playing = false;
    double m_speed = 1.0;
    int m_direction = 1;
    double m_anchorTime = -1.0;
    int64_t m_anchorPos = 0;
    double m_stallSince = -1.0;
    bool m_stateDirty = true;
    JumpSettings m_jump;
    int64_t m_loopFirst = -1;
    int64_t m_loopLast = -1;
    bool m_loopActive = false;
    int64_t m_offset = 0;
    SyncMode m_syncMode = SyncMode::Frame;
};

} // namespace scvp
