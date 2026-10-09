#pragma once

#include <QDialog>
#include <QElapsedTimer>
#include <QPointer>

class QLabel;
class QProgressBar;
class QPushButton;
class QTimer;

namespace scvp::ui {

class ExportJob;
struct ExportStats;

// A small window beside the player while an export runs in the background: progress, speed,
// time left and Cancel; when done, the result and a way to the file. Closing it leaves the
// export running (File > Export Comparison Video shows it again).
class ExportProgress : public QDialog {
    Q_OBJECT
public:
    explicit ExportProgress(QWidget* parent = nullptr);

    void start(ExportJob* job, const QString& path);
    void finish(bool ok, const QString& message, const ExportStats& stats);
    QString detail() const;  // the progress line (tests)

private:
    void poll();

    QPointer<ExportJob> m_job;
    QString m_path;
    QLabel* m_title;
    QProgressBar* m_bar;
    QLabel* m_detail;
    QPushButton* m_cancel;
    QPushButton* m_open;
    QPushButton* m_close;
    QTimer* m_timer;
    QElapsedTimer m_clock;
};

} // namespace scvp::ui
