#include "engine/FrameIndex.h"
#include "engine/IndexScanner.h"
#include "support/TestSupport.h"

#include <QtTest>

#include <map>

using namespace scvp;

namespace {

IndexPacket pkt(int64_t pts, int64_t dts, bool key, int64_t pos = -1, uint16_t extra = 0)
{
    IndexPacket p;
    p.pts = pts;
    p.dts = dts;
    p.pos = pos;
    p.size = 100;
    p.flags = uint16_t((key ? IndexPacket::Key : 0) | extra);
    return p;
}

// Decode order for an I P B B P B B ... GOP of `gop` frames (pts in frame units), repeated.
std::vector<IndexPacket> ibbpStream(int gops, int gop)
{
    std::vector<IndexPacket> out;
    int64_t dts = -1;
    for (int g = 0; g < gops; ++g) {
        const int base = g * gop;
        out.push_back(pkt(base, dts++, true, int64_t(out.size()) * 1000));
        for (int i = 3; i < gop + 3; i += 3) {
            const int p = std::min(base + i, base + gop - 1);
            out.push_back(pkt(p, dts++, false, int64_t(out.size()) * 1000));
            for (int b = base + i - 2; b < base + i && b < p; ++b)
                out.push_back(pkt(b, dts++, false, int64_t(out.size()) * 1000));
            if (p == base + gop - 1)
                break;
        }
    }
    return out;
}

} // namespace

class TstFrameIndex : public QObject {
    Q_OBJECT

private slots:
    void initTestCase() { ff::installLogCallback(AV_LOG_QUIET); }

    void reorderedStreamMapsToPts()
    {
        FrameIndexBuilder b(AVRational{1, 30}, 30.0);
        const auto stream = ibbpStream(3, 12);
        for (const auto& p : stream)
            b.add(p);
        auto idx = b.build(true);
        QCOMPARE(idx->frameCount(), 36);
        QCOMPARE(idx->packetCount(), 36);
        for (int32_t pres = 0; pres < idx->frameCount(); ++pres) {
            const int32_t dec = idx->decodeFromPres(pres);
            QVERIFY(dec >= 0);
            QCOMPARE(idx->packet(dec).pts, int64_t(pres));
            QCOMPARE(idx->presFromDecode(dec), pres);
        }
        QVERIFY(idx->reorderDepth() >= 2);
        QCOMPARE(int(idx->keyFrames().size()), 3);
        QCOMPARE(idx->gopLengthAround(5), 12);
        QCOMPARE(idx->frameRate(), 30.0);
        QCOMPARE(idx->secondsOf(30), 1.0);
        QCOMPARE(idx->presAtSeconds(1.0), 30);
        QCOMPARE(idx->presAtSeconds(1.01), 30);
    }

    void seekPointIsKeyOfGop()
    {
        FrameIndexBuilder b(AVRational{1, 30}, 30.0);
        for (const auto& p : ibbpStream(3, 12))
            b.add(p);
        auto idx = b.build(true);
        for (int32_t pres = 0; pres < idx->frameCount(); ++pres) {
            const SeekPoint sp = idx->seekPointFor(pres);
            QVERIFY(sp.valid());
            QCOMPARE(sp.pres, pres / 12 * 12);
        }
        const SeekPoint sp = idx->seekPointFor(30);
        QCOMPARE(idx->seekPointBefore(sp).pres, 12);
        QVERIFY(!idx->seekPointBefore(idx->seekPointFor(0)).valid());
        QCOMPARE(idx->decodeIndexForPos(sp.pos), sp.dec);
    }

