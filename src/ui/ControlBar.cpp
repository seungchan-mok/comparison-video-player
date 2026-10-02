#include "ui/ControlBar.h"

#include "ui/Player.h"
#include "ui/TimelineWidget.h"

#include <QAbstractItemView>
#include <QComboBox>
#include <QDoubleSpinBox>
#include <QSpinBox>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QLabel>
#include <QPainter>
#include <QStyle>
#include <QToolButton>
#include <QVBoxLayout>

#include <cmath>

namespace scvp::ui {
namespace {

QDoubleSpinBox* makeJumpBox(QWidget* parent, double value)
{
    auto* b = new QDoubleSpinBox(parent);
    b->setRange(0.001, 100000);
    b->setDecimals(3);
    b->setValue(value);
    b->setKeyboardTracking(false);    // commit on Enter / focus loss, not on every digit
    b->setFocusPolicy(Qt::ClickFocus);  // never steals the arrow keys from the player
    b->setButtonSymbols(QAbstractSpinBox::NoButtons);
    b->setFixedWidth(64);
    b->setAlignment(Qt::AlignRight);
    return b;
}

QString timecode(int64_t frame, double fps)
{
    const double s = double(frame) / (fps > 0 ? fps : 25.0);
    const int h = int(s / 3600);
    const int m = int(s / 60) % 60;
    return QStringLiteral("%1:%2:%3").arg(h).arg(m, 2, 10, QLatin1Char('0')).arg(std::fmod(s, 60.0), 6, 'f', 3, QLatin1Char('0'));
}

} // namespace

ControlBar::ControlBar(Player* player, QWidget* parent)
    : QWidget(parent)
    , m_player(player)
    , m_timeline(new TimelineWidget(player, this))
    , m_play(new QToolButton(this))
    , m_time(new QLabel(this))
    , m_jumpSmall(makeJumpBox(this, 5))
    , m_jumpLarge(makeJumpBox(this, 30))
    , m_jumpUnit(new QComboBox(this))
    , m_speed(new QComboBox(this))
    , m_offset(new QSpinBox(this))
    , m_offsetLabel(new QLabel(tr("B offset"), this))
    , m_loop(new QToolButton(this))
    , m_loopLabel(new QLabel(this))
    , m_cache(new QLabel(this))
{
    setFocusPolicy(Qt::NoFocus);
    m_play->setFocusPolicy(Qt::NoFocus);
    m_play->setAutoRaise(true);
    m_play->setToolTip(tr("Play / pause (Space)"));
    connect(m_play, &QToolButton::clicked, m_player, &Player::togglePlay);

    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_time->setFont(mono);
    m_time->setMinimumWidth(QFontMetrics(mono).horizontalAdvance(QStringLiteral("000000 / 000000  0:00:00.000")) + 8);

    m_jumpUnit->addItem(tr("s"));
    m_jumpUnit->addItem(tr("frames"));
    m_jumpUnit->setFocusPolicy(Qt::NoFocus);
    m_jumpSmall->setToolTip(tr("Jump with Left / Right"));
    m_jumpLarge->setToolTip(tr("Jump with Down / Up"));
    connect(m_jumpSmall, &QDoubleSpinBox::valueChanged, this, &ControlBar::applyJumps);
    connect(m_jumpLarge, &QDoubleSpinBox::valueChanged, this, &ControlBar::applyJumps);
    connect(m_jumpUnit, &QComboBox::currentIndexChanged, this, &ControlBar::applyJumps);

    for (double s : {0.25, 0.5, 1.0, 2.0, 4.0})
        m_speed->addItem(QStringLiteral("%1x").arg(s), s);
    m_speed->setCurrentIndex(2);
    m_speed->setFocusPolicy(Qt::NoFocus);
    m_speed->setToolTip(tr("Playback speed"));
    connect(m_speed, &QComboBox::currentIndexChanged, this, [this](int i) { m_player->setSpeed(m_speed->itemData(i).toDouble()); });

    m_offset->setRange(-1000000, 1000000);
    m_offset->setKeyboardTracking(false);
    m_offset->setFocusPolicy(Qt::ClickFocus);
    m_offset->setFixedWidth(72);
    m_offset->setAlignment(Qt::AlignRight);
    m_offset->setToolTip(tr("B shows frame A + offset (< and > change it)"));
    connect(m_offset, &QSpinBox::valueChanged, this, [this](int v) { m_player->setOffset(v); });

    m_loop->setText(tr("Loop"));
    m_loop->setCheckable(true);
    m_loop->setFocusPolicy(Qt::NoFocus);
    m_loop->setAutoRaise(true);
    m_loop->setToolTip(tr("Loop on/off (L); set with [ and ]"));
    connect(m_loop, &QToolButton::clicked, m_player, &Player::toggleLoop);
    m_loopLabel->setFont(mono);
    m_cache->setFont(mono);
    m_cache->setToolTip(tr("Decoded frames held in memory"));

    auto* row = new QHBoxLayout;
    row->setContentsMargins(6, 0, 6, 4);
    row->setSpacing(8);
    row->addWidget(m_play);
    row->addWidget(m_time);
    row->addStretch(1);
    row->addWidget(new QLabel(tr("Jump"), this));
    row->addWidget(m_jumpSmall);
    row->addWidget(m_jumpLarge);
    row->addWidget(m_jumpUnit);
    row->addSpacing(8);
    row->addWidget(m_offsetLabel);
    row->addWidget(m_offset);
    row->addSpacing(8);
    row->addWidget(m_speed);
    row->addWidget(m_loop);
    row->addWidget(m_loopLabel);
    row->addSpacing(8);
    row->addWidget(m_cache);

    auto* col = new QVBoxLayout(this);
    col->setContentsMargins(0, 2, 0, 0);
    col->setSpacing(0);
    col->addWidget(m_timeline);
    col->addLayout(row);

    connect(m_player, &Player::positionChanged, this, &ControlBar::syncPosition);
    connect(m_player, &Player::positionChanged, m_timeline, &TimelineWidget::positionChanged);
    connect(m_player, &Player::transportChanged, this, &ControlBar::syncTransport);
    connect(m_player, &Player::transportChanged, m_timeline, &TimelineWidget::modelChanged);
    connect(m_player, &Player::mediaChanged, this, [this] {
        syncOffset();
        syncTransport();
        syncPosition();
        m_timeline->modelChanged();
    });
    m_stats.setInterval(250);
    connect(&m_stats, &QTimer::timeout, this, &ControlBar::syncStats);
    m_stats.start();
    syncOffset();
    syncTransport();
    syncPosition();
}

void ControlBar::syncOffset()
{
    const bool two = m_player->slotOpen(0) && m_player->slotOpen(1);
    m_offset->setVisible(two);
    m_offsetLabel->setVisible(two);
    if (m_offset->value() != m_player->offset()) {
        const QSignalBlocker block(m_offset);
        m_offset->setValue(int(m_player->offset()));
    }
}

void ControlBar::setJumpSettings(const JumpSettings& j)
{
    const QSignalBlocker b1(m_jumpSmall);
    const QSignalBlocker b2(m_jumpLarge);
    const QSignalBlocker b3(m_jumpUnit);
    m_jumpSmall->setValue(j.small);
    m_jumpLarge->setValue(j.large);
    m_jumpUnit->setCurrentIndex(j.inFrames ? 1 : 0);
}

void ControlBar::applyJumps()
{
    JumpSettings j;
    j.small = m_jumpSmall->value();
    j.large = m_jumpLarge->value();
    j.inFrames = m_jumpUnit->currentIndex() == 1;
    m_player->setJumpSettings(j);
}

void ControlBar::syncTransport()
{
    const bool playing = m_player->playing();
    m_play->setIcon(style()->standardIcon(playing ? QStyle::SP_MediaPause : QStyle::SP_MediaPlay));
    m_loop->setChecked(m_player->loopActive());
    m_loop->setEnabled(m_player->hasLoop());
    m_loopLabel->setText(m_player->hasLoop() ? QStringLiteral("%1-%2").arg(m_player->loopFirst()).arg(m_player->loopLast()) : QString());
}

void ControlBar::syncPosition()
{
    const int64_t p = m_player->position();
    const int64_t n = m_player->length();
    m_time->setText(QStringLiteral("%1 / %2  %3").arg(p).arg(n).arg(timecode(p, m_player->frameRate())));
}

void ControlBar::syncStats()
{
    size_t bytes = 0;
    size_t frames = 0;
    size_t budget = 0;
    size_t gpu = 0;
    for (int s = 0; s < Engine::kSlots; ++s)
        if (m_player->slotOpen(s)) {
            const SlotStats st = m_player->engine().stats(s);
            bytes += st.cachedBytes;
            frames += st.cachedFrames;
            budget += st.budgetBytes;
            gpu += st.gpuBytes;
        }
    QString t = QStringLiteral("%1/%2 MB %3 fr").arg(bytes >> 20).arg(budget >> 20).arg(frames);
    if (gpu)
        t += QStringLiteral(" +%1 MB GPU").arg(gpu >> 20);
    m_cache->setText(t);
}

void ControlBar::setOverlay(bool overlay)
{
    m_overlay = overlay;
    setAutoFillBackground(!overlay);
    update();
}

bool ControlBar::hasActivePopup() const
{
    return m_speed->view()->isVisible() || m_jumpUnit->view()->isVisible();
}

void ControlBar::paintEvent(QPaintEvent* e)
{
    if (m_overlay) {
        QPainter p(this);
        p.fillRect(rect(), QColor(0, 0, 0, 160));
    }
    QWidget::paintEvent(e);
}

} // namespace scvp::ui
