#include "ui/Player.h"

#include "ui/EngineBridge.h"
#include "ui/FramePacer.h"
#include "ui/VideoView.h"

namespace scvp::ui {

Player::Player(VideoView* view, QObject* parent)
    : QObject(parent)
    , m_backend(m_engine)
    , m_controller(m_backend)
    , m_bridge(new EngineBridge(m_engine, this))
    , m_pacer(new FramePacer(view, this))
    , m_view(view)
{
    connect(m_pacer, &FramePacer::tick, this, &Player::onTick);
    connect(m_bridge, &EngineBridge::framesArrived, this, [this] {
        if (!m_controller.playing())
            refresh();
    });
    connect(m_bridge, &EngineBridge::indexChanged, this, [this] {
        m_controller.mediaChanged();
        emit mediaChanged();
        refresh();
    });
}

Player::~Player()
{
    m_pacer->setActive(false);
    for (int s = 0; s < Engine::kSlots; ++s)
        m_engine.close(s);
}

bool Player::open(int slot, const QString& path, QString* error)
{
    std::string err;
    if (!m_engine.open(slot, path.toUtf8().toStdString(), InputOptions{}, &err)) {
        if (error)
            *error = QString::fromStdString(err);
        return false;
    }
    m_controller.mediaChanged();
    emit mediaChanged();
    refresh();
    return true;
}

void Player::close(int slot)
{
    m_engine.close(slot);
    m_controller.mediaChanged();
    emit mediaChanged();
    refresh();
}

void Player::togglePlay()
{
    m_controller.togglePlay();
    refresh();
}

void Player::step(int frames)
{
    m_controller.step(frames);
    refresh();
}

void Player::jump(int direction, bool large)
{
    m_controller.jump(direction, large);
    refresh();
}

void Player::seek(int64_t position)
{
    m_controller.seek(position);
    refresh();
}

void Player::setSpeed(double speed)
{
    m_controller.setSpeed(speed);
    refresh();
    emit transportChanged();
}

void Player::setJumpSettings(const JumpSettings& j)
{
    m_controller.setJumpSettings(j);
    emit transportChanged();
}

void Player::setLoopStart()
{
    m_controller.setLoopStart();
    refresh();
    emit transportChanged();
}

void Player::setLoopEnd()
{
    m_controller.setLoopEnd();
    refresh();
    emit transportChanged();
}

void Player::toggleLoop()
{
    m_controller.toggleLoop();
    refresh();
    emit transportChanged();
}

void Player::clearLoop()
{
    m_controller.clearLoop();
    refresh();
    emit transportChanged();
}

void Player::setLoop(int64_t first, int64_t last)
{
    m_controller.setLoop(first, last);
    refresh();
    emit transportChanged();
}

int64_t Player::indexedFrames() const
{
    FrameIndexPtr idx = m_engine.index(m_controller.masterSlot());
    return idx ? idx->frameCount() : 0;
}

void Player::scrubBegin()
{
    m_resumeAfterScrub = m_controller.playing();
    if (m_resumeAfterScrub)
        m_controller.pause();
}

void Player::scrubTo(int64_t frame) { seek(frame); }

void Player::scrubEnd(int64_t frame)
{
    seek(frame);
    if (m_resumeAfterScrub)
        togglePlay();
    m_resumeAfterScrub = false;
}

void Player::onTick(double now) { present(m_controller.tick(now)); }

void Player::refresh() { present(m_controller.tick(m_pacer->now())); }

void Player::present(const TickResult& r)
{
    if (r.changed)
        m_view->setFrames({r.frames[0].frame, r.frames[1].frame});
    m_pacer->setActive(m_controller.playing());
    if (r.position != m_lastPosition) {
        m_lastPosition = r.position;
        emit positionChanged(r.position);
    }
    if (m_controller.playing() != m_lastPlaying) {
        m_lastPlaying = m_controller.playing();
        emit playingChanged(m_lastPlaying);
        emit transportChanged();
    }
    if (r.stalled != m_lastStalled) {
        m_lastStalled = r.stalled;
        emit stalledChanged(r.stalled);
    }
}

} // namespace scvp::ui
