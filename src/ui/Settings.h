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
    size_t vramMb = 0;   // 0 = automatic (40 % of the CUDA device's memory)
    QString hwdec = QStringLiteral("gpu");  // "gpu" (GPU first), "cpu", or a device name ("cuda", ...)
    JumpSettings jumps;
    bool smoothScaling = true;  // bilinear at fractional zooms such as fit to window
    KeyMap keys = KeyMap::defaults();
    QString lastDir;
    QByteArray geometry;

    static AppSettings load();
    void save() const;
    size_t cacheBytes() const;  // resolved budget
    size_t vramBytes() const;
    HwConfig hwConfig() const;
    static HwConfig parseHwdec(const QString& value, bool* ok = nullptr);
};

} // namespace scvp::ui
