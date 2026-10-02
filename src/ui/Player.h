#pragma once

#include "engine/Engine.h"
#include "engine/PlaybackController.h"
#include "ui/TimelineModel.h"

#include <QObject>
#include <QString>

namespace scvp::ui {

class EngineBridge;
class FramePacer;
class VideoView;

// Connects the decode engine, the transport controller and the video view: runs the playback
// tick from the pacer while playing and on frame arrival / input while paused.
class Player : public QObject, public TimelineModel {
    Q_OBJECT
public:
    explicit Player(VideoView* view, QObject* parent = nullptr);
    ~Player() override;

    bool open(int slot, const QString& path, QString* error);
    void close(int slot);
    bool isOpen(int slot) const { return m_engine.isOpen(slot); }

    Engine& engine() { return m_engine; }
    PlaybackController& controller() { return m_controller; }

    // Transport (forwarded to the controller, then refreshed).
    void togglePlay();
    void step(int frames);
    void jump(int direction, bool large);
    void seek(int64_t position);
    void setSpeed(double speed);
    void setJumpSettings(const JumpSettings& j);
    void setLoopStart();
    void setLoopEnd();
    void toggleLoop();
    void clearLoop();
    void refresh();

    // Two videos.
    void setOffset(int64_t offset);
    int64_t offset() const { return m_controller.offset(); }
    void setSyncMode(SyncMode m);
    void swapSlots();
    // What the last presentation showed of each video (frame number, held at an end).
    const DisplayFrame& shown(int slot) const { return m_lastTick.frames[size_t(slot)]; }

    // TimelineModel
    int64_t length() const override { return m_controller.length(); }
    int64_t indexedFrames() const override;
    int64_t position() const override { return m_controller.position(); }
    double frameRate() const override { return m_controller.frameRate(); }
    bool playing() const override { return m_controller.playing(); }
    bool slotOpen(int slot) const override { return m_engine.isOpen(slot); }
    RangeSetPtr cachedRanges(int slot) const override { return m_engine.cachedRanges(slot); }
    int64_t slotOffset(int slot) const override { return m_controller.mapper().shift(slot); }
    bool hasLoop() const override { return m_controller.hasLoop(); }
    bool loopActive() const override { return m_controller.loopActive(); }
    int64_t loopFirst() const override { return m_controller.loopFirst(); }
    int64_t loopLast() const override { return m_controller.loopLast(); }
    void scrubBegin() override;
    void scrubTo(int64_t frame) override;
    void scrubEnd(int64_t frame) override;
    void setLoop(int64_t first, int64_t last) override;

signals:
    void transportChanged();  // loop, speed, jump settings, play state
    void positionChanged(qint64 position);
    void playingChanged(bool playing);
    void mediaChanged();
    void stalledChanged(bool stalled);

private:
    void onTick(double now);
    void present(const TickResult& r);

    Engine m_engine;
    EngineBackend m_backend;
    PlaybackController m_controller;
    EngineBridge* m_bridge;
    FramePacer* m_pacer;
    VideoView* m_view;
    TickResult m_lastTick;
    int64_t m_lastPosition = -1;
    bool m_lastPlaying = false;
    bool m_lastStalled = false;
    bool m_resumeAfterScrub = false;
};

} // namespace scvp::ui
