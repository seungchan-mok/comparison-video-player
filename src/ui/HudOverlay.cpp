#include "ui/HudOverlay.h"

#include "ui/Player.h"
#include "ui/VideoView.h"

#include "engine/GpuFrame.h"

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

QString HudOverlay::pathText(int slot) const
{
    auto* view = qobject_cast<const VideoView*>(parentWidget());
    switch (view ? view->uploadPath(slot) : render::UploadPath::None) {
    case render::UploadPath::ZeroCopy: return QStringLiteral(", to screen: zero-copy");
    case render::UploadPath::Download: return QStringLiteral(", to screen: download (GL on another GPU)");
    case render::UploadPath::Upload: return QStringLiteral(", to screen: upload");
    default: return QString();
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
        const DisplayFrame& shown = m_player->shown(slot);
        const int64_t pos = shown.number;
        const FramePtr f = shown.frame;
        const QString name = QFileInfo(QString::fromStdString(info.path)).fileName();
        out << QStringLiteral("%1  %2").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B"), name);
        QString fmt = QString::fromLatin1(av_get_pix_fmt_name(f ? f->format() : info.pixelFormat));
        out << QStringLiteral("   %1 %2x%3 %4  %5 fps").arg(QString::fromStdString(info.codecName)).arg(info.width).arg(info.height).arg(fmt)
                   .arg(idx ? idx->frameRate() : info.nominalFrameRate, 0, 'f', 3);
        QString frameLine = QStringLiteral("   frame %1 / %2").arg(pos).arg(idx ? idx->frameCount() : 0);
        if (slot == 1 && m_player->slotOpen(0))
            frameLine += QStringLiteral(" (offset %1%2)").arg(m_player->offset() > 0 ? QStringLiteral("+") : QString()).arg(m_player->offset());
        if (shown.hold)
            frameLine += QStringLiteral("  HOLD");
        if (f) {
            const double t = idx ? idx->secondsOf(f->pres) : 0.0;
            frameLine += QStringLiteral("  %1 s  %2%3").arg(t, 0, 'f', 3).arg(QLatin1Char(f->pictType)).arg(f->key ? QStringLiteral(" key") : QString());
        } else {
            frameLine += QStringLiteral("  decoding...");
        }
        out << frameLine;
        QString decoder = QStringLiteral("   decoder %1").arg(st.hardwareDevice.empty() ? QStringLiteral("software")
                                                                                 : QString::fromStdString(st.hardwareDevice));
        if (st.gpuBudgetBytes && !st.hardwareDevice.empty())
            decoder += QStringLiteral("  VRAM %1 frames %2 / %3 MB, %4 spilled%5")
                           .arg(st.gpuFrames)
                           .arg(st.gpuBytes >> 20)
                           .arg(st.gpuBudgetBytes >> 20)
                           .arg(st.spills)
                           .arg(pathText(slot));
        out << decoder;
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

// Which video each part of the view shows, and whether it is holding an end frame.
void HudOverlay::paintPaneLabels(QPainter& p, const QFontMetrics& fm)
{
    auto* view = qobject_cast<const VideoView*>(parentWidget());
    if (!view || !m_player->slotOpen(0) || !m_player->slotOpen(1))
        return;
    const double dpr = view->devicePixelRatioF();
    const render::CompareLayout layout = view->layout();
    const render::CompareSettings& c = view->compare();
    auto label = [&](int slot) {
        QString t = slot == 0 ? QStringLiteral("A") : QStringLiteral("B");
        if (m_player->shown(slot).hold)
            t += tr(" HOLD");
        return t;
    };
    auto draw = [&](QPoint at, const QString& t, bool right, bool warn) {
        const int w = fm.horizontalAdvance(t) + 10;
        const QRect box(right ? at.x() - w : at.x(), at.y(), w, fm.height() + 4);
        p.fillRect(box, warn ? QColor(150, 90, 0, 200) : QColor(0, 0, 0, 150));
        p.setPen(QColor(240, 240, 240));
        p.drawText(box, Qt::AlignCenter, t);
    };
    const int top = m_info ? -1 : 8;  // the info box takes the top-left corner
    for (const render::Pane& pane : layout.panes) {
        const QRect r(QPoint(int(pane.rect.x() / dpr), int(pane.rect.y() / dpr)),
                      QSize(int(pane.rect.width() / dpr), int(pane.rect.height() / dpr)));
        const int y = top < 0 ? r.bottom() - fm.height() - 12 : r.top() + top;
        if (layout.panes.size() > 1 || c.mode == render::CompareMode::Flip) {
            draw(QPoint(r.right() - 8, y), label(pane.slot), true, m_player->shown(pane.slot).hold);
        } else if (c.mode == render::CompareMode::Wipe) {
            const int x = r.left() + int(c.wipe * r.width());
            draw(QPoint(x - 6, y), label(0), true, m_player->shown(0).hold);
            draw(QPoint(x + 6, y), label(1), false, m_player->shown(1).hold);
        } else {
            const QString t = QStringLiteral("|A-B| x%1").arg(c.diffGain);
            draw(QPoint(r.right() - 8, y), t, true, m_player->shown(0).hold || m_player->shown(1).hold);
        }
    }
}

void HudOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    p.setFont(mono);
    const QFontMetrics fm(mono);
    paintPaneLabels(p, fm);
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
