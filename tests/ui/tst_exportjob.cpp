#include "render/OffscreenView.h"
#include "support/GlTestContext.h"
#include "support/TestSupport.h"
#include "ui/ExportJob.h"

#include <QDir>
#include <QFontDatabase>
#include <QSignalSpy>
#include <QTemporaryDir>
#include <QtTest>

using namespace scvp;
using namespace scvp::ui;

namespace {

std::shared_ptr<IVideoSource> openFixture(const QString& name)
{
    const auto fx = test::fixture(name);
    if (!fx)
        return nullptr;
    std::string err;
    std::shared_ptr<IVideoSource> s = openVideoSource(fx->path.toStdString(), InputOptions{}, &err);
    if (!s || !s->waitForIndex(30000))
        return nullptr;
    return s;
}

// What the app would export of these videos at this offset: full frames, FFV1, whole timeline.
ExportSpec makeSpec(std::shared_ptr<IVideoSource> a, std::shared_ptr<IVideoSource> b, int64_t offset,
                    render::CompareSettings c, const QString& path, double scale = 1.0)
{
    ExportSpec spec;
    spec.sources = {a, b};
    spec.hw = HwConfig{HwPolicy::Off};
    for (int slot = 0; slot < 2; ++slot) {
        const auto& s = spec.sources[size_t(slot)];
        if (!s)
            continue;
        const FrameIndexPtr idx = s->index();
        spec.mapper.setSlot(slot, SlotTiming{true, idx->frameCount(), idx->frameRate(), idx});
    }
    spec.mapper.setOffset(offset);
    spec.first = 0;
    spec.last = spec.mapper.length() - 1;
    render::RenderState like;
    like.compare = c;
    const VideoInfo& info = (a ? a : b)->info();
    spec.view = render::OffscreenView::fullFrameState(like, QSizeF(info.width, info.height), a != nullptr, b != nullptr, scale);
    spec.scene.open = {a != nullptr, b != nullptr};
    spec.scene.names = {QStringLiteral("first.mkv"), QStringLiteral("second.mkv")};
    spec.scene.font = QFont();
    spec.mono = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    spec.encoder.format = OutputFormat::Ffv1;
    spec.encoder.frameRate = av_d2q(spec.mapper.fps(), 1001000);
    spec.path = path.toStdString();
    return spec;
}

struct Result {
    bool ok = false;
    QString message;
    std::vector<int64_t> progress;  // encoded() as seen while it ran
};

Result run(ExportJob& job)
{
    Result r;
    QSignalSpy spy(&job, &ExportJob::finished);
    job.start();
    QElapsedTimer t;
    t.start();
    while (spy.isEmpty() && t.elapsed() < 120000) {
        r.progress.push_back(job.encoded());
        spy.wait(20);
    }
    if (spy.isEmpty())
        return r;
    r.ok = spy.first().at(0).toBool();
    r.message = spy.first().at(1).toString();
    return r;
}

std::vector<FramePtr> decodeAll(const QString& path)
{
    std::vector<FramePtr> out;
    std::string err;
    auto src = openVideoSource(path.toStdString(), InputOptions{}, &err);
    if (!src || !src->waitForIndex(30000))
        return out;
    auto session = src->openSession(SessionConfig{std::make_shared<FramePool>("check"), 0, HwConfig{HwPolicy::Off}}, &err);
    if (!session || session->seekTo(src->index()->seekPointFor(0)) != DecodeStatus::Ok)
        return out;
    for (;;) {
        FramePtr f;
        if (session->next(f, DecodeHints{}) != DecodeStatus::Ok)
            break;
        out.push_back(f);
    }
    return out;
}

FramePtr decodeFrame(IVideoSource& src, int32_t n)
{
    std::string err;
    auto session = src.openSession(SessionConfig{std::make_shared<FramePool>("ref"), 0, HwConfig{HwPolicy::Off}}, &err);
    if (!session || session->seekTo(src.index()->seekPointFor(n)) != DecodeStatus::Ok)
        return nullptr;
    for (;;) {
        FramePtr f;
        if (session->next(f, DecodeHints{}) != DecodeStatus::Ok)
            return nullptr;
        if (f->pres >= n)
            return f->pres == n ? f : nullptr;
    }
}

// B, G, R of an exported FFV1 frame (bgr0).
std::array<int, 3> bgr(const AVFrame* f, int x, int y)
{
    const uint8_t* p = f->data[0] + ptrdiff_t(y) * f->linesize[0] + x * 4;
    return {p[0], p[1], p[2]};
}

} // namespace

