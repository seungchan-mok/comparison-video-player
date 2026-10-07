#include "ui/ExportProgress.h"

#include "engine/VideoEncoder.h"
#include "ui/ExportJob.h"

#include <QDesktopServices>
#include <QDir>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QProgressBar>
#include <QPushButton>
#include <QTimer>
#include <QUrl>
#include <QVBoxLayout>

#include <algorithm>
#include <cmath>

namespace scvp::ui {
namespace {

QString clock(double seconds)
{
    const int s = int(std::lround(std::max(0.0, seconds)));
    return QStringLiteral("%1:%2").arg(s / 60).arg(s % 60, 2, 10, QLatin1Char('0'));
}

} // namespace

ExportProgress::ExportProgress(QWidget* parent)
    : QDialog(parent)
    , m_title(new QLabel(this))
    , m_bar(new QProgressBar(this))
    , m_detail(new QLabel(this))
    , m_cancel(new QPushButton(tr("Cancel"), this))
    , m_open(new QPushButton(tr("Open Folder"), this))
    , m_close(new QPushButton(tr("Close"), this))
    , m_timer(new QTimer(this))
{
    setWindowTitle(tr("Export"));
    setModal(false);
    m_title->setWordWrap(true);
    m_title->setTextInteractionFlags(Qt::TextSelectableByMouse);
    m_detail->setObjectName(QStringLiteral("detail"));
    m_bar->setRange(0, 1000);
    auto* buttons = new QHBoxLayout;
    buttons->addStretch(1);
    buttons->addWidget(m_open);
    buttons->addWidget(m_cancel);
    buttons->addWidget(m_close);
    auto* col = new QVBoxLayout(this);
    col->addWidget(m_title);
    col->addWidget(m_bar);
    col->addWidget(m_detail);
    col->addLayout(buttons);
    setMinimumWidth(460);
    m_timer->setInterval(200);
    connect(m_timer, &QTimer::timeout, this, &ExportProgress::poll);
    connect(m_cancel, &QPushButton::clicked, this, [this] {
        if (m_job)
            m_job->cancel();
        m_cancel->setEnabled(false);
        m_detail->setText(tr("Cancelling..."));
    });
    connect(m_open, &QPushButton::clicked, this, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(QFileInfo(m_path).absolutePath()));
    });
    connect(m_close, &QPushButton::clicked, this, &QDialog::hide);
}

void ExportProgress::start(ExportJob* job, const QString& path)
{
    m_job = job;
    m_path = path;
    m_clock.start();
    m_title->setText(tr("Exporting to %1").arg(QDir::toNativeSeparators(path)));
    m_bar->setValue(0);
    m_detail->clear();
    m_cancel->setEnabled(true);
    m_cancel->show();
    m_open->hide();
    m_close->setText(tr("Hide"));
    m_timer->start();
    poll();
    show();
    raise();
}

QString ExportProgress::detail() const { return m_detail->text(); }

void ExportProgress::poll()
{
    if (!m_job)
        return;
    const int64_t total = std::max<int64_t>(1, m_job->total());
    const int64_t done = m_job->encoded();
    const double elapsed = m_clock.elapsed() / 1000.0;
    m_bar->setValue(int(1000 * done / total));
    QString t = tr("%1 / %2 frames").arg(done).arg(total);
    if (done > 0 && elapsed > 0.5) {
        const double fps = done / elapsed;
        t += tr("  ·  %1 fps  ·  %2 left").arg(fps, 0, 'f', 1).arg(clock(double(total - done) / fps));
    }
    m_detail->setText(t);
}

void ExportProgress::finish(bool ok, const QString& message, const ExportStats& stats)
{
    m_timer->stop();
    m_job = nullptr;
    m_cancel->hide();
    m_close->setText(tr("Close"));
    if (ok) {
        m_bar->setValue(1000);
        m_title->setText(tr("Exported to %1").arg(QDir::toNativeSeparators(m_path)));
        const double fps = stats.seconds > 0 ? stats.frames / stats.seconds : 0;
        QString t = tr("%1 frames in %2 (%3 fps), %4").arg(stats.frames).arg(clock(stats.seconds)).arg(fps, 0, 'f', 1)
                        .arg(QString::fromStdString(stats.encoder));
        if (stats.missing)
            t += QLatin1Char('\n') + tr("%1 frames could not be decoded; the frame next to each was used.").arg(stats.missing);
        m_detail->setText(t);
        m_open->show();
    } else {
        m_title->setText(stats.cancelled ? tr("Export cancelled") : tr("Export failed"));
        m_detail->setText(stats.cancelled ? tr("Nothing was written.") : message);
    }
    show();
}

} // namespace scvp::ui
