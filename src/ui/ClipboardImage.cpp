#include "ui/ClipboardImage.h"

#include <QBuffer>
#include <QClipboard>
#include <QGuiApplication>
#include <QImageWriter>
#include <QMimeData>

namespace scvp::ui {
namespace {

// Qt maps PNG quality 80 to zlib level 1: about twice as fast as the default level for ~30 %
// more bytes, still lossless.
constexpr int kPngQuality = 80;

} // namespace

ClipboardImage::ClipboardImage(QObject* parent)
    : QObject(parent)
{
    m_pool.setMaxThreadCount(1);
}

ClipboardImage::~ClipboardImage()
{
    m_generation = -1;  // a queued encode need not start
    m_pool.waitForDone();
}

void ClipboardImage::copy(const QImage& image)
{
    const int generation = ++m_generation;
    m_pool.start([this, image, generation] {
        if (generation != m_generation)
            return;  // a newer image came before this one started
        QByteArray png;
        QBuffer buffer(&png);
        buffer.open(QIODevice::WriteOnly);
        QImageWriter writer(&buffer, "png");
        writer.setQuality(kPngQuality);
        if (!writer.write(image))
            png.clear();  // the image alone then: Qt encodes when asked
        // Runs on the GUI thread; the destructor waits for this job, and posted calls die with
        // the object, so `this` is valid there.
        QMetaObject::invokeMethod(this, [this, image, png, generation] { finish(image, png, generation); }, Qt::QueuedConnection);
    });
}

void ClipboardImage::finish(const QImage& image, const QByteArray& png, int generation)
{
    if (generation != m_generation)
        return;
    auto* data = new QMimeData;
    data->setImageData(image);
    if (!png.isEmpty())
        data->setData(QStringLiteral("image/png"), png);
    QGuiApplication::clipboard()->setMimeData(data);
    emit copied(image.size());
}

} // namespace scvp::ui
