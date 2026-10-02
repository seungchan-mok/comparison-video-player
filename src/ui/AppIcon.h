#pragma once

#include <QIcon>
#include <QImage>
#include <QString>

namespace scvp::ui {

// The application icon, drawn in code so window, launcher and package icons share one source:
// a dark tile with a split screen (A orange, B teal, a wipe line between them), a pixel grid
// in B and a play mark.
QImage renderAppIcon(int size);
QIcon appIcon();  // needs a QGuiApplication

// Writes the icon as DIR/<n>x<n>/apps/scvp.png for the usual freedesktop (hicolor) sizes.
// Works without a QGuiApplication (packaging); returns false if a file could not be written.
bool exportAppIcons(const QString& hicolorDir);

} // namespace scvp::ui