    void partialSnapshotsAreStable()
    {
        const auto stream = ibbpStream(4, 12);
        FrameIndexBuilder full(AVRational{1, 30}, 30.0);
        for (const auto& p : stream)
            full.add(p);
        auto final = full.build(true);

        FrameIndexBuilder b(AVRational{1, 30}, 30.0);
        int32_t lastCount = 0;
        for (const auto& p : stream) {
            b.add(p);
            auto snap = b.build(false);
            QVERIFY(snap->frameCount() >= lastCount);
            lastCount = snap->frameCount();
            for (int32_t pres = 0; pres < snap->frameCount(); ++pres)
                QCOMPARE(snap->decodeFromPres(pres), final->decodeFromPres(pres));
        }
        QVERIFY(lastCount <= final->frameCount());
    }

    void discardedPacketsAreNotFrames()
    {
        FrameIndexBuilder b(AVRational{1, 30}, 30.0);
        b.add(pkt(-3, -4, true, 0, IndexPacket::Discard));
        b.add(pkt(-1, -3, false, 1, IndexPacket::Discard));
        b.add(pkt(-2, -2, false, 2, IndexPacket::Discard));
        b.add(pkt(1, -1, false, 3));
        b.add(pkt(0, 0, false, 4));
        b.add(pkt(2, 1, false, 5));
        b.add(pkt(3, 2, true, 6));
        b.setFirstOutputPts(0);
        auto idx = b.build(true);
        QCOMPARE(idx->frameCount(), 4);
        QCOMPARE(idx->ptsOf(0), int64_t(0));
        QCOMPARE(idx->firstPts(), int64_t(0));
        // Frames before the second key decode from the discarded pre-roll key.
        QCOMPARE(idx->seekPointFor(0).dec, 0);
        QCOMPARE(idx->seekPointFor(2).dec, 0);
        QCOMPARE(idx->seekPointFor(3).dec, 6);
    }

    void framesBeforeFirstKeyAndLeadingAreHidden()
    {
        // Stream starting mid-GOP: two orphan B/P frames, then an open-GOP I with two leading B's.
        FrameIndexBuilder b(AVRational{1, 30}, 30.0);
        b.add(pkt(5, 5, false));
        b.add(pkt(6, 6, false));
        b.add(pkt(10, 7, true));  // I
        b.add(pkt(8, 8, false));  // leading B (references the missing previous GOP)
        b.add(pkt(9, 9, false));  // leading B
        b.add(pkt(13, 10, false));
        b.add(pkt(11, 11, false));
        b.add(pkt(12, 12, false));
        b.setFirstOutputPts(10);
        auto idx = b.build(true);
        QCOMPARE(idx->frameCount(), 4);
        QCOMPARE(idx->ptsOf(0), int64_t(10));
        QCOMPARE(idx->presFromDecode(0), -1);
        QCOMPARE(idx->presFromDecode(3), -1);
        QCOMPARE(idx->presFromDecode(2), 0);
    }

    void openGopLeadingPicturesSeekToPreviousKey()
    {
        // IDR at 0, CRA at decode 4 shown at pts 6 with RASL pictures pts 4,5 after it.
        FrameIndexBuilder b(AVRational{1, 30}, 30.0);
        b.add(pkt(0, -1, true));
        b.add(pkt(3, 0, false));
        b.add(pkt(1, 1, false));
        b.add(pkt(2, 2, false));
        b.add(pkt(6, 3, true));   // CRA
        b.add(pkt(4, 4, false));  // RASL
        b.add(pkt(5, 5, false));  // RASL
        b.add(pkt(7, 6, false));
        auto idx = b.build(true);
        QCOMPARE(idx->frameCount(), 8);
        QCOMPARE(idx->seekPointFor(4).dec, 0);  // leading picture of the CRA: start at the IDR
        QCOMPARE(idx->seekPointFor(5).dec, 0);
        QCOMPARE(idx->seekPointFor(6).dec, 4);  // the CRA itself
        QCOMPARE(idx->seekPointFor(7).dec, 4);
    }

