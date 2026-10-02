#include "ui/OpenRawDialog.h"

#include <QComboBox>
#include <QDialogButtonBox>
#include <QDoubleSpinBox>
#include <QFileInfo>
#include <QFormLayout>
#include <QHBoxLayout>
#include <QLabel>
#include <QPushButton>
#include <QSpinBox>

namespace scvp::ui {

QStringList OpenRawDialog::commonFormats()
{
    return {QStringLiteral("yuv420p"),     QStringLiteral("nv12"),        QStringLiteral("nv21"),        QStringLiteral("yuv422p"),
            QStringLiteral("yuv444p"),     QStringLiteral("yuyv422"),     QStringLiteral("uyvy422"),     QStringLiteral("yuv420p10le"),
            QStringLiteral("yuv422p10le"), QStringLiteral("p010le"),      QStringLiteral("p016le"),      QStringLiteral("gray"),
            QStringLiteral("gray10le"),    QStringLiteral("gray16le"),    QStringLiteral("rgb24"),       QStringLiteral("bgr24"),
            QStringLiteral("rgba"),        QStringLiteral("bgra"),        QStringLiteral("gbrp"),        QStringLiteral("rgb48le"),
            QStringLiteral("bayer_rggb8"), QStringLiteral("bayer_bggr8"), QStringLiteral("bayer_grbg8"), QStringLiteral("bayer_gbrg8"),
            QStringLiteral("bayer_rggb16le"), QStringLiteral("bayer_bggr16le"), QStringLiteral("bayer_grbg16le"),
            QStringLiteral("bayer_gbrg16le")};
}

OpenRawDialog::OpenRawDialog(const QString& path, const RawVideoParams& initial, QWidget* parent)
    : QDialog(parent)
    , m_path(path)
    , m_fileSize(QFileInfo(path).size())
    , m_width(new QSpinBox(this))
    , m_height(new QSpinBox(this))
    , m_format(new QComboBox(this))
    , m_fps(new QDoubleSpinBox(this))
    , m_header(new QSpinBox(this))
    , m_summary(new QLabel(this))
    , m_buttons(new QDialogButtonBox(QDialogButtonBox::Ok | QDialogButtonBox::Cancel, this))
{
    setWindowTitle(tr("Open raw video"));
    for (QSpinBox* b : {m_width, m_height}) {
        b->setRange(1, 16384);
        b->setAccelerated(true);
    }
    m_width->setValue(initial.width > 0 ? initial.width : 1920);
    m_height->setValue(initial.height > 0 ? initial.height : 1080);
    m_format->setEditable(true);
    m_format->addItems(commonFormats());
    const QString fmt = initial.format != AV_PIX_FMT_NONE ? QString::fromLatin1(av_get_pix_fmt_name(initial.format)) : QStringLiteral("yuv420p");
    m_format->setCurrentText(fmt);
    m_fps->setRange(0.001, 1000);
    m_fps->setDecimals(3);
    m_fps->setValue(initial.frameRate > 0 ? initial.frameRate : 30.0);
    m_header->setRange(0, 1 << 30);
    m_header->setValue(int(initial.headerBytes));
    m_summary->setWordWrap(true);

    auto* size = new QHBoxLayout;
    size->addWidget(m_width);
    size->addWidget(new QLabel(QStringLiteral("x"), this));
    size->addWidget(m_height);
    auto* form = new QFormLayout;
    form->addRow(tr("File"), new QLabel(QFileInfo(path).fileName(), this));
    form->addRow(tr("Frame size"), size);
    form->addRow(tr("Pixel format"), m_format);
    form->addRow(tr("Frame rate"), m_fps);
    form->addRow(tr("Header bytes"), m_header);
    auto* col = new QVBoxLayout(this);
    col->addLayout(form);
    col->addWidget(m_summary);
    col->addWidget(m_buttons);
    connect(m_buttons, &QDialogButtonBox::accepted, this, &QDialog::accept);
    connect(m_buttons, &QDialogButtonBox::rejected, this, &QDialog::reject);
    for (QSpinBox* b : {m_width, m_height, m_header})
        connect(b, &QSpinBox::valueChanged, this, &OpenRawDialog::update);
    connect(m_format, &QComboBox::currentTextChanged, this, &OpenRawDialog::update);
    update();
}

RawVideoParams OpenRawDialog::params() const
{
    RawVideoParams p;
    p.width = m_width->value();
    p.height = m_height->value();
    p.format = av_get_pix_fmt(m_format->currentText().trimmed().toLatin1().constData());
    p.frameRate = m_fps->value();
    p.headerBytes = m_header->value();
    return p;
}

QString OpenRawDialog::summary() const { return m_summary->text(); }

void OpenRawDialog::update()
{
    const RawVideoParams p = params();
    const int frameSize = p.valid() ? av_image_get_buffer_size(p.format, p.width, p.height, 1) : -1;
    bool ok = frameSize > 0;
    if (!ok) {
        m_summary->setText(tr("Unknown pixel format."));
    } else {
        const qint64 payload = m_fileSize - p.headerBytes;
        const qint64 frames = payload > 0 ? payload / frameSize : 0;
        const qint64 rest = payload > 0 ? payload % frameSize : 0;
        ok = frames > 0;
        QString t = tr("%1 bytes per frame: %2 frames").arg(frameSize).arg(frames);
        if (rest)
            t += tr(" (%1 bytes left over: size or format may be wrong)").arg(rest);
        m_summary->setText(t);
    }
    m_buttons->button(QDialogButtonBox::Ok)->setEnabled(ok);
}

std::optional<RawVideoParams> OpenRawDialog::ask(QWidget* parent, const QString& path, const RawVideoParams& initial)
{
    OpenRawDialog d(path, initial, parent);
    if (d.exec() != QDialog::Accepted)
        return std::nullopt;
    return d.params();
}

} // namespace scvp::ui
