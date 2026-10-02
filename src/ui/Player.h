#pragma once

#include "engine/Engine.h"
#include "engine/PlaybackController.h"

#include <QObject>
#include <QString>

namespace scvp::ui {

class EngineBridge;
class FramePacer;
class VideoView;

// Connects the decode engine, the transport controller and the video view: runs the playback
// tick from the pacer while playing and on frame arrival / input while paused.
class Player : public QObject {
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
    void refresh();

signals:
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
    int64_t m_lastPosition = -1;
    bool m_lastPlaying = false;
    bool m_lastStalled = false;
};

} // namespace scvp::ui
