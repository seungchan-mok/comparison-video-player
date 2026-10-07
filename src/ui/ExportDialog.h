#pragma once

#include "engine/VideoEncoder.h"

#include <QDialog>
#include <QSize>
#include <QString>

#include <functional>
#include <optional>
#include <thread>

class QCheckBox;
class QComboBox;
class QDialogButtonBox;
class QLabel;
class QLineEdit;
class QSpinBox;
class QTimer;

namespace scvp::ui {

enum class ExportPicture {
    FullFrames,  // the comparison mode with the videos 1:1, no zoom or pan
    Screen,      // the view as on screen (zoom, pan, pixel values), drawn as large as a copy
};

// What the user chose for an export.
struct ExportOptions {
    OutputFormat format = OutputFormat::H264;
    QString encoder;  // "" = automatic
    OutputQuality quality = OutputQuality::High;
    ExportPicture picture = ExportPicture::FullFrames;
    int scale = 100;  // percent of the picture's natural size
    qint64 first = 0;
    qint64 last = -1;  // inclusive; below first: to the end of the timeline
    bool labels = true;      // the video names (legend, or the short A/B labels)
    bool frameInfo = false;  // frame numbers and time
    QString path;

    // The choices remembered between exports (QSettings "export/..."; not the range or file).
    static ExportOptions load();
    void save() const;
    QString lastDir() const;
};

// The timeline as the dialog shows it; asked again while the videos are still being indexed.
struct ExportTimeline {
    qint64 length = 0;
    bool final = true;           // every open video is indexed (the export needs it)
    double indexProgress = 1.0;
    bool hasLoop = false;
    qint64 loopFirst = 0;
    qint64 loopLast = -1;
    double fps = 25.0;
};

// Asks what to export: format and encoder, frames, picture and overlay, and the file; shows
// the resulting size and length live and enables Export only for a valid choice.
class ExportDialog : public QDialog {
    Q_OBJECT
public:
    using TimelineFn = std::function<ExportTimeline()>;
    using SizeFn = std::function<QSize(const ExportOptions&)>;  // empty: cannot be exported

    ExportDialog(const ExportOptions& initial, TimelineFn timeline, SizeFn size, QWidget* parent = nullptr);
    ~ExportDialog() override;

    ExportOptions options() const;
    QString summary() const;  // the size/length line (tests)
    bool canExport() const;
    static std::optional<ExportOptions> ask(QWidget* parent, const ExportOptions& initial, TimelineFn timeline, SizeFn size);
    static QString formatName(OutputFormat f);

protected:
    void accept() override;

private:
    void update();
    void formatChanged();
    void rangeChanged();
    void fillEncoders();
    void browse();

    TimelineFn m_timelineFn;
    SizeFn m_sizeFn;
    ExportTimeline m_timeline;
    QString m_initialEncoder;
    QComboBox* m_format;
    QComboBox* m_encoder;
    QComboBox* m_quality;
    QComboBox* m_range;
    QSpinBox* m_first;
    QSpinBox* m_last;
    QComboBox* m_picture;
    QComboBox* m_scale;
    QCheckBox* m_labels;
    QCheckBox* m_frameInfo;
    QLineEdit* m_path;
    QLabel* m_summary;
    QDialogButtonBox* m_buttons;
    QTimer* m_indexPoll;
    QString m_confirmed;  // a path the file dialog already asked to replace
    std::thread m_probe;  // finds the usable encoders (opening hardware ones takes a moment)
    bool m_probed = false;
};

} // namespace scvp::ui