class TstExportJob : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;

private slots:
    void initTestCase()
    {
        ff::installLogCallback(AV_LOG_QUIET);
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
    }

    // A and its copy without the first three frames, offset -3, as their difference: black
    // wherever both show a frame of their own.
    void alignedDifferenceIsBlack()
    {
        SCVP_REQUIRE_FIXTURES();
        auto a = openFixture(QStringLiteral("ffv1_10bit"));
        auto b = openFixture(QStringLiteral("ffv1_10bit_trim3"));
        QVERIFY(a && b);
        render::CompareSettings c;
        c.mode = render::CompareMode::Difference;
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/diff.mkv");
        const ExportSpec spec = makeSpec(a, b, -3, c, path);
        ExportJob job(spec);
        const Result r = run(job);
        QVERIFY2(r.ok, qPrintable(r.message));
        const std::vector<FramePtr> frames = decodeAll(path);
        QCOMPARE(int64_t(frames.size()), spec.frames());
        int black = 0;
        for (int64_t pos = spec.first; pos <= spec.last; ++pos) {
            const bool holds = spec.mapper.local(0, pos).hold || spec.mapper.local(1, pos).hold;
            const AVFrame* f = frames[size_t(pos)]->av;
            QCOMPARE(QSize(f->width, f->height), spec.view.framebufferSize);
            bool allBlack = true;
            for (int y = 0; y < f->height && allBlack; ++y)
                for (int x = 0; x < f->width && allBlack; ++x)
                    allBlack = bgr(f, x, y) == (std::array<int, 3>{0, 0, 0});
            if (!holds) {
                QVERIFY2(allBlack, qPrintable(QStringLiteral("position %1").arg(pos)));
                ++black;
            }
        }
        QCOMPARE(black, 117);
    }

    // Split, with B starting five frames later: every exported picture is what the renderer
    // draws for that position's pair of frames (decoded on their own here), held ends included.
    void splitIsTheRenderedPair_data()
    {
        QTest::addColumn<bool>("hardware");
        QTest::newRow("software decoding") << false;
        QTest::newRow("hardware first") << true;
    }
    void splitIsTheRenderedPair()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(bool, hardware);
        auto a = openFixture(QStringLiteral("h264_b"));
        auto b = openFixture(QStringLiteral("hevc_opengop"));
        QVERIFY(a && b);
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/split.mkv");
        ExportSpec spec = makeSpec(a, b, -5, render::CompareSettings{}, path);
        if (hardware)
            spec.hw = HwConfig{HwPolicy::GpuFirst};
        spec.first = 2;
        spec.last = 61;
        QCOMPARE(spec.view.framebufferSize, QSize(2 * a->info().width + 2, a->info().height));
        ExportJob job(spec);
        const Result r = run(job);
        QVERIFY2(r.ok, qPrintable(r.message));
        QCOMPARE(job.stats().frames, 60);
        QVERIFY(std::is_sorted(r.progress.begin(), r.progress.end()));
        const std::vector<FramePtr> frames = decodeAll(path);
        QCOMPARE(int(frames.size()), 60);

        render::Renderer renderer;
        QVERIFY(renderer.initialize(m_gl.gl()));
        render::OffscreenView view;
        QVERIFY(view.create(m_gl.gl(), spec.view.framebufferSize, nullptr));
        const QSize size = spec.view.framebufferSize;
        for (int64_t pos : {2, 3, 4, 7, 30, 61}) {
            render::RenderState s = spec.view;
            for (int slot = 0; slot < 2; ++slot)
                s.frames[size_t(slot)] = decodeFrame(*spec.sources[size_t(slot)], spec.mapper.local(slot, pos).frame);
            QVERIFY(s.frames[0] && s.frames[1]);
            std::vector<uint8_t> want;
            QVERIFY(view.render(renderer, s) && view.take(want, true));
            const AVFrame* f = frames[size_t(pos - spec.first)]->av;
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    const uint8_t* p = &want[(size_t(size.height() - 1 - y) * size_t(size.width()) + size_t(x)) * 4];
                    if (bgr(f, x, y) != (std::array<int, 3>{p[0], p[1], p[2]}))
                        QFAIL(qPrintable(QStringLiteral("position %1 pixel %2,%3").arg(pos).arg(x).arg(y)));
                }
        }
        QVERIFY(spec.mapper.local(1, 2).hold);  // B had not started yet there
        view.release();
        renderer.release();
    }

    // The legend and the frame line change their corners of the picture and nothing else.
    void overlayStaysInItsCorners()
    {
        SCVP_REQUIRE_FIXTURES();
        auto a = openFixture(QStringLiteral("h264_b"));
        auto b = openFixture(QStringLiteral("hevc_opengop"));
        QVERIFY(a && b);
        QTemporaryDir dir;
        ExportSpec plain = makeSpec(a, b, 0, render::CompareSettings{}, dir.path() + QStringLiteral("/plain.mkv"));
        plain.last = 9;
        ExportSpec over = plain;
        over.path = (dir.path() + QStringLiteral("/over.mkv")).toStdString();
        over.labels = true;
        over.legend = true;
        over.frameInfo = true;
        for (ExportSpec* s : {&plain, &over}) {
            ExportJob job(*s);
            const Result r = run(job);
            QVERIFY2(r.ok, qPrintable(r.message));
        }
        const auto p = decodeAll(dir.path() + QStringLiteral("/plain.mkv"));
        const auto o = decodeAll(dir.path() + QStringLiteral("/over.mkv"));
        QCOMPARE(p.size(), size_t(10));
        QCOMPARE(o.size(), size_t(10));
        const QSize size = plain.view.framebufferSize;
        const int paneW = a->info().width;
        int top = 0, bottom = 0;
        for (size_t i = 0; i < p.size(); ++i)
            for (int y = 0; y < size.height(); ++y)
                for (int x = 0; x < size.width(); ++x) {
                    if (bgr(p[i]->av, x, y) == bgr(o[i]->av, x, y))
                        continue;
                    const int px = x % (paneW + 2);
                    if (y < 60 && px < paneW - 20)
                        ++top;  // legend entry at the top left of each pane
                    else if (y >= size.height() - 50)
                        ++bottom;  // frame numbers at the bottom left
                    else
                        QFAIL(qPrintable(QStringLiteral("frame %1 pixel %2,%3 changed").arg(i).arg(x).arg(y)));
                }
        QVERIFY(top > 100);
        QVERIFY(bottom > 100);
    }

    // A range as a PNG sequence: one file per position, numbered by position.
    void rangeAsPngSequence()
    {
        SCVP_REQUIRE_FIXTURES();
        auto a = openFixture(QStringLiteral("h264_b"));
        QVERIFY(a);
        QTemporaryDir dir;
        ExportSpec spec = makeSpec(a, nullptr, 0, render::CompareSettings{}, dir.path() + QStringLiteral("/shot.png"));
        spec.encoder.format = OutputFormat::PngSequence;
        spec.first = 10;
        spec.last = 19;
        ExportJob job(spec);
        const Result r = run(job);
        QVERIFY2(r.ok, qPrintable(r.message));
        QStringList want;
        for (int i = 10; i <= 19; ++i)
            want << QStringLiteral("shot_%1.png").arg(i, 6, 10, QLatin1Char('0'));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files, QDir::Name), want);
    }

    // Cancelled (at once, or while it runs): it says so and leaves no file behind.
    void cancelLeavesNothing_data()
    {
        QTest::addColumn<bool>("immediately");
        QTest::newRow("immediately") << true;
        QTest::newRow("while running") << false;
    }
    void cancelLeavesNothing()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(bool, immediately);
        auto a = openFixture(QStringLiteral("h264_b"));
        auto b = openFixture(QStringLiteral("hevc_opengop"));
        QVERIFY(a && b);
        QTemporaryDir dir;
        // Large pictures make it slow enough to be caught running.
        ExportJob job(makeSpec(a, b, 0, render::CompareSettings{}, dir.path() + QStringLiteral("/c.mkv"), 4.0));
        QSignalSpy spy(&job, &ExportJob::finished);
        job.start();
        if (immediately) {
            job.cancel();
        } else {
            QTRY_VERIFY_WITH_TIMEOUT(job.encoded() > 0 || !spy.isEmpty(), 60000);
            if (!spy.isEmpty())
                QSKIP("finished before it could be cancelled");
            job.cancel();
        }
        QVERIFY(spy.wait(60000) || !spy.isEmpty());
        QCOMPARE(spy.first().at(0).toBool(), false);
        QCOMPARE(spy.first().at(1).toString(), QStringLiteral("Cancelled"));
        QVERIFY(!job.running());
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList());
    }

    // Where GL cannot be used from another thread, the GUI thread draws in short slices: same
    // pictures, and the event loop keeps running meanwhile.
    void drawingOnTheGuiThread()
    {
        SCVP_REQUIRE_FIXTURES();
        auto a = openFixture(QStringLiteral("ffv1_10bit"));
        auto b = openFixture(QStringLiteral("ffv1_10bit_trim3"));
        QVERIFY(a && b);
        render::CompareSettings c;
        c.mode = render::CompareMode::Difference;
        QTemporaryDir dir;
        const QString path = dir.path() + QStringLiteral("/gui.mkv");
        ExportSpec spec = makeSpec(a, b, -3, c, path);
        spec.first = 3;
        qputenv("SCVP_EXPORT_GUI_RENDER", "1");
        ExportJob job(spec);
        int ticks = 0;
        QTimer timer;
        connect(&timer, &QTimer::timeout, this, [&] { ++ticks; });
        timer.start(1);
        const Result r = run(job);
        qunsetenv("SCVP_EXPORT_GUI_RENDER");
        QVERIFY2(r.ok, qPrintable(r.message));
        QVERIFY(ticks > 0);
        const std::vector<FramePtr> frames = decodeAll(path);
        QCOMPARE(int(frames.size()), 117);
        for (const FramePtr& f : frames)
            for (int y = 0; y < f->height(); y += 7)
                for (int x = 0; x < f->width(); x += 5)
                    QCOMPARE(bgr(f->av, x, y), (std::array<int, 3>{0, 0, 0}));
    }

    // An encoder that cannot take the picture fails the export with its reason.
    void encoderErrorsAreReported()
    {
        SCVP_REQUIRE_FIXTURES();
        auto a = openFixture(QStringLiteral("h264_b"));
        QVERIFY(a);
        QTemporaryDir dir;
        ExportSpec spec = makeSpec(a, nullptr, 0, render::CompareSettings{}, dir.path() + QStringLiteral("/e.mkv"));
        spec.encoder.encoder = "no_such_encoder";
        ExportJob job(spec);
        const Result r = run(job);
        QVERIFY(!r.ok);
        QVERIFY2(r.message.contains(QLatin1String("no_such_encoder")), qPrintable(r.message));
        QCOMPARE(QDir(dir.path()).entryList(QDir::Files), QStringList());
    }
};

QTEST_MAIN(TstExportJob)
#include "tst_exportjob.moc"