    void backwardTimestampJumpStartsSegment()
    {
        FrameIndexBuilder b(AVRational{1, 90000}, 30.0);
        for (int i = 0; i < 10; ++i)
            b.add(pkt(900000 + i * 3000, 900000 + i * 3000, i % 5 == 0));
        for (int i = 0; i < 10; ++i)  // timestamps restart near zero
            b.add(pkt(i * 3000, i * 3000, i % 5 == 0));
        auto idx = b.build(true);
        QCOMPARE(idx->frameCount(), 20);
        QCOMPARE(idx->ptsOf(9), int64_t(900000 + 27000));
        QCOMPARE(idx->ptsOf(10), int64_t(0));
        QCOMPARE(idx->packet(idx->decodeFromPres(10)).segment, int16_t(1));
        QCOMPARE(idx->seekPointFor(12).pres, 10);
    }

    void variableFrameRateTiming()
    {
        FrameIndexBuilder b(AVRational{1, 1000}, 30.0);
        int64_t t = 0;
        for (int i = 0; i < 30; ++i) {
            b.add(pkt(t, t, i == 0));
            t += (i == 14) ? 500 : 33;  // one half-second gap
        }
        auto idx = b.build(true);
        QCOMPARE(idx->frameRate(), 30.0);  // 33 ms median snaps to the nominal rate
        QCOMPARE(idx->presAtSeconds(0.47), 14);   // inside the gap: the last shown frame
        QCOMPARE(idx->presAtSeconds(0.962), 15);
        QCOMPARE(idx->estimatePres(330), 10);
    }

    void scanFixtures_data()
    {
        QTest::addColumn<QString>("name");
        for (const auto& fx : test::fixtures({QStringLiteral("container")})) {
            if (fx.name == QLatin1String("h264_b_avi"))  // no pts: needs POC ordering (later milestone)
                continue;
            QTest::newRow(qPrintable(fx.name)) << fx.name;
        }
    }

    void scanFixtures()
    {
        SCVP_REQUIRE_FIXTURES();
        QFETCH(QString, name);
        const auto fx = test::fixture(name);
        std::vector<FrameIndexPtr> snapshots;
        std::string err;
        auto idx = IndexScanner::scan(fx->path.toStdString(), InputOptions{}, &err,
                                      [&](FrameIndexPtr s) { snapshots.push_back(std::move(s)); });
        QVERIFY2(idx, err.c_str());
        QVERIFY(idx->complete());
        QCOMPARE(idx->frameCount(), fx->frames);
        QVERIFY(!snapshots.empty());
        for (const auto& s : snapshots)
            for (int32_t pres = 0; pres < s->frameCount(); ++pres)
                QCOMPARE(s->decodeFromPres(pres), idx->decodeFromPres(pres));
        for (int32_t pres = 0; pres < idx->frameCount(); ++pres) {
            const SeekPoint sp = idx->seekPointFor(pres);
            QVERIFY(sp.valid());
            QVERIFY(idx->packet(sp.dec).isKey());
            QVERIFY(sp.dec <= idx->decodeFromPres(pres));
        }
    }

    void backgroundScannerPublishes()
    {
        SCVP_REQUIRE_FIXTURES();
        const auto fx = test::fixture(QStringLiteral("hevc_opengop"));
        QVERIFY(fx);
        std::mutex m;
        FrameIndexPtr last;
        int count = 0;
        IndexScanner scanner(fx->path.toStdString(), InputOptions{}, [&](FrameIndexPtr s) {
            std::lock_guard<std::mutex> lock(m);
            last = std::move(s);
            ++count;
        });
        scanner.start();
        QTRY_VERIFY_WITH_TIMEOUT(scanner.finished(), 10000);
        scanner.stop();
        std::lock_guard<std::mutex> lock(m);
        QVERIFY(last && last->complete());
        QCOMPARE(last->frameCount(), fx->frames);
        QVERIFY(count >= 1);
        QCOMPARE(scanner.progress(), 1.0);
    }
};

QTEST_GUILESS_MAIN(TstFrameIndex)
#include "tst_frameindex.moc"
