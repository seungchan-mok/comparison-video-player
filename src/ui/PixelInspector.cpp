#include "ui/PixelInspector.h"

#include <QStringList>

namespace scvp::ui {
namespace {

// "r 255 g 7 b 0": each value with its channel letter.
QString labelled(const std::array<int, 3>& v, const char* names)
{
    return QStringLiteral("%1 %2 %3 %4 %5 %6")
        .arg(QLatin1Char(names[0])).arg(v[0]).arg(QLatin1Char(names[1])).arg(v[1]).arg(QLatin1Char(names[2])).arg(v[2]);
}

// Displayed RGB, then the source codes when they say something the RGB does not.
QString video(const char* name, const render::SlotSample& s)
{
    using render::CodeKind;
    QString t = QStringLiteral("%1 (%2,%3)  ").arg(QLatin1String(name)).arg(s.texel.x()).arg(s.texel.y());
    if (s.kind == CodeKind::Gray) {
        t += QStringLiteral("y %1").arg(s.hasCodes ? s.codes[0] : s.rgb[0]);
        return t;
    }
    t += labelled(s.rgb, "rgb");
    if (!s.hasCodes)
        return t;
    if (s.kind == CodeKind::Yuv)
        t += QStringLiteral("  ") + labelled(s.codes, "yuv");
    else if (s.kind == CodeKind::Bayer)
        t += QStringLiteral("  raw %1").arg(s.codes[0]);
    else if (s.codes != s.rgb)  // limited-range RGB: the stored values differ from the displayed ones
        t += QStringLiteral("  (code ") + labelled(s.codes, "rgb") + QLatin1Char(')');
    return t;
}

} // namespace

QString formatPixelSample(const render::PixelSample& s)
{
    if (!s.valid)
        return QString();
    QStringList parts;
    const auto& a = s.videos[0];
    const auto& b = s.videos[1];
    if (a.valid)
        parts << video("A", a);
    if (b.valid)
        parts << video("B", b);
    if (a.valid && b.valid && a.bitDepth == b.bitDepth) {
        const std::array<int, 3> d{a.rgb[0] - b.rgb[0], a.rgb[1] - b.rgb[1], a.rgb[2] - b.rgb[2]};
        parts << (a.gray && b.gray ? QStringLiteral("A-B  y %1").arg(d[0]) : QStringLiteral("A-B  ") + labelled(d, "rgb"));
    }
    return parts.join(QStringLiteral("   |   "));
}

} // namespace scvp::ui
