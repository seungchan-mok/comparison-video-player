#include "ui/OverlayPainter.h"

#include <QCoreApplication>
#include <QFontMetrics>
#include <QList>
#include <QPainter>

#include <algorithm>

namespace scvp::ui::overlay {
namespace {

QString tr(const char* text) { return QCoreApplication::translate("scvp::ui::HudOverlay", text); }

QRect logicalRect(const render::Pane& pane, double dpr)
{
    return QRect(QPoint(int(pane.rect.x() / dpr), int(pane.rect.y() / dpr)),
                 QSize(int(pane.rect.width() / dpr), int(pane.rect.height() / dpr)));
}

} // namespace

QColor slotColor(int slot) { return slot == 0 ? QColor(255, 138, 61) : QColor(41, 182, 197); }

void paintPaneLabels(QPainter& p, const OverlayScene& s, const QFontMetrics& fm, bool infoVisible)
{
    if (!s.open[0] || !s.open[1])
        return;
    const render::CompareSettings& c = s.compare;
    auto label = [&](int slot) {
        QString t = slot == 0 ? QStringLiteral("A") : QStringLiteral("B");
        if (s.hold[size_t(slot)])
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
    const int top = infoVisible ? -1 : 8;  // the info box takes the top-left corner
    for (const render::Pane& pane : s.layout.panes) {
        const QRect r = logicalRect(pane, s.dpr);
        const int y = top < 0 ? r.bottom() - fm.height() - 12 : r.top() + top;
        if (s.layout.panes.size() > 1 || c.mode == render::CompareMode::Flip) {
            draw(QPoint(r.right() - 8, y), label(pane.slot), true, s.hold[size_t(pane.slot)]);
        } else if (c.mode == render::CompareMode::Wipe) {
            const int x = r.left() + int(c.wipe * r.width());
            draw(QPoint(x - 6, y), label(0), true, s.hold[0]);
            draw(QPoint(x + 6, y), label(1), false, s.hold[1]);
        } else {
            const QString t = QStringLiteral("|A-B| x%1").arg(c.diffGain);
            draw(QPoint(r.right() - 8, y), t, true, s.hold[0] || s.hold[1]);
        }
    }
}

void paintLegend(QPainter& p, const OverlayScene& s, const QRect& avoid)
{
    if (!s.open[0] && !s.open[1])
        return;
    QFont nameFont = s.font;
    nameFont.setPointSizeF(nameFont.pointSizeF() * 1.2);
    QFont chipFont = nameFont;
    chipFont.setBold(true);
    const QFontMetrics fm(nameFont);
    const int h = fm.height() + 10;
    const int pad = 6;

    // A legend entry: [chip] name [chip] name ... (one or two videos), right-aligned if asked.
    auto entry = [&](QPoint at, const QList<int>& ids, bool right, const QString& suffix, int maxWidth) {
        QList<QString> names;
        int w = pad;
        const int nameRoom = std::max(60, (maxWidth - int(ids.size()) * (h + 2 * pad)) / std::max<int>(1, int(ids.size())));
        for (int slot : ids) {
            QString n = s.names[size_t(slot)];
            if (s.hold[size_t(slot)])
                n += tr("  HOLD");
            n = fm.elidedText(n, Qt::ElideMiddle, nameRoom);
            names << n;
            w += h - 4 + pad + fm.horizontalAdvance(n) + pad;
        }
        if (!suffix.isEmpty())
            w += fm.horizontalAdvance(suffix) + pad;
        QRect box(right ? at.x() - w : at.x(), at.y(), w, h);
        if (box.intersects(avoid))
            box.moveTop(avoid.bottom() + 8);
        p.setPen(Qt::NoPen);
        p.setBrush(QColor(0, 0, 0, 170));
        p.drawRoundedRect(box, 6, 6);
        int x = box.left() + pad;
        for (int i = 0; i < ids.size(); ++i) {
            const int slot = ids[i];
            const QRect chip(x, box.top() + 2, h - 4, h - 4);
            p.setBrush(slotColor(slot));
            p.drawRoundedRect(chip, 4, 4);
            p.setFont(chipFont);
            p.setPen(QColor(20, 20, 24));
            p.drawText(chip, Qt::AlignCenter, slot == 0 ? QStringLiteral("A") : QStringLiteral("B"));
            x = chip.right() + 1 + pad;
            p.setFont(nameFont);
            p.setPen(s.hold[size_t(slot)] ? QColor(255, 200, 90) : QColor(245, 245, 245));
            p.drawText(QRect(x, box.top(), fm.horizontalAdvance(names[i]) + 1, h), Qt::AlignVCenter | Qt::AlignLeft, names[i]);
            x += fm.horizontalAdvance(names[i]) + pad;
            p.setPen(Qt::NoPen);
        }
        if (!suffix.isEmpty()) {
            p.setFont(nameFont);
            p.setPen(QColor(200, 200, 200));
            p.drawText(QRect(x, box.top(), fm.horizontalAdvance(suffix) + 1, h), Qt::AlignVCenter | Qt::AlignLeft, suffix);
            p.setPen(Qt::NoPen);
        }
    };

    const render::CompareSettings& c = s.compare;
    const bool both = s.open[0] && s.open[1];
    for (const render::Pane& pane : s.layout.panes) {
        const QRect r = logicalRect(pane, s.dpr);
        const int room = r.width() - 20;
        const QPoint topLeft(r.left() + 10, r.top() + 10);
        if (!both || s.layout.panes.size() > 1 || c.mode == render::CompareMode::Flip) {
            entry(topLeft, {pane.slot}, false, QString(), room);
        } else if (c.mode == render::CompareMode::Wipe) {
            entry(topLeft, {0}, false, QString(), room / 2 - 10);
            entry(QPoint(r.right() - 10, r.top() + 10), {1}, true, QString(), room / 2 - 10);
        } else {
            entry(topLeft, {0, 1}, false, QStringLiteral("|A-B| x%1").arg(c.diffGain), room);
        }
    }
}

void paintFrameInfo(QPainter& p, QSize size, const QString& text, const QFont& mono)
{
    if (text.isEmpty())
        return;
    p.setFont(mono);
    const QFontMetrics fm(mono);
    const int w = std::min(size.width() - 16, fm.horizontalAdvance(text) + 16);
    const QRect box(8, size.height() - fm.height() - 16, w, fm.height() + 8);
    p.setPen(Qt::NoPen);
    p.setBrush(QColor(0, 0, 0, 170));
    p.drawRoundedRect(box, 4, 4);
    p.setPen(QColor(235, 235, 235));
    p.drawText(box, Qt::AlignCenter, fm.elidedText(text, Qt::ElideRight, box.width() - 8));
}

} // namespace scvp::ui::overlay
