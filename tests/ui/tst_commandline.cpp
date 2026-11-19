#include "ui/CommandLine.h"
#include "ui/KeyMap.h"

#include <QCommandLineParser>
#include <QtTest>

using namespace scvp;
using namespace scvp::ui;

namespace {

// Parses `args` (after the program name) as scvp does; *error receives the message on failure.
std::optional<LaunchOptions> parse(const QStringList& args, QString* error = nullptr)
{
    QCommandLineParser parser;
    addLaunchOptions(parser);
    QString err;
    LaunchOptions o;
    const bool ok = parser.parse(QStringList{QStringLiteral("scvp")} + args) ? readLaunchOptions(parser, &o, &err)
                                                                             : (err = parser.errorText(), false);
    if (error)
        *error = err;
    return ok ? std::optional<LaunchOptions>(o) : std::nullopt;
}

QStringList split(const char* args) { return QString::fromUtf8(args).split(QLatin1Char(' '), Qt::SkipEmptyParts); }

} // namespace

class TstCommandLine : public QObject {
    Q_OBJECT

private slots:
    void nothingSetByDefault()
    {
        const auto o = parse({});
        QVERIFY(o);
        QVERIFY(o->files.isEmpty());
        QVERIFY(!o->frame && !o->play && !o->speed && !o->loop && !o->offset && !o->sync);
        QVERIFY(!o->compare && !o->zoom && !o->center && !o->pixelMode);
        QVERIFY(!o->legend && !o->info && !o->fullscreen);
        QVERIFY(o->color[0] == render::ColorOverrides{} && o->color[1] == render::ColorOverrides{});
        QVERIFY(!o->raw[0] && !o->raw[1] && !o->hwdec && !o->cacheMb && !o->vramMb);
    }

    void readsEveryOption()
    {
        const auto o = parse(QStringList{QStringLiteral("a.mp4"), QStringLiteral("b.yuv")} +
                             split("--raw-b 1920x1080:nv12:60:16 --frame 120 --play --speed 2x --loop 100:200 --offset -3 "
                                   "--sync time --view tb --diff-gain 8 --zoom 32 --center 960,540 --pixel-mode codes "
                                   "--legend --info --fullscreen --matrix 709 --matrix-b 601 --range-a full --hwdec cpu "
                                   "--cache-mb 2048 --vram-mb 0") +
                             QStringList{QStringLiteral("--legend-a"), QStringLiteral(" 기준 x264 ")});
        QVERIFY(o);
        QCOMPARE(o->files, (QStringList{QStringLiteral("a.mp4"), QStringLiteral("b.yuv")}));
        QVERIFY(!o->raw[0]);
        QVERIFY(o->raw[1]);
        QCOMPARE(o->raw[1]->width, 1920);
        QCOMPARE(o->raw[1]->height, 1080);
        QCOMPARE(o->raw[1]->format, AV_PIX_FMT_NV12);
        QCOMPARE(o->raw[1]->frameRate, 60.0);
        QCOMPARE(o->raw[1]->headerBytes, int64_t(16));
        QCOMPARE(*o->frame, int64_t(120));
        QVERIFY(o->play);
        QCOMPARE(*o->speed, 2.0);
        QCOMPARE(*o->loop, (std::pair<int64_t, int64_t>(100, 200)));
        QCOMPARE(*o->offset, int64_t(-3));
        QCOMPARE(*o->sync, SyncMode::Timestamp);
        // --view decides the mode; --diff-gain only sets the gain then.
        QCOMPARE(o->compare->mode, render::CompareMode::Split);
        QCOMPARE(o->compare->orientation, render::SplitOrientation::TopBottom);
        QCOMPARE(o->compare->diffGain, 8.0);
        QCOMPARE(*o->zoom, 32.0);
        QCOMPARE(*o->center, QPointF(960, 540));
        QCOMPARE(*o->pixelMode, render::ValueMode::Codes);
        QVERIFY(o->legend && o->info && o->fullscreen);
        QCOMPARE(*o->legendText[0], QStringLiteral("기준 x264"));
        QVERIFY(!o->legendText[1]);
        // -a / -b win over the shared option.
        QCOMPARE(o->color[0].matrix, std::optional<AVColorSpace>(AVCOL_SPC_BT709));
        QCOMPARE(o->color[1].matrix, std::optional<AVColorSpace>(AVCOL_SPC_BT470BG));
        QCOMPARE(o->color[0].range, std::optional<AVColorRange>(AVCOL_RANGE_JPEG));
        QVERIFY(!o->color[1].range);
        QCOMPARE(o->hwdec->policy, HwPolicy::Off);
        QCOMPARE(*o->cacheMb, size_t(2048));
        QCOMPARE(*o->vramMb, size_t(0));
    }

