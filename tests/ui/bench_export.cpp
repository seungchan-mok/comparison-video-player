// Export throughput on this machine's GPU and encoders (not a ctest; run by hand):
//   QT_QPA_PLATFORM=offscreen build/dev/tests/ui/bench_export A.mkv [B.mkv] [--view split|wipe|diff]
//       [--format h264|hevc|ffv1|png] [--encoder NAME] [--frames N] [--scale PERCENT] [--hw gpu|cpu]
//       [--offset N] [--labels] [--info] [--out FILE]
// Prints frames per second and how busy each stage was (the one near 100 % limits the export).

#include "render/OffscreenView.h"
#include "ui/ExportJob.h"

#include <QCoreApplication>
#include <QFontDatabase>
#include <QGuiApplication>
#include <QSignalSpy>
#include <QStringList>
#include <QTemporaryDir>

#include <cstdio>

using namespace scvp;
using namespace scvp::ui;

int main(int argc, char* argv[])
{
    QGuiApplication app(argc, argv);
    ff::installLogCallback(AV_LOG_ERROR);
    QStringList files;
    QString view = QStringLiteral("split"), format = QStringLiteral("hevc"), encoder, hw = QStringLiteral("gpu"), out;
    int frames = 120, scale = 100;
    int64_t offset = 0;
    bool labels = false, info = false;
    const QStringList args = QCoreApplication::arguments().mid(1);
    for (int i = 0; i < args.size(); ++i) {
        const QString& a = args[i];
        auto next = [&] { return i + 1 < args.size() ? args[++i] : QString(); };
        if (a == QLatin1String("--view"))
            view = next();
        else if (a == QLatin1String("--format"))
            format = next();
        else if (a == QLatin1String("--encoder"))
            encoder = next();
        else if (a == QLatin1String("--frames"))
            frames = next().toInt();
        else if (a == QLatin1String("--scale"))
            scale = next().toInt();
        else if (a == QLatin1String("--hw"))
            hw = next();
        else if (a == QLatin1String("--offset"))
            offset = next().toLongLong();
        else if (a == QLatin1String("--labels"))
            labels = true;
        else if (a == QLatin1String("--info"))
            info = true;
        else if (a == QLatin1String("--out"))
            out = next();
        else
            files << a;
    }
    if (files.isEmpty() || files.size() > 2) {
        std::fprintf(stderr, "usage: bench_export A [B] [--view split|wipe|diff] [--format h264|hevc|ffv1|png] ...\n");
        return 2;
    }

    ExportSpec spec;
    for (int slot = 0; slot < files.size(); ++slot) {
        std::string err;
        std::shared_ptr<IVideoSource> s = openVideoSource(files[slot].toStdString(), InputOptions{}, &err);
        if (!s || !s->waitForIndex(600000)) {
            std::fprintf(stderr, "cannot open %s: %s\n", qPrintable(files[slot]), err.c_str());
            return 1;
        }
        const FrameIndexPtr idx = s->index();
        spec.mapper.setSlot(slot, SlotTiming{true, idx->frameCount(), idx->frameRate(), idx});
        spec.sources[size_t(slot)] = s;
    }
    spec.mapper.setOffset(offset);
    spec.hw = HwConfig{hw == QLatin1String("cpu") ? HwPolicy::Off : HwPolicy::GpuFirst};
    spec.first = 0;
    spec.last = std::min<int64_t>(spec.mapper.length(), frames) - 1;
    render::RenderState like;
    like.compare.mode = view == QLatin1String("wipe")   ? render::CompareMode::Wipe
                        : view == QLatin1String("diff") ? render::CompareMode::Difference
                                                        : render::CompareMode::Split;
    const VideoInfo& vi = spec.sources[0]->info();
    spec.view = render::OffscreenView::fullFrameState(like, QSizeF(vi.width, vi.height), true, spec.sources[1] != nullptr, scale / 100.0);
    spec.labels = labels;
    spec.legend = true;
    spec.frameInfo = info;
    spec.scene.open = {true, spec.sources[1] != nullptr};
    spec.scene.names = {QStringLiteral("A"), QStringLiteral("B")};
    spec.scene.dpr = std::max(1.0, vi.height * scale / 100.0 / 1080.0);
    spec.mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    spec.encoder.format = format == QLatin1String("h264")  ? OutputFormat::H264
                          : format == QLatin1String("ffv1") ? OutputFormat::Ffv1
                          : format == QLatin1String("png")  ? OutputFormat::PngSequence
                                                            : OutputFormat::Hevc;
    spec.encoder.encoder = encoder.toStdString();
    spec.encoder.frameRate = av_d2q(spec.mapper.fps(), 1001000);
    QTemporaryDir dir;
    spec.path = (out.isEmpty() ? dir.filePath(QStringLiteral("bench.") + QLatin1String(VideoEncoder::extension(spec.encoder.format))) : out)
                    .toStdString();

    ExportJob job(spec);
    QSignalSpy done(&job, &ExportJob::finished);
    job.start();
    while (done.isEmpty())
        done.wait(1000);
    const ExportStats s = job.stats();
    auto pct = [&s](double busy) { return s.seconds > 0 ? 100.0 * busy / s.seconds : 0.0; };
    std::printf("%s %lld frames %dx%d via %s in %.2f s: %.1f fps; busy decode A %.0f%% B %.0f%%, render %.0f%%, encode %.0f%%%s%s\n",
                done.first().at(0).toBool() ? "ok" : "FAILED", static_cast<long long>(s.frames), s.width, s.height, s.encoder.c_str(),
                s.seconds, s.seconds > 0 ? s.frames / s.seconds : 0.0, pct(s.decode[0]), pct(s.decode[1]), pct(s.render),
                pct(s.encode), done.first().at(0).toBool() ? "" : ": ", qPrintable(done.first().at(1).toString()));
    return done.first().at(0).toBool() ? 0 : 1;
}
