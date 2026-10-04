#pragma once

#include "engine/Demux.h"

#include <QDialog>

#include <optional>

class QComboBox;
class QDialogButtonBox;
class QDoubleSpinBox;
class QLabel;
class QSpinBox;

namespace scvp::ui {

// Asks for the geometry of a headerless raw video file: size, pixel format, frame rate and a
// header to skip, prefilled from the file name, with a live check that the file holds whole
// frames of that size.
class OpenRawDialog : public QDialog {
    Q_OBJECT
public:
    OpenRawDialog(const QString& path, const RawVideoParams& initial, QWidget* parent = nullptr);

    RawVideoParams params() const;
    QString summary() const;  // the frame count line (tests)
    static std::optional<RawVideoParams> ask(QWidget* parent, const QString& path, const RawVideoParams& initial);
    // Pixel formats offered in the list (any FFmpeg name can be typed).
    static QStringList commonFormats();

private:
    void update();

    QString m_path;
    qint64 m_fileSize = 0;
    QSpinBox* m_width;
    QSpinBox* m_height;
    QComboBox* m_format;
    QDoubleSpinBox* m_fps;
    QSpinBox* m_header;
    QLabel* m_summary;
    QDialogButtonBox* m_buttons;
};

} // namespace scvp::ui
