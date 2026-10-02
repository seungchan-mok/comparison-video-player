#include "ui/AppIcon.h"

#include <QDir>
#include <QLinearGradient>
#include <QPainter>
#include <QPainterPath>
#include <QPixmap>

#include <algorithm>

namespace scvp::ui {
namespace {

constexpr int kSizes[] = {16, 22, 24, 32, 48, 64, 128, 256, 512};

} // namespace

QImage renderAppIcon(int size)
{
    QImage img(size, size, QImage::Format_ARGB32_Premultiplied);
    img.fill(Qt::transparent);
    QPainter p(&img);
    p.setRenderHint(QPainter::Antialiasing);
    const double s = size;

    // Tile.
    const QRectF tile(s * 0.06, s * 0.06, s * 0.88, s * 0.88);
    QLinearGradient bg(tile.topLeft(), tile.bottomLeft());
    bg.setColorAt(0, QColor(58, 62, 76));
    bg.setColorAt(1, QColor(24, 26, 32));
    p.setPen(Qt::NoPen);
    p.setBrush(bg);
    p.drawRoundedRect(tile, s * 0.2, s * 0.2);

    // Screen split into A (left) and B (right).
    const QRectF scr(s * 0.17, s * 0.25, s * 0.66, s * 0.5);
    QPainterPath screen;
    screen.addRoundedRect(scr, s * 0.05, s * 0.05);
    p.save();
    p.setClipPath(screen);
    const double mid = scr.center().x();
    QLinearGradient a(scr.topLeft(), scr.bottomLeft());
    a.setColorAt(0, QColor(255, 150, 70));
    a.setColorAt(1, QColor(236, 88, 32));
    p.fillRect(QRectF(scr.left(), scr.top(), mid - scr.left(), scr.height()), a);
    QLinearGradient b(scr.topLeft(), scr.bottomLeft());
    b.setColorAt(0, QColor(56, 205, 222));
    b.setColorAt(1, QColor(22, 136, 164));
    p.fillRect(QRectF(mid, scr.top(), scr.right() - mid, scr.height()), b);
    if (size >= 32) {
        // A few magnified pixels in B: a checker of lighter cells in its top-right corner.
        const double cell = scr.height() / 6.0;
        for (int y = 0; y < 3; ++y)
            for (int x = 0; x < 3; ++x)
                if ((x + y) % 2 == 0)
                    p.fillRect(QRectF(scr.right() - cell * (x + 1), scr.top() + cell * y, cell, cell), QColor(255, 255, 255, 46));
    }
    p.restore();

    // Wipe line, a little taller than the screen.
    const double lw = std::max(1.0, s * 0.03);
    p.setBrush(QColor(250, 250, 250));
    p.drawRoundedRect(QRectF(mid - lw / 2, scr.top() - s * 0.06, lw, scr.height() + s * 0.12), lw / 2, lw / 2);

    // Play mark over the middle.
    const double h = scr.height() * 0.46;
    const QPointF c = scr.center() + QPointF(h * 0.12, 0);
    QPainterPath play;
    play.moveTo(c + QPointF(-h * 0.42, -h * 0.5));
    play.lineTo(c + QPointF(h * 0.5, 0));
    play.lineTo(c + QPointF(-h * 0.42, h * 0.5));
    play.closeSubpath();
    p.setBrush(QColor(0, 0, 0, 70));
    p.drawPath(play.translated(0, s * 0.012));
    p.setBrush(QColor(255, 255, 255));
    p.drawPath(play);
    return img;
}

QIcon appIcon()
{
    QIcon icon;
    for (int n : kSizes)
        icon.addPixmap(QPixmap::fromImage(renderAppIcon(n)));
    return icon;
}

bool exportAppIcons(const QString& hicolorDir)
{
    bool ok = true;
    for (int n : kSizes) {
        const QString dir = QStringLiteral("%1/%2x%2/apps").arg(hicolorDir).arg(n);
        ok &= QDir().mkpath(dir);
        ok &= renderAppIcon(n).save(dir + QStringLiteral("/scvp.png"), "PNG");
    }
    return ok;
}

} // namespace scvp::ui
