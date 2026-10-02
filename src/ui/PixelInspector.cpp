#include "ui/PixelInspector.h"

#include <QStringList>

namespace scvp::ui {
namespace {

QString triple(const std::array<int, 3>& v, bool gray)
{
    return gray ? QString::number(v[0]) : QStringLiteral("%1 %2 %3").arg(v[0]).arg(v[1]).arg(v[2]);
}

QString video(const char* name, const render::SlotSample& s)
{
    QString t = QStringLiteral("%1 (%2,%3) RGB %4").arg(QLatin1String(name)).arg(s.texel.x()).arg(s.texel.y()).arg(triple(s.rgb, s.gray));
    if (s.hasCodes && !s.gray)
        t += QStringLiteral("  code %1").arg(triple(s.codes, false));
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
    if (a.valid && b.valid && a.bitDepth == b.bitDepth)
        parts << QStringLiteral("A-B %1").arg(triple({a.rgb[0] - b.rgb[0], a.rgb[1] - b.rgb[1], a.rgb[2] - b.rgb[2]}, a.gray && b.gray));
    return parts.join(QStringLiteral("   |   "));
}

} // namespace scvp::ui
