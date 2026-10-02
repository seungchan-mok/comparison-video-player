#include "ui/Settings.h"

#include "engine/Engine.h"

#include <QSettings>

namespace scvp::ui {

AppSettings AppSettings::load()
{
    AppSettings a;
    QSettings s;
    a.cacheMb = size_t(s.value(QStringLiteral("cache/ram_mb"), 0).toULongLong());
    a.hwdec = s.value(QStringLiteral("decode/hwdec"), a.hwdec).toString();
    a.vramMb = size_t(s.value(QStringLiteral("cache/vram_mb"), 0).toULongLong());
    a.jumps.small = s.value(QStringLiteral("jump/small"), a.jumps.small).toDouble();
    a.jumps.large = s.value(QStringLiteral("jump/large"), a.jumps.large).toDouble();
    a.jumps.inFrames = s.value(QStringLiteral("jump/in_frames"), a.jumps.inFrames).toBool();
    a.smoothScaling = s.value(QStringLiteral("view/smooth_scaling"), a.smoothScaling).toBool();
    a.legend = s.value(QStringLiteral("view/legend"), a.legend).toBool();
    a.keys.load(s);
    a.lastDir = s.value(QStringLiteral("ui/last_dir")).toString();
    a.geometry = s.value(QStringLiteral("ui/geometry")).toByteArray();
    return a;
}

void AppSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("cache/ram_mb"), qulonglong(cacheMb));
    s.setValue(QStringLiteral("decode/hwdec"), hwdec);
    s.setValue(QStringLiteral("cache/vram_mb"), qulonglong(vramMb));
    s.setValue(QStringLiteral("jump/small"), jumps.small);
    s.setValue(QStringLiteral("jump/large"), jumps.large);
    s.setValue(QStringLiteral("jump/in_frames"), jumps.inFrames);
    s.setValue(QStringLiteral("view/smooth_scaling"), smoothScaling);
    s.setValue(QStringLiteral("view/legend"), legend);
    keys.save(s);
    s.setValue(QStringLiteral("ui/last_dir"), lastDir);
    s.setValue(QStringLiteral("ui/geometry"), geometry);
}

HwConfig AppSettings::parseHwdec(const QString& value, bool* ok)
{
    HwConfig c;
    if (ok)
        *ok = true;
    const QString v = value.trimmed().toLower();
    if (v.isEmpty() || v == QLatin1String("gpu") || v == QLatin1String("auto"))
        return c;
    if (v == QLatin1String("cpu") || v == QLatin1String("off") || v == QLatin1String("no")) {
        c.policy = HwPolicy::Off;
        return c;
    }
    const AVHWDeviceType t = av_hwdevice_find_type_by_name(v.toUtf8().constData());
    if (t == AV_HWDEVICE_TYPE_NONE) {
        if (ok)
            *ok = false;
        return c;
    }
    c.policy = HwPolicy::Device;
    c.device = t;
    return c;
}

HwConfig AppSettings::hwConfig() const { return parseHwdec(hwdec); }

size_t AppSettings::vramBytes() const { return vramMb ? vramMb << 20 : Engine::defaultGpuMemoryBudget(); }

size_t AppSettings::cacheBytes() const { return cacheMb ? cacheMb << 20 : Engine::defaultMemoryBudget(); }

} // namespace scvp::ui
