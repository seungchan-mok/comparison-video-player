#pragma once

#include <QImage>
#include <QObject>
#include <QThreadPool>

#include <atomic>

namespace scvp::ui {

// Puts images on the clipboard without stalling the UI. Applications paste PNG, and Qt would
// encode it on the GUI thread each time the clipboard is read (desktop clipboard managers read
// it at once): ~0.5 s for 8 MP. So the PNG is encoded on a worker first, then offered as is,
// together with the image for the other formats (bitmaps on Windows, TIFF on macOS). The PNG
// keeps the image's resolution (dots per metre).
class ClipboardImage : public QObject {
    Q_OBJECT
public:
    explicit ClipboardImage(QObject* parent = nullptr);
    ~ClipboardImage() override;  // waits for an encode in flight

    // The newest image wins: one still encoding when another arrives is dropped.
    void copy(const QImage& image);

signals:
    void copied(QSize size);  // on the clipboard now

private:
    void finish(const QImage& image, const QByteArray& png, int generation);

    QThreadPool m_pool;
    std::atomic<int> m_generation{0};
};

} // namespace scvp::ui
