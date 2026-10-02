#include "ui/Settings.h"

#include "engine/Engine.h"

#include <QSettings>

namespace scvp::ui {

AppSettings AppSettings::load()
{
    AppSettings a;
    QSettings s;
    a.cacheMb = size_t(s.value(QStringLiteral("cache/ram_mb"), 0).toULongLong());
    a.jumps.small = s.value(QStringLiteral("jump/small"), a.jumps.small).toDouble();
    a.jumps.large = s.value(QStringLiteral("jump/large"), a.jumps.large).toDouble();
    a.jumps.inFrames = s.value(QStringLiteral("jump/in_frames"), a.jumps.inFrames).toBool();
    a.keys.load(s);
    a.lastDir = s.value(QStringLiteral("ui/last_dir")).toString();
    a.geometry = s.value(QStringLiteral("ui/geometry")).toByteArray();
    return a;
}

void AppSettings::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("cache/ram_mb"), qulonglong(cacheMb));
    s.setValue(QStringLiteral("jump/small"), jumps.small);
    s.setValue(QStringLiteral("jump/large"), jumps.large);
    s.setValue(QStringLiteral("jump/in_frames"), jumps.inFrames);
    keys.save(s);
    s.setValue(QStringLiteral("ui/last_dir"), lastDir);
    s.setValue(QStringLiteral("ui/geometry"), geometry);
}

size_t AppSettings::cacheBytes() const { return cacheMb ? cacheMb << 20 : Engine::defaultMemoryBudget(); }

} // namespace scvp::ui