    void viewModes_data()
    {
        QTest::addColumn<QString>("args");
        QTest::addColumn<int>("mode");
        QTest::addColumn<int>("orientation");
        using M = render::CompareMode;
        using O = render::SplitOrientation;
        QTest::newRow("split") << "--view split" << int(M::Split) << int(O::Auto);
        QTest::newRow("sbs") << "--view sbs" << int(M::Split) << int(O::SideBySide);
        QTest::newRow("wipe") << "--view wipe" << int(M::Wipe) << int(O::Auto);
        QTest::newRow("flip") << "--view FLIP" << int(M::Flip) << int(O::Auto);
        QTest::newRow("diff") << "--view diff" << int(M::Difference) << int(O::Auto);
        QTest::newRow("--wipe selects wipe") << "--wipe 0.3" << int(M::Wipe) << int(O::Auto);
        QTest::newRow("--diff-gain selects diff") << "--diff-gain 4" << int(M::Difference) << int(O::Auto);
        QTest::newRow("--view wins over --wipe") << "--wipe 0.3 --view flip" << int(M::Flip) << int(O::Auto);
        QTest::newRow("--view with both") << "--view diff --wipe 0.3 --diff-gain 4" << int(M::Difference) << int(O::Auto);
    }

    void viewModes()
    {
        QFETCH(QString, args);
        QFETCH(int, mode);
        QFETCH(int, orientation);
        QString err;
        const auto o = parse(args.split(QLatin1Char(' ')), &err);
        QVERIFY2(o, qPrintable(err));
        QVERIFY(o->compare);
        QCOMPARE(int(o->compare->mode), mode);
        QCOMPARE(int(o->compare->orientation), orientation);
        QVERIFY(!o->compare->flipShowsB);  // flip starts on A
    }

    void zoomForms()
    {
        QCOMPARE(*parse(split("--zoom fit"))->zoom, 0.0);
        QCOMPARE(*parse(split("--zoom 1:1"))->zoom, 1.0);
        QCOMPARE(*parse(split("--zoom 0.25"))->zoom, 0.25);
        // --center alone shows that pixel 1:1.
        const auto c = parse(split("--center 10.5,7"));
        QCOMPARE(*c->zoom, 1.0);
        QCOMPARE(*c->center, QPointF(10.5, 7));
    }

    void rawSpec()
    {
        const auto bayer = parseRawSpec(QStringLiteral("3840X2160:bayer_rggb16le"));
        QVERIFY(bayer);
        QCOMPARE(bayer->format, AV_PIX_FMT_BAYER_RGGB16LE);
        QCOMPARE(bayer->frameRate, 0.0);  // the source's default (30)
        const auto both = parse(split("--raw 640x480:yuv420p --raw-a 320x240:uyvy422"));
        QVERIFY(both);
        QCOMPARE(both->raw[0]->width, 320);  // --raw-a wins for A
        QCOMPARE(both->raw[1]->width, 640);
        for (const char* bad : {"1920x1080", "1920:nv12", "0x1080:nv12", "1920x1080:nv13", "1920x1080:nv12:0", "1920x1080:nv12:30:-1",
                                "1920x1080:nv12:30:0:9", "axb:nv12"})
            QVERIFY2(!parseRawSpec(QLatin1String(bad)), bad);
    }

    // A bad value stops the start with a message naming the option and what it takes.
    void badValues_data()
    {
        QTest::addColumn<QString>("args");
        QTest::addColumn<QString>("message");
        QTest::newRow("view") << "--view nope" << "--view expects split, sbs, tb, wipe, flip or diff, not 'nope'";
        QTest::newRow("frame") << "--frame -1" << "--frame expects";
        QTest::newRow("frame text") << "--frame ten" << "--frame expects";
        QTest::newRow("speed") << "--speed 32" << "--speed expects";
        QTest::newRow("loop order") << "--loop 20:10" << "--loop expects";
        QTest::newRow("loop form") << "--loop 10" << "--loop expects";
        QTest::newRow("offset") << "--offset 1.5" << "--offset expects";
        QTest::newRow("sync") << "--sync pts" << "--sync expects frame or time";
        QTest::newRow("wipe") << "--wipe 1.5" << "--wipe expects";
        QTest::newRow("gain") << "--diff-gain 0.5" << "--diff-gain expects";
        QTest::newRow("wipe and gain") << "--wipe 0.3 --diff-gain 4" << "choose one with --view";
        QTest::newRow("zoom") << "--zoom 1000" << "--zoom expects";
        QTest::newRow("center") << "--center 10" << "--center expects";
        QTest::newRow("center at fit") << "--zoom fit --center 1,2" << "--center needs a zoom scale";
        QTest::newRow("pixel mode") << "--pixel-mode yuv" << "--pixel-mode expects rgb or codes";
        QTest::newRow("matrix") << "--matrix-b 470" << "--matrix-b expects auto, 601, 709 or 2020";
        QTest::newRow("range") << "--range tv2" << "--range expects";
        QTest::newRow("raw") << "--raw-a 1x1" << "--raw-a expects";
        QTest::newRow("hwdec") << "--hwdec nvidia" << "--hwdec expects";
        QTest::newRow("cache") << "--cache-mb 0" << "--cache-mb expects";
        QTest::newRow("vram") << "--vram-mb lots" << "--vram-mb expects";
        QTest::newRow("three files") << "a b c" << "at most two videos";
        QTest::newRow("unknown") << "--bogus" << "bogus";
    }

