#pragma once

#include "engine/HwDevice.h"
#include "engine/PlaybackController.h"
#include "ui/KeyMap.h"

#include <QByteArray>
#include <QString>

#include <cstddef>

namespace scvp::ui {

// Persistent user settings (QSettings: ~/.config/scvp/scvp.conf, the registry, or a plist).
struct AppSettings {
    size_t cacheMb = 0;  // 0 = automatic (25 % of physical memory)
    QString hwdec = QStringLiteral("gpu");  // "gpu" (GPU first), "cpu", or a device name ("cuda", ...)
    JumpSettings jumps;
    KeyMap keys = KeyMap::defaults();
    QString lastDir;
    QByteArray geometry;

    static AppSettings load();
    void save() const;
    size_t cacheBytes() const;  // resolved budget
    HwConfig hwConfig() const;
    static HwConfig parseHwdec(const QString& value, bool* ok = nullptr);
};

} // namespace scvp::ui
