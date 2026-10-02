#include "engine/FrameAllocator.h"
#include "render/CodeReader.h"
#include "render/Renderer.h"
#include "support/GlTestContext.h"

#include <QtTest>

#include <random>

using namespace scvp;
using namespace scvp::render;

namespace {

FramePtr randomFrame(FramePool& pool, AVPixelFormat fmt, int w, int h, unsigned seed)
{
    auto f = std::make_shared<Frame>();
    f->av = pool.allocFrame(fmt, w, h);
    std::mt19937 rng(seed);
    const AVPixFmtDescriptor* d = av_pix_fmt_desc_get(fmt);
    const int bits = d->comp[0].depth;
    for (int p = 0; p < 4 && f->av->data[p]; ++p) {
        const int ph = p == 0 || p == 3 ? h : AV_CEIL_RSHIFT(h, d->log2_chroma_h);
        for (int y = 0; y < ph; ++y) {
            uint8_t* row = f->av->data[p] + ptrdiff_t(y) * f->av->linesize[p];
            if (bits > 8) {
                auto* r16 = reinterpret_cast<uint16_t*>(row);
                for (int x = 0; x < f->av->linesize[p] / 2; ++x)
                    r16[x] = uint16_t((rng() & ((1u << bits) - 1u)) << (d->comp[0].shift));
            } else {
                for (int x = 0; x < f->av->linesize[p]; ++x)
                    row[x] = uint8_t(rng());
            }
        }
    }
    f->av->color_range = AVCOL_RANGE_MPEG;
    f->av->colorspace = AVCOL_SPC_BT709;
    return f;
}

} // namespace

class TstReadback : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;
    Renderer m_renderer;
    test::RenderTarget m_target;
    FramePool m_pool;

    RenderState state(const FramePtr& a, const FramePtr& b, int vw, int vh)
    {
        m_target.create(m_gl.gl(), vw, vh);
        RenderState s;
        s.frames = {a, b};
        s.framebufferSize = QSize(vw, vh);
        s.compare.mode = CompareMode::Wipe;
        s.view.setImageSize(QSizeF(a->width(), a->height()));
        s.view.setViewportSize(QSizeF(vw, vh));
        return s;
    }

    PixelSample probe(RenderState& s, QPointF pos)
    {
        s.probe = true;
        s.probePos = pos;
        m_renderer.render(s, m_target.fbo);
        m_renderer.finishSample();
        return m_renderer.sample();
    }

    static void verifySlot(const SlotSample& got, const FramePtr& f, QPoint texel)
    {
        QVERIFY(got.valid);
        QCOMPARE(got.texel, texel);
        const PixelLayout layout = *PixelLayout::of(f->format());
        const ColorParams color = ColorParams::resolve(layout, f->av->colorspace, f->av->color_range, f->height());
        const auto codes = readCodes(f->av, layout, texel.x(), texel.y());
        const auto rgb = color.toRgb(codes[0], codes[1], codes[2]);
        QVERIFY(got.hasCodes);
        for (int c = 0; c < 3; ++c) {
            QCOMPARE(got.codes[size_t(c)], int(codes[size_t(c)]));
            QVERIFY2(std::abs(got.rgb[size_t(c)] - rgb[size_t(c)]) <= 1,
                     qPrintable(QStringLiteral("channel %1: %2 vs %3").arg(c).arg(got.rgb[size_t(c)]).arg(rgb[size_t(c)])));
        }
        QCOMPARE(got.bitDepth, layout.bitDepth);
    }

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
        QVERIFY(m_renderer.initialize(m_gl.gl()));
    }

    void cleanupTestCase()
    {
        if (m_gl.gl()) {
            m_target.destroy(m_gl.gl());
            m_renderer.release();
        }
    }

    void readsBothVideosAtTheCursor_data()
    {
        QTest::addColumn<int>("fa");
        QTest::addColumn<int>("fb");
        QTest::newRow("yuv420p / nv12") << int(AV_PIX_FMT_YUV420P) << int(AV_PIX_FMT_NV12);
        QTest::newRow("p010 / yuv422p10") << int(AV_PIX_FMT_P010LE) << int(AV_PIX_FMT_YUV422P10LE);
        QTest::newRow("rgb24 / gray") << int(AV_PIX_FMT_RGB24) << int(AV_PIX_FMT_GRAY8);
    }

    void readsBothVideosAtTheCursor()
    {
        QFETCH(int, fa);
        QFETCH(int, fb);
        const FramePtr a = randomFrame(m_pool, AVPixelFormat(fa), 33, 21, 1);
        const FramePtr b = randomFrame(m_pool, AVPixelFormat(fb), 33, 21, 2);
        RenderState s = state(a, b, 66, 42);
        for (const QPoint p : {QPoint(0, 0), QPoint(32, 20), QPoint(17, 9), QPoint(5, 14)}) {
            const PixelSample got = probe(s, QPointF(p) + QPointF(0.5, 0.5));
            QVERIFY(got.valid);
            verifySlot(got.videos[0], a, p);
            verifySlot(got.videos[1], b, p);
        }
    }

    void otherSizeBUsesTheDisplayMapping()
    {
        const FramePtr a = randomFrame(m_pool, AV_PIX_FMT_YUV420P, 20, 10, 3);
        const FramePtr b = randomFrame(m_pool, AV_PIX_FMT_YUV420P, 40, 20, 4);
        RenderState s = state(a, b, 80, 40);
        const PixelSample got = probe(s, QPointF(7.75, 3.25));
        verifySlot(got.videos[0], a, QPoint(7, 3));
        verifySlot(got.videos[1], b, QPoint(15, 6));
    }

    void outsideTheImageReadsNothing()
    {
        const FramePtr a = randomFrame(m_pool, AV_PIX_FMT_YUV420P, 20, 10, 5);
        RenderState s = state(a, nullptr, 40, 20);
        probe(s, QPointF(3, 3));
        QVERIFY(m_renderer.sample().valid);
        s.probePos = QPointF(-1, 3);
        m_renderer.render(s, m_target.fbo);
        QVERIFY(!m_renderer.samplePending());
        QVERIFY(!m_renderer.sample().valid);
    }

    void readIsAsynchronous()
    {
        const FramePtr a = randomFrame(m_pool, AV_PIX_FMT_YUV420P, 20, 10, 6);
        RenderState s = state(a, nullptr, 40, 20);
        s.probe = true;
        s.probePos = QPointF(2.5, 2.5);
        m_renderer.render(s, m_target.fbo);  // issues the read, never waits for it
        m_gl.gl()->glFinish();
        m_renderer.render(s, m_target.fbo);  // collects it
        QVERIFY(m_renderer.sample().valid);
        QCOMPARE(m_renderer.sample().videos[0].texel, QPoint(2, 2));
    }

    void cursorIsOutlined()
    {
        const FramePtr a = randomFrame(m_pool, AV_PIX_FMT_YUV420P, 4, 4, 7);
        RenderState s = state(a, nullptr, 64, 64);
        s.overlay.values = false;
        s.overlay.grid = false;
        s.probe = true;
        s.probePos = QPointF(1.5, 2.5);
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        const auto inner = m_target.at(px, 16 + 8, 32 + 8);
        const auto edge = m_target.at(px, 16, 32 + 8);
        const auto neighbour = m_target.at(px, 15, 32 + 8);  // last column of the pixel to the left
        QVERIFY(edge != inner);
        QVERIFY(m_target.at(px, 16 + 15, 32 + 15) != inner);
        QVERIFY(neighbour != edge);
    }
};

QTEST_MAIN(TstReadback)
#include "tst_readback.moc"
