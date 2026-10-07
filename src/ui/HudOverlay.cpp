#include "ui/HudOverlay.h"

#include "ui/Player.h"
#include "ui/VideoView.h"

#include "engine/GpuFrame.h"

#include <QDir>
#include <QEvent>
#include <QFileInfo>
#include <QFontDatabase>
#include <QPainter>

#include <cmath>

namespace scvp::ui {
namespace {

constexpr qint64 kFlashMs = 1000;      // flip indicator: fully visible, then fading out
constexpr qint64 kFlashSolidMs = 550;

} // namespace

HudOverlay::HudOverlay(Player* player, QWidget* parent)
    : QWidget(parent)
    , m_player(player)
{
    setAttribute(Qt::WA_TransparentForMouseEvents);
    setAttribute(Qt::WA_NoSystemBackground);
    setFocusPolicy(Qt::NoFocus);
    m_refresh.setInterval(250);
    connect(&m_refresh, &QTimer::timeout, this, [this] {
        sampleRates();
        update();
    });
    connect(m_player, &Player::positionChanged, this, [this] {
        ++m_positionCount;
        update();
    });
    m_rateClock.start();
    m_flashTimer.setInterval(16);
    connect(&m_flashTimer, &QTimer::timeout, this, [this] {
        if (m_flashClock.elapsed() > kFlashMs) {
            m_flashSlot = -1;
            m_flashTimer.stop();
        }
        update();
    });
    m_messageTimer.setSingleShot(true);
    connect(&m_messageTimer, &QTimer::timeout, this, [this] {
        m_message.clear();
        update();
    });
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

QColor HudOverlay::slotColor(int slot) { return overlay::slotColor(slot); }

void HudOverlay::setLegendVisible(bool on)
{
    m_legend = on;
    update();
}

void HudOverlay::flashSlot(int slot)
{
    m_flashSlot = slot;
    m_flashClock.restart();
    m_flashTimer.start();
    update();
}

void HudOverlay::setLegendText(int slot, const QString& text)
{
    m_legendText[size_t(slot)] = text;
    update();
}

void HudOverlay::showMessage(const QString& text, int ms)
{
    m_message = text;
    m_messageTimer.start(ms);
    update();
}

QString HudOverlay::slotName(int slot) const
{
    const QString& own = m_legendText[size_t(slot)];
    return own.isEmpty() ? fileName(slot) : own;
}

QString HudOverlay::fileName(int slot) const
{
    const QFileInfo fi(QString::fromStdString(m_player->engine().info(slot).path));
    return fi.fileName().isEmpty() ? fi.filePath() : fi.fileName();
}

void HudOverlay::setPixelText(const QString& text)
{
    if (text == m_pixelText)
        return;
    m_pixelText = text;
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

void HudOverlay::sampleRates()
{
    const double dt = m_rateClock.restart() / 1000.0;
    if (dt <= 0)
        return;
    auto smooth = [](double old, double now) { return old <= 0 ? now : old * 0.6 + now * 0.4; };
    m_shownFps = smooth(m_shownFps, double(m_positionCount - m_lastPositionCount) / dt);
    m_lastPositionCount = m_positionCount;
    for (int slot = 0; slot < Engine::kSlots; ++slot) {
        const uint64_t decoded = m_player->slotOpen(slot) ? m_player->engine().stats(slot).framesDecoded : 0;
        const uint64_t d = decoded >= m_lastDecoded[size_t(slot)] ? decoded - m_lastDecoded[size_t(slot)] : 0;
        m_decodeFps[size_t(slot)] = smooth(m_decodeFps[size_t(slot)], double(d) / dt);
        m_lastDecoded[size_t(slot)] = decoded;
    }
    if (auto* view = qobject_cast<const VideoView*>(parentWidget())) {
        m_paintMs = smooth(m_paintMs, view->lastPaintMs());
        const render::RenderTimings& t = view->lastTimings();
        if (t.converted) {  // only paints that brought a new frame to the GPU
            m_uploadMs = smooth(m_uploadMs, t.uploadMs);
            m_convertMs = smooth(m_convertMs, t.convertMs);
        }
    }
}

QString HudOverlay::perfLine() const
{
    QString t = QStringLiteral("perf  shown %1 fps  paint %2 ms (upload %3, convert %4)  decode")
                    .arg(m_shownFps, 0, 'f', 1)
                    .arg(m_paintMs, 0, 'f', 2)
                    .arg(m_uploadMs, 0, 'f', 2)
                    .arg(m_convertMs, 0, 'f', 2);
    for (int slot = 0; slot < Engine::kSlots; ++slot)
        if (m_player->slotOpen(slot))
            t += QStringLiteral(" %1 %2 fps").arg(slot == 0 ? QLatin1Char('A') : QLatin1Char('B')).arg(m_decodeFps[size_t(slot)], 0, 'f', 0);
    return t;
}

QStringList HudOverlay::lines() const
{
    QStringList out;
    if (m_player->slotOpen(0) || m_player->slotOpen(1))
        out << perfLine();
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
        // Full path: two files of the same name (a/out.mp4, b/out.mp4) are told apart.
        const QString path = QDir::toNativeSeparators(QFileInfo(QString::fromStdString(info.path)).absoluteFilePath());
        out << QStringLiteral("%1  %2").arg(slot == 0 ? QStringLiteral("A") : QStringLiteral("B"), path);
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
        if (auto* view = qobject_cast<const VideoView*>(parentWidget()); view && f) {
            const render::ColorParams c = view->colorParams(slot);
            out << QStringLiteral("   color %1 %2, chroma %3")
                       .arg(QString::fromLatin1(render::ColorParams::matrixName(c.matrixUsed)),
                            c.rangeUsed == AVCOL_RANGE_JPEG ? QStringLiteral("full") : QStringLiteral("limited"),
                            c.chromaBilinear ? QStringLiteral("bilinear") : QStringLiteral("nearest"));
        }
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

OverlayScene HudOverlay::scene() const
{
    OverlayScene s;
    if (auto* view = qobject_cast<const VideoView*>(parentWidget())) {
        s.layout = view->layout();
        s.dpr = view->devicePixelRatioF();
        s.compare = view->compare();
    }
    for (int slot = 0; slot < Engine::kSlots; ++slot) {
        s.open[size_t(slot)] = m_player->slotOpen(slot);
        s.hold[size_t(slot)] = m_player->shown(slot).hold;
        s.names[size_t(slot)] = slotName(slot);
    }
    s.font = font();
    return s;
}

// The flip indicator: which video is on screen now, big, fading out.
void HudOverlay::paintFlash(QPainter& p)
{
    if (m_flashSlot < 0 || !m_player->slotOpen(m_flashSlot))
        return;
    const qint64 t = m_flashClock.elapsed();
    if (t > kFlashMs)
        return;
    const double alpha = t < kFlashSolidMs ? 1.0 : 1.0 - double(t - kFlashSolidMs) / double(kFlashMs - kFlashSolidMs);
    const int big = std::clamp(height() / 5, 40, 140);
    QFont letterFont = font();
    letterFont.setPixelSize(int(big * 0.7));
    letterFont.setBold(true);
    QFont nameFont = font();
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.3);
    const QFontMetrics nfm(nameFont);
    const QString name = nfm.elidedText(slotName(m_flashSlot), Qt::ElideMiddle, width() - 80);
    const int w = std::max(big + 40, nfm.horizontalAdvance(name) + 40);
    const int h = big + nfm.height() + 44;
    const QRect box((width() - w) / 2, (height() - h) / 2, w, h);
    p.setOpacity(alpha);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 180));
    p.drawRoundedRect(box, 14, 14);
    const QRect chip(box.center().x() - big / 2, box.top() + 16, big, big);
    p.setBrush(slotColor(m_flashSlot));
    p.drawRoundedRect(chip, big / 6, big / 6);
    p.setFont(letterFont);
    p.setPen(QColor(20, 20, 24));
    p.drawText(chip, Qt::AlignCenter, m_flashSlot == 0 ? QStringLiteral("A") : QStringLiteral("B"));
    p.setFont(nameFont);
    p.setPen(QColor(245, 245, 245));
    p.drawText(QRect(box.left(), chip.bottom() + 8, box.width(), nfm.height() + 8), Qt::AlignCenter, name);
    p.setOpacity(1.0);
}

// Breaks a line that does not fit after a path separator (else a space, else where the width
// runs out); continuations are indented.
QStringList HudOverlay::wrapLine(const QString& line, const QFontMetrics& fm, int maxWidth)
{
    QStringList out;
    QString rest = line;
    const QString indent = QStringLiteral("      ");
    while (fm.horizontalAdvance(rest) > maxWidth) {
        int sep = -1, space = -1;
        for (int i = 1; i < rest.size(); ++i) {
            if (fm.horizontalAdvance(rest.left(i)) > maxWidth)
                break;
            const QChar c = rest.at(i - 1);
            if (c == QLatin1Char('/') || c == QLatin1Char('\\'))
                sep = i;
            else if (c == QLatin1Char(' '))
                space = i;
        }
        int cut = sep > indent.size() ? sep : space;
        if (cut <= indent.size()) {  // no separator: cut where the width runs out
            cut = 1;
            while (cut < rest.size() && fm.horizontalAdvance(rest.left(cut + 1)) <= maxWidth)
                ++cut;
        }
        out << rest.left(cut);
        rest = indent + rest.mid(cut);
    }
    out << rest;
    return out;
}

// Centred above the pixel values line.
void HudOverlay::paintMessage(QPainter& p)
{
    if (m_message.isEmpty())
        return;
    QFont f = font();
    f.setPointSizeF(f.pointSizeF() * 1.1);
    const QFontMetrics fm(f);
    const QString t = fm.elidedText(m_message, Qt::ElideMiddle, std::max(60, width() - 60));
    const int w = fm.horizontalAdvance(t) + 28;
    const int h = fm.height() + 14;
    const int below = m_pixelText.isEmpty() ? 0 : QFontMetrics(QFontDatabase::systemFont(QFontDatabase::FixedFont)).height() + 8;
    const QRect box((width() - w) / 2, height() - below - h - 14, w, h);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 190));
    p.drawRoundedRect(box, 8, 8);
    p.setFont(f);
    p.setPen(QColor(245, 245, 245));
    p.drawText(box, Qt::AlignCenter, t);
}

