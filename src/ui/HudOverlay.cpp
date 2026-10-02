#include "ui/HudOverlay.h"

#include "ui/Player.h"

#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPainter>

#include <cmath>

namespace scvp::ui {

HudOverlay::HudOverlay(Player* player, QWidget* parent)
    : QWidget(parent)
    , m_player(player)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    m_refresh.setInterval(250);
    connect(&m_refresh, &QTimer::timeout, this, qOverload<>(&QWidget::update));
    connect(m_player, &Player::positionChanged, this, qOverload<>(&QWidget::update));
    m_stallDelay.setSingleShot(true);
    m_stallDelay.setInterval(100);  // brief waits do not flash a badge
    connect(&m_stallDelay, &QTimer::timeout, this, [this] {
        m_showStall = m_stalled;
        update();
    });
    parent->installEventFilter(this);
    reposition();
    show();
}

void HudOverlay::reposition()
{
    if (parentWidget())
        setGeometry(parentWidget()->rect());
    raise();
}

bool HudOverlay::eventFilter(QObject* obj, QEvent* e)
{
    if (obj == parentWidget() && e->type() == QEvent::Resize)
        reposition();
    return QWidget::eventFilter(obj, e);
}

void HudOverlay::setInfoVisible(bool on)
{
    m_info = on;
    on ? m_refresh.start() : m_refresh.stop();
    update();
}

void HudOverlay::setStalled(bool stalled)
{
    m_stalled = stalled;
    if (stalled) {
        m_stallDelay.start();
    } else {
        m_stallDelay.stop();
        m_showStall = false;
        update();
    }
}

QStringList HudOverlay::lines() const
{
    QStringList out;
    for (int slot = 0; slot < Engine::kSlots; ++slot) {
        if (!m_player->slotOpen(slot))
            continue;
        Engine& e = m_player->engine();
        const VideoInfo info = e.info(slot);
        const FrameIndexPtr idx = e.index(slot);
        const SlotStats st = e.stats(slot);
        const int64_t pos = m_player->position();
        const FramePtr f = e.frame(slot, int32_t(pos));
        const QString name = QFileInfo(QString::fromStdString(info.path)).fileName();
        out << QStringLiteral("%1  %2").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B"), name);
        QString fmt = QString::fromLatin1(av_get_pix_fmt_name(f ? f->format() : info.pixelFormat));
        out << QStringLiteral("   %1 %2x%3 %4  %5 fps").arg(QString::fromStdString(info.codecName)).arg(info.width).arg(info.height).arg(fmt)
                   .arg(idx ? idx->frameRate() : info.nominalFrameRate, 0, 'f', 3);
        QString frameLine = QStringLiteral("   frame %1 / %2").arg(pos).arg(idx ? idx->frameCount() : 0);
        if (f) {
            const double t = idx ? idx->secondsOf(f->pres) : 0.0;
            frameLine += QStringLiteral("  %1 s  %2%3").arg(t, 0, 'f', 3).arg(QLatin1Char(f->pictType)).arg(f->key ? QStringLiteral(" key") : QString());
        } else {
            frameLine += QStringLiteral("  decoding...");
        }
        out << frameLine;
        out << QStringLiteral("   cache %1 frames %2 MB / %3 MB, %4 decoded, %5 seeks%6")
                   .arg(st.cachedFrames)
                   .arg(st.cachedBytes >> 20)
                   .arg(st.budgetBytes >> 20)
                   .arg(st.framesDecoded)
                   .arg(st.seeks)
                   .arg(st.indexComplete ? QString() : QStringLiteral(", indexing %1%").arg(int(st.indexProgress * 100)));
    }
    return out;
}

void HudOverlay::paintEvent(QPaintEvent*)
{
    if (!m_info && !m_showStall)
        return;
    QPainter p(this);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    p.setFont(mono);
    const QFontMetrics fm(mono);
    if (m_info) {
        const QStringList ls = lines();
        int w = 0;
        for (const QString& l : ls)
            w = std::max(w, fm.horizontalAdvance(l));
        const QRect box(8, 8, w + 16, fm.lineSpacing() * int(ls.size()) + 12);
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(QColor(235, 235, 235));
        int y = box.top() + 6 + fm.ascent();
        for (const QString& l : ls) {
            p.drawText(box.left() + 8, y, l);
            y += fm.lineSpacing();
        }
    }
    if (m_showStall) {
        const QString t = tr("Waiting for decoding...");
        const QRect box(width() - fm.horizontalAdvance(t) - 28, 8, fm.horizontalAdvance(t) + 20, fm.height() + 10);
        p.fillRect(box, QColor(160, 60, 20, 200));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, t);
    }
}

} // namespace scvp::ui