    void badValues()
    {
        QFETCH(QString, args);
        QFETCH(QString, message);
        QString err;
        QVERIFY(!parse(args.split(QLatin1Char(' ')), &err));
        QVERIFY2(err.contains(message), qPrintable(err));
    }

    // Every option is in the help, and the keys are the ones bound now.
    void helpDescribesEverything()
    {
        KeyMap keys = KeyMap::defaults();
        keys.setKeys(Action::ViewWipe, {QKeyCombination(Qt::ControlModifier, Qt::Key_K)});
        const QString help = launchHelp(keys);
        QVERIFY(launchOptionNames().size() > 20);
        for (const QString& name : launchOptionNames())
            QVERIFY2(help.contains(QStringLiteral("--") + name + QLatin1Char(' ')) ||
                         help.contains(QStringLiteral("--") + name + QLatin1Char('\n')),
                     qPrintable(name));
        for (const char* section : {"Usage: scvp", "Files:", "Position and playback:", "Comparison view", "View:", "Color", "Decoding:",
                                    "Notes:", "Examples:", "Keys", "Mouse:"})
            QVERIFY2(help.contains(QLatin1String(section)), section);
        const QString wipeLine = help.split(QLatin1Char('\n')).filter(QRegularExpression(QStringLiteral("^  Wipe "))).value(0);
        QVERIFY2(wipeLine.endsWith(QKeySequence(Qt::ControlModifier | Qt::Key_K).toString(QKeySequence::NativeText)), qPrintable(wipeLine));
        for (const QString& line : help.split(QLatin1Char('\n')))
            QVERIFY2(line.size() <= 80 || line.startsWith(QLatin1String("  scvp ")), qPrintable(line));  // examples may run long
    }

    // The examples in the help are valid command lines.
    void helpExamplesParse()
    {
        const QStringList lines = launchHelp(KeyMap::defaults()).split(QLatin1Char('\n'));
        int examples = 0;
        for (const QString& line : lines) {
            if (!line.startsWith(QLatin1String("  scvp ")))
                continue;
            // Quoted words stay together, as a shell keeps them.
            QStringList args;
            const QRegularExpression word(QStringLiteral("\"([^\"]*)\"|(\\S+)"));
            for (auto it = word.globalMatch(line.mid(7)); it.hasNext();) {
                const QRegularExpressionMatch m = it.next();
                args << (m.capturedStart(1) >= 0 ? m.captured(1) : m.captured(2));
            }
            QString err;
            QVERIFY2(parse(args, &err), qPrintable(line + QStringLiteral(": ") + err));
            ++examples;
        }
        QVERIFY(examples >= 5);
    }

    // --version and Help > About: SCVP's version as the application has it, Qt's, FFmpeg's.
    void versionTextNamesEachPart()
    {
        QCoreApplication::setApplicationVersion(QStringLiteral("9.8.7"));
        const QStringList lines = versionText().trimmed().split(QLatin1Char('\n'));
        QCOMPARE(lines.size(), 3);
        QCOMPARE(lines[0], QStringLiteral("scvp 9.8.7"));
        QCOMPARE(lines[1], QStringLiteral("Qt ") + QLatin1String(qVersion()));
        QVERIFY2(lines[2].startsWith(QLatin1String("FFmpeg ")) && lines[2].contains(QLatin1String("avcodec ")), qPrintable(lines[2]));
    }
};

QTEST_GUILESS_MAIN(TstCommandLine)
#include "tst_commandline.moc"