void HudOverlay::paintEvent(QPaintEvent*)
{
    QPainter p(this);
    paintContent(p, false);
}

void HudOverlay::paintCapture(QPainter& p) { paintContent(p, true); }

void HudOverlay::paintContent(QPainter& p, bool capture)
{
    p.setRenderHint(QPainter::Antialiasing);
    const QFont mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    p.setFont(mono);
    const QFontMetrics fm(mono);
    // The info box first decides its place, so the legend can stay clear of it.
    QStringList infoLines;
    QRect infoBox;
    if (m_info) {
        for (const QString& l : lines())
            infoLines << wrapLine(l, fm, std::max(200, width() - 32));
        int w = 0;
        for (const QString& l : infoLines)
            w = std::max(w, fm.horizontalAdvance(l));
        infoBox = QRect(8, 8, w + 16, fm.lineSpacing() * int(infoLines.size()) + 12);
    }
    if (m_legend) {
        overlay::paintLegend(p, scene(), infoBox);
        p.setFont(mono);
    } else {
        overlay::paintPaneLabels(p, scene(), fm, m_info);
    }
    if (!m_pixelText.isEmpty()) {
        const int h = fm.height() + 8;
        const QRect box(0, height() - h, std::min(width(), fm.horizontalAdvance(m_pixelText) + 16), h);
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(QColor(235, 235, 235));
        p.drawText(box.adjusted(8, 0, 0, 0), Qt::AlignVCenter | Qt::AlignLeft, m_pixelText);
    }
    if (m_info) {
        const QRect box = infoBox;
        p.fillRect(box, QColor(0, 0, 0, 170));
        p.setPen(QColor(235, 235, 235));
        int y = box.top() + 6 + fm.ascent();
        for (const QString& l : infoLines) {
            p.drawText(box.left() + 8, y, l);
            y += fm.lineSpacing();
        }
    }
    if (capture)
        return;
    if (m_showStall) {
        const QString t = tr("Waiting for decoding...");
        const QRect box(width() - fm.horizontalAdvance(t) - 28, 8, fm.horizontalAdvance(t) + 20, fm.height() + 10);
        p.fillRect(box, QColor(160, 60, 20, 200));
        p.setPen(Qt::white);
        p.drawText(box, Qt::AlignCenter, t);
    }
    paintFlash(p);
    paintMessage(p);
}

} // namespace scvp::ui
