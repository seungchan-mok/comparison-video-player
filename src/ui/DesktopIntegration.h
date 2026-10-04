#pragma once

#include <QString>
#include <QStringList>

namespace scvp::ui {

// Default-application handling on freedesktop systems (Linux): SCVP is registered by its
// applications-menu entry scvp.desktop (packaging/install.sh), whose MimeType list says what it
// opens; xdg-mime reads and changes the user's defaults.
namespace desktop {

bool supported();              // a freedesktop desktop with xdg-mime
QString installedEntry();      // path of the installed scvp.desktop, "" if not installed
QStringList handledTypes();    // MimeType of the installed entry
// Desktop-file id of the application opening `mimeType` by default ("vlc.desktop"), "" if none.
QString defaultFor(const QString& mimeType);
bool isDefault();              // SCVP opens MP4 files by default
// Makes SCVP the default for every type it handles.
bool setAsDefault(QString* error);
// Readable name of a desktop-file id ("VLC media player").
QString applicationName(const QString& desktopId);

} // namespace desktop
} // namespace scvp::ui
