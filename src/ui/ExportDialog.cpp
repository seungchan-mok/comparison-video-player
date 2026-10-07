#include "ui/ExportDialog.h"

#include "render/Renderer.h"

#include <QCheckBox>
#include <QComboBox>
#include <QDialogButtonBox>
#include <QDir>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QLineEdit>
#include <QMessageBox>
#include <QPushButton>
#include <QSettings>
#include <QSpinBox>
#include <QStandardItemModel>
#include <QTimer>

#include <algorithm>
#include <cmath>

namespace scvp::ui {
namespace {

const char* formatKey(OutputFormat f)
{
    switch (f) {
    case OutputFormat::H264: return "h264";
    case OutputFormat::Hevc: return "hevc";
    case OutputFormat::Ffv1: return "ffv1";
    case OutputFormat::PngSequence: return "png";
    }
    return "h264";
}

const char* qualityKey(OutputQuality q)
{
    switch (q) {
    case OutputQuality::High: return "high";
    case OutputQuality::Medium: return "medium";
    case OutputQuality::Small: return "small";
    }
    return "high";
}

QString withExtension(const QString& path, OutputFormat f)
{
    const QString ext = QLatin1String(VideoEncoder::extension(f));
    if (path.isEmpty())
        return path;
    const QFileInfo fi(path);
    const QString suffix = fi.suffix().toLower();
    if (suffix == QLatin1String("mp4") || suffix == QLatin1String("mkv") || suffix == QLatin1String("png"))
        return path.left(path.size() - fi.suffix().size()) + ext;
    return path + QLatin1Char('.') + ext;
}

QString duration(double seconds)
{
    if (seconds < 59.95)
        return QStringLiteral("%1 s").arg(seconds, 0, 'f', 1);
    const int s = int(std::lround(seconds));
    return s >= 3600 ? QStringLiteral("%1:%2:%3").arg(s / 3600).arg(s / 60 % 60, 2, 10, QLatin1Char('0')).arg(s % 60, 2, 10, QLatin1Char('0'))
                     : QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

ExportOptions ExportOptions::load()
{
    ExportOptions o;
    QSettings s;
    const QString f = s.value(QStringLiteral("export/format")).toString();
    for (OutputFormat x : {OutputFormat::H264, OutputFormat::Hevc, OutputFormat::Ffv1, OutputFormat::PngSequence})
        if (f == QLatin1String(formatKey(x)))
            o.format = x;
    o.encoder = s.value(QStringLiteral("export/encoder")).toString();
    const QString q = s.value(QStringLiteral("export/quality")).toString();
    for (OutputQuality x : {OutputQuality::High, OutputQuality::Medium, OutputQuality::Small})
        if (q == QLatin1String(qualityKey(x)))
            o.quality = x;
    o.picture = s.value(QStringLiteral("export/picture")).toString() == QLatin1String("screen") ? ExportPicture::Screen
                                                                                                : ExportPicture::FullFrames;
    o.scale = std::clamp(s.value(QStringLiteral("export/scale"), 100).toInt(), 1, 100);
    o.labels = s.value(QStringLiteral("export/labels"), true).toBool();
    o.frameInfo = s.value(QStringLiteral("export/frame_info"), false).toBool();
    return o;
}

void ExportOptions::save() const
{
    QSettings s;
    s.setValue(QStringLiteral("export/format"), QLatin1String(formatKey(format)));
    s.setValue(QStringLiteral("export/encoder"), encoder);
    s.setValue(QStringLiteral("export/quality"), QLatin1String(qualityKey(quality)));
    s.setValue(QStringLiteral("export/picture"), picture == ExportPicture::Screen ? QStringLiteral("screen") : QStringLiteral("full"));
    s.setValue(QStringLiteral("export/scale"), scale);
    s.setValue(QStringLiteral("export/labels"), labels);
    s.setValue(QStringLiteral("export/frame_info"), frameInfo);
    if (!path.isEmpty())
        s.setValue(QStringLiteral("export/dir"), QFileInfo(path).absolutePath());
}

QString ExportOptions::lastDir() const { return QSettings().value(QStringLiteral("export/dir")).toString(); }

QString ExportDialog::formatName(OutputFormat f)
{
    switch (f) {
    case OutputFormat::H264: return tr("H.264 video (MP4)");
    case OutputFormat::Hevc: return tr("HEVC video (MP4)");
    case OutputFormat::Ffv1: return tr("Lossless video, FFV1 (MKV)");
    case OutputFormat::PngSequence: return tr("PNG image sequence");
    }
    return {};
}

ExportDialog::ExportDialog(const ExportOptions& initial, TimelineFn timeline, SizeFn size, QWidget* parent)
    : QDialog(parent)
    , m_timelineFn(std::move(timeline))
    , m_sizeFn(std::move(size))
    , m_initialEncoder(initial.encoder)
    , m_format(new QComboBox(this))
    , m_encoder(new QComboBox(this))
    , m_quality(new QComboBox(this))
    , m_range(new QComboBox(this))
    , m_first(new QSpinBox(this))
    , m_last(new QSpinBox(this))
    , m_picture(new QComboBox(this))
    , m_scale(new QComboBox(this))
    , m_labels(new QCheckBox(tr("Video names"), this))
    , m_frameInfo(new QCheckBox(tr("Frame numbers and time"), this))
    , m_path(new QLineEdit(this))
    , m_summary(new QLabel(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
    , m_indexPoll(new QTimer(this))
{
    setWindowTitle(tr("Export Comparison Video"));
    m_timeline = m_timelineFn();
    m_format->setObjectName(QStringLiteral("format"));
    m_encoder->setObjectName(QStringLiteral("encoder"));
    m_quality->setObjectName(QStringLiteral("quality"));
    m_range->setObjectName(QStringLiteral("range"));
    m_first->setObjectName(QStringLiteral("first"));
    m_last->setObjectName(QStringLiteral("last"));
    m_picture->setObjectName(QStringLiteral("picture"));
    m_scale->setObjectName(QStringLiteral("scale"));
    m_labels->setObjectName(QStringLiteral("labels"));
    m_frameInfo->setObjectName(QStringLiteral("frameInfo"));
    m_path->setObjectName(QStringLiteral("path"));
    m_buttons->button(QDialogButtonBox::Ok)->setText(tr("Export"));

    for (OutputFormat f : {OutputFormat::H264, OutputFormat::Hevc, OutputFormat::Ffv1, OutputFormat::PngSequence})
        m_format->addItem(formatName(f), int(f));
    m_format->setCurrentIndex(m_format->findData(int(initial.format)));
    m_quality->addItem(tr("High"), int(OutputQuality::High));
    m_quality->addItem(tr("Medium"), int(OutputQuality::Medium));
    m_quality->addItem(tr("Small file"), int(OutputQuality::Small));
    m_quality->setCurrentIndex(m_quality->findData(int(initial.quality)));

    m_range->addItem(tr("Whole timeline"), 0);
    m_range->addItem(tr("Loop"), 1);
    m_range->addItem(tr("Frames"), 2);
    if (auto* model = qobject_cast<QStandardItemModel*>(m_range->model()))
        model->item(1)->setEnabled(m_timeline.hasLoop);
    for (QSpinBox* b : {m_first, m_last}) {
        b->setRange(0, std::max<int>(0, int(m_timeline.length) - 1));
        b->setAccelerated(true);
    }
    const bool custom = initial.last >= initial.first && (initial.first > 0 || initial.last < m_timeline.length - 1);
    m_range->setCurrentIndex(custom ? 2 : m_timeline.hasLoop ? 1 : 0);
    if (custom) {
        m_first->setValue(int(initial.first));
        m_last->setValue(int(initial.last));
    }

    m_picture->addItem(tr("Full frames (videos at 1:1)"), int(ExportPicture::FullFrames));
    m_picture->addItem(tr("As on screen (zoom, pan, pixel values)"), int(ExportPicture::Screen));
    m_picture->setCurrentIndex(m_picture->findData(int(initial.picture)));
    for (int s : {100, 75, 50, 25})
        m_scale->addItem(QStringLiteral("%1 %").arg(s), s);
    m_scale->setCurrentIndex(std::max(0, m_scale->findData(initial.scale)));
    m_labels->setChecked(initial.labels);
    m_frameInfo->setChecked(initial.frameInfo);
    m_path->setText(withExtension(initial.path, initial.format));
    m_summary->setWordWrap(true);

    auto* range = new QHBoxLayout;
    range->addWidget(m_range, 1);
    range->addWidget(m_first);
    range->addWidget(new QLabel(QStringLiteral("–"), this));
    range->addWidget(m_last);
    auto* overlay = new QHBoxLayout;
    overlay->addWidget(m_labels);
    overlay->addWidget(m_frameInfo);
    overlay->addStretch(1);
    auto* file = new QHBoxLayout;
    file->addWidget(m_path, 1);
    auto* browseButton = new QPushButton(tr("Browse..."), this);
    file->addWidget(browseButton);
    auto* form = new QFormLayout;
    form->addRow(tr("Format"), m_format);
    form->addRow(tr("Encoder"), m_encoder);
    form->addRow(tr("Quality"), m_quality);
    form->addRow(tr("Range"), range);
    form->addRow(tr("Picture"), m_picture);
    form->addRow(tr("Size"), m_scale);
    form->addRow(tr("Overlay"), overlay);
    form->addRow(tr("File"), file);
    auto* col = new QVBoxLayout(this);
    col->addLayout(form);
    col->addWidget(m_summary);
    col->addWidget(m_buttons);
    setMinimumWidth(560);

    connect(m_buttons, &QDialogButtonBox::accepted, this, &ExportDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    connect(browseButton, &QPushButton::clicked, this, &ExportDialog::browse);
    connect(m_format, &QComboBox::currentIndexChanged, this, &ExportDialog::formatChanged);
    connect(m_range, &QComboBox::currentIndexChanged, this, &ExportDialog::rangeChanged);
    for (QSpinBox* b : {m_first, m_last})
        connect(b, &QSpinBox::valueChanged, this, &ExportDialog::update);
    for (QComboBox* c : {m_encoder, m_quality, m_picture, m_scale})
        connect(c, &QComboBox::currentIndexChanged, this, &ExportDialog::update);
    connect(m_path, &QLineEdit::textChanged, this, &ExportDialog::update);
    // Until every video is indexed the frame numbers are not final: keep asking.
    m_indexPoll->setInterval(250);
    connect(m_indexPoll, &QTimer::timeout, this, [this] {
        const bool wasFinal = m_timeline.final;
        m_timeline = m_timelineFn();
        for (QSpinBox* b : {m_first, m_last})
            b->setMaximum(std::max<int>(0, int(m_timeline.length) - 1));
        if (m_timeline.final && !wasFinal) {
            m_indexPoll->stop();
            rangeChanged();
        }
        update();
    });
    if (!m_timeline.final)
        m_indexPoll->start();

    // Opening hardware encoders to see whether they work takes up to a few hundred ms (once per
    // run): on a thread, the list fills in when it is done.
    fillEncoders();
    m_probe = std::thread([this] {
        for (OutputFormat f : {OutputFormat::H264, OutputFormat::Hevc, OutputFormat::Ffv1, OutputFormat::PngSequence})
            VideoEncoder::available(f);
        QMetaObject::invokeMethod(this, [this] {
            m_probed = true;
            fillEncoders();
        }, Qt::QueuedConnection);
    });
    rangeChanged();
    formatChanged();
}

ExportDialog::~ExportDialog()
{
    if (m_probe.joinable())
        m_probe.join();
}

void ExportDialog::fillEncoders()
{
    const auto format = OutputFormat(m_format->currentData().toInt());
    const QString current = m_encoder->count() ? m_encoder->currentData().toString() : m_initialEncoder;
    const QSignalBlocker block(m_encoder);
    m_encoder->clear();
    if (!m_probed) {
        m_encoder->addItem(tr("Automatic (checking the encoders...)"), QString());
        return;
    }
    const std::vector<EncoderChoice> list = VideoEncoder::available(format);
    m_encoder->addItem(list.empty() ? tr("None in this FFmpeg build")
                                    : tr("Automatic (%1)").arg(QString::fromStdString(list.front().label)),
                       QString());
    for (const EncoderChoice& c : list)
        m_encoder->addItem(QString::fromStdString(c.label), QString::fromStdString(c.name));
    m_encoder->setCurrentIndex(std::max(0, m_encoder->findData(current)));
    m_encoder->setEnabled(list.size() > 1);
    update();
}

void ExportDialog::formatChanged()
{
    const auto format = OutputFormat(m_format->currentData().toInt());
    m_quality->setEnabled(!VideoEncoder::lossless(format));
    if (!m_path->text().isEmpty())
        m_path->setText(withExtension(m_path->text(), format));
    m_confirmed.clear();
    fillEncoders();
    update();
}

void ExportDialog::rangeChanged()
{
    const int mode = m_range->currentData().toInt();
    const qint64 last = std::max<qint64>(0, m_timeline.length - 1);
    if (mode == 0) {
        m_first->setValue(0);
        m_last->setValue(int(last));
    } else if (mode == 1 && m_timeline.hasLoop) {
        m_first->setValue(int(m_timeline.loopFirst));
        m_last->setValue(int(std::min(m_timeline.loopLast, last)));
    }
    m_first->setEnabled(mode == 2);
    m_last->setEnabled(mode == 2);
    update();
}

ExportOptions ExportDialog::options() const
{
    ExportOptions o;
    o.format = OutputFormat(m_format->currentData().toInt());
    o.encoder = m_encoder->currentData().toString();
    o.quality = OutputQuality(m_quality->currentData().toInt());
    o.picture = ExportPicture(m_picture->currentData().toInt());
    o.scale = m_scale->currentData().toInt();
    o.first = m_first->value();
    o.last = m_last->value();
    o.labels = m_labels->isChecked();
    o.frameInfo = m_frameInfo->isChecked();
    o.path = m_path->text().trimmed();
    return o;
}

QString ExportDialog::summary() const { return m_summary->text(); }

bool ExportDialog::canExport() const { return m_buttons->button(QDialogButtonBox::Ok)->isEnabled(); }

void ExportDialog::update()
{
    const ExportOptions o = options();
    const qint64 frames = o.last - o.first + 1;
    const QSize size = m_sizeFn ? m_sizeFn(o) : QSize();
    QStringList parts;
    QString problem;
    if (!m_timeline.final) {
        problem = tr("Indexing the videos (%1 %): the export can start when it is done.").arg(int(m_timeline.indexProgress * 100));
    } else if (m_timeline.length <= 0) {
        problem = tr("Open a video first.");
    } else if (frames <= 0) {
        problem = tr("The last frame comes before the first.");
    } else if (size.isEmpty()) {
        problem = tr("Nothing to draw.");
    } else if (size.width() > render::Renderer::kMaxCaptureSide || size.height() > render::Renderer::kMaxCaptureSide) {
        problem = tr("%1 x %2 is too large (at most %3 pixels a side): choose a smaller size.")
                      .arg(size.width()).arg(size.height()).arg(render::Renderer::kMaxCaptureSide);
    } else if (m_probed && VideoEncoder::available(o.format).empty()) {
        problem = tr("This FFmpeg build has no encoder for this format.");
    } else if (o.path.isEmpty()) {
        problem = tr("Choose a file.");
    }
    if (frames > 0 && m_timeline.length > 0)
        parts << (frames == 1 ? tr("1 frame") : tr("%1 frames").arg(frames));
    if (!size.isEmpty())
        parts << QStringLiteral("%1 x %2").arg(size.width()).arg(size.height());
    if (m_timeline.fps > 0 && frames > 0) {
        parts << tr("%1 fps").arg(m_timeline.fps, 0, 'f', m_timeline.fps == std::floor(m_timeline.fps) ? 0 : 3);
        parts << duration(double(frames) / m_timeline.fps);
    }
    QString text = parts.join(QStringLiteral("  ·  "));
    if (o.encoder == QLatin1String("h264_nvenc") && (size.width() > 4096 || size.height() > 4096))
        text += QLatin1Char('\n') + tr("NVENC H.264 takes at most 4096 pixels a side: choose HEVC, Automatic or a smaller size.");
    if (!problem.isEmpty())
        text += (text.isEmpty() ? QString() : QStringLiteral("\n")) + problem;
    m_summary->setText(text);
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(problem.isEmpty());
}

void ExportDialog::browse()
{
    const auto format = OutputFormat(m_format->currentData().toInt());
    QString filter;
    switch (format) {
    case OutputFormat::H264:
    case OutputFormat::Hevc: filter = tr("MP4 video (*.mp4)"); break;
    case OutputFormat::Ffv1: filter = tr("Matroska video (*.mkv)"); break;
    case OutputFormat::PngSequence: filter = tr("PNG images (*.png)"); break;
    }
    // PNG sequences are numbered files: the name is their stem, nothing to replace yet.
    const QFileDialog::Options opts = format == OutputFormat::PngSequence ? QFileDialog::DontConfirmOverwrite : QFileDialog::Options();
    const QString path = QFileDialog::getSaveFileName(this, tr("Export to"), m_path->text(), filter, nullptr, opts);
    if (path.isEmpty())
        return;
    m_path->setText(withExtension(path, format));
    m_confirmed = format == OutputFormat::PngSequence ? QString() : m_path->text();
}

void ExportDialog::accept()
{
    if (!canExport())
        return;
    const ExportOptions o = options();
    // Replacing a file: ask, unless the file dialog just did.
    QString existing;
    if (o.format == OutputFormat::PngSequence) {
        const QString first = QString::fromStdString(VideoEncoder::sequenceFile(o.path.toStdString(), int(o.first)));
        if (QFileInfo::exists(first))
            existing = first;
    } else if (QFileInfo::exists(o.path) && o.path != m_confirmed) {
        existing = o.path;
    }
    if (!existing.isEmpty() &&
        QMessageBox::question(this, tr("Replace file"), tr("%1 already exists. Replace it?").arg(QDir::toNativeSeparators(existing)),
                              QMessageBox::Yes | QMessageBox::No, QMessageBox::No) != QMessageBox::Yes)
        return;
    QDialog::accept();
}

std::optional<ExportOptions> ExportDialog::ask(QWidget* parent, const ExportOptions& initial, TimelineFn timeline, SizeFn size)
{
    ExportDialog d(initial, std::move(timeline), std::move(size), parent);
    if (d.exec() != QDialog::Accepted)
        return std::nullopt;
    return d.options();
}

} // namespace scvp::ui
