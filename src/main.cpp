#include <QApplication>
#include <QCommandLineParser>
#include <QLabel>
#include <QMainWindow>
#include <cstdio>

extern "C" {
#include <libavcodec/avcodec.h>
#include <libavformat/avformat.h>
#include <libavutil/avutil.h>
#include <libswscale/swscale.h>
}

namespace {

QString ffmpegVersionString()
{
    auto v = [](unsigned int x) {
        return QStringLiteral("%1.%2.%3").arg(x >> 16).arg((x >> 8) & 0xff).arg(x & 0xff);
    };
    return QStringLiteral("FFmpeg %1 (avcodec %2, avformat %3, avutil %4, swscale %5)")
        .arg(QString::fromUtf8(av_version_info()), v(avcodec_version()), v(avformat_version()),
             v(avutil_version()), v(swscale_version()));
}

} // namespace

int main(int argc, char* argv[])
{
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("scvp"));
    QApplication::setApplicationVersion(QStringLiteral(SCVP_VERSION));

    QCommandLineParser parser;
    parser.setApplicationDescription(QStringLiteral("Simple comparison video player"));
    parser.addHelpOption();
    parser.addVersionOption();
    parser.process(app);

    std::fprintf(stderr, "scvp %s, Qt %s, %s\n", SCVP_VERSION, qVersion(),
                 qPrintable(ffmpegVersionString()));

    QMainWindow window;
    window.setWindowTitle(QStringLiteral("SCVP"));
    auto* placeholder = new QLabel(ffmpegVersionString(), &window);
    placeholder->setAlignment(Qt::AlignCenter);
    window.setCentralWidget(placeholder);
    window.resize(1280, 720);
    window.show();
    return QApplication::exec();
}
