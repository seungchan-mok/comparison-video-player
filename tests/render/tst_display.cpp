#include "engine/FrameAllocator.h"
#include "render/Renderer.h"
#include "support/GlTestContext.h"

#include <QtTest>

#include <random>

using namespace scvp;
using namespace scvp::render;

namespace {

struct Target {
    GLuint fbo = 0;
    GLuint tex = 0;
    int w = 0;
    int h = 0;

    void create(GL* gl, int width, int height)
    {
        destroy(gl);
        w = width;
        h = height;
        gl->glGenTextures(1, &tex);
        gl->glBindTexture(GL_TEXTURE_2D, tex);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, w, h, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glTexParameteri(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
    }
    void destroy(GL* gl)
    {
        if (fbo)
            gl->glDeleteFramebuffers(1, &fbo);
        if (tex)
            gl->glDeleteTextures(1, &tex);
        fbo = tex = 0;
    }
    // Pixel at device (x, y), top-left origin.
    std::array<int, 3> at(const std::vector<uint8_t>& px, int x, int y) const
    {
        const size_t i = (size_t(h - 1 - y) * size_t(w) + size_t(x)) * 4;
        return {px[i], px[i + 1], px[i + 2]};
    }
    std::vector<uint8_t> read(GL* gl) const
    {
        std::vector<uint8_t> px(size_t(w) * size_t(h) * 4);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
        gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
        gl->glReadPixels(0, 0, w, h, GL_RGBA, GL_UNSIGNED_BYTE, px.data());
        return px;
    }
};

FramePtr randomRgb(FramePool& pool, int w, int h, unsigned seed)
{
    auto f = std::make_shared<Frame>();
    f->av = pool.allocFrame(AV_PIX_FMT_RGB24, w, h);
    std::mt19937 rng(seed);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w * 3; ++x)
            f->av->data[0][size_t(y) * size_t(f->av->linesize[0]) + size_t(x)] = uint8_t(rng());
    return f;
}

std::array<int, 3> src(const AVFrame* f, int x, int y)
{
    const uint8_t* p = f->data[0] + ptrdiff_t(y) * f->linesize[0] + x * 3;
    return {p[0], p[1], p[2]};
}

} // namespace

class TstDisplay : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;
    Renderer m_renderer;
    Target m_target;
    FramePool m_pool;
    FramePtr m_frame;
    FramePtr m_frameB;

    // Two videos in a comparison mode; the view gets the pane size like VideoView does.
    RenderState compareState(int vw, int vh, CompareSettings c, FramePtr b = nullptr)
    {
        m_target.create(m_gl.gl(), vw, vh);
        RenderState s;
        s.frames = {m_frame, b ? b : m_frameB};
        s.framebufferSize = QSize(vw, vh);
        s.background = QColor(10, 20, 30);
        s.divider = QColor(200, 100, 50);
        s.compare = c;
        const QSizeF image(m_frame->width(), m_frame->height());
        s.view.setImageSize(image);
        s.view.setViewportSize(QSizeF(CompareLayout::compute(c, s.framebufferSize, image, true, true).paneSize()));
        return s;
    }

    RenderState stateFor(int vw, int vh)
    {
        m_target.create(m_gl.gl(), vw, vh);
        RenderState s;
        s.frames[0] = m_frame;
        s.framebufferSize = QSize(vw, vh);
        s.background = QColor(10, 20, 30);
        s.view.setViewportSize(QSizeF(vw, vh));
        s.view.setImageSize(QSizeF(m_frame->width(), m_frame->height()));
        return s;
    }

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
        QVERIFY(m_renderer.initialize(m_gl.gl()));
        auto f = std::make_shared<Frame>();
        f->av = m_pool.allocFrame(AV_PIX_FMT_RGB24, 64, 48);
        std::mt19937 rng(5);
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64 * 3; ++x)
                f->av->data[0][size_t(y) * size_t(f->av->linesize[0]) + size_t(x)] = uint8_t(rng());
        m_frame = f;
        m_frameB = randomRgb(m_pool, 64, 48, 77);
    }

    void cleanupTestCase()
    {
        if (m_gl.gl()) {
            m_target.destroy(m_gl.gl());
            m_renderer.release();
        }
    }

    void oneToOneIsExactCopy()
    {
        RenderState s = stateFor(64, 48);
        QCOMPARE(s.view.scale(), 1.0);
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                QCOMPARE(m_target.at(px, x, y), src(m_frame->av, x, y));
    }

    void integerZoomMakesBlocks()
    {
        RenderState s = stateFor(192, 144);
        QCOMPARE(s.view.scale(), 3.0);
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 144; ++y)
            for (int x = 0; x < 192; ++x)
                QCOMPARE(m_target.at(px, x, y), src(m_frame->av, x / 3, y / 3));
    }

    void backgroundOutsideImage()
    {
        RenderState s = stateFor(100, 60);
        QCOMPARE(s.view.renderTranslation(), QPointF(18, 6));
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        QCOMPARE(m_target.at(px, 0, 0), (std::array<int, 3>{10, 20, 30}));
        QCOMPARE(m_target.at(px, 99, 59), (std::array<int, 3>{10, 20, 30}));
        QCOMPARE(m_target.at(px, 18, 6), src(m_frame->av, 0, 0));
        QCOMPARE(m_target.at(px, 18 + 63, 6 + 47), src(m_frame->av, 63, 47));
    }

    void minifiedUsesMipmaps()
    {
        RenderState s = stateFor(32, 24);
        QCOMPARE(s.view.scale(), 0.5);
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        int worst = 0;
        for (int y = 0; y < 24; ++y)
            for (int x = 0; x < 32; ++x) {
                const auto got = m_target.at(px, x, y);
                for (int c = 0; c < 3; ++c) {
                    const int avg = (src(m_frame->av, 2 * x, 2 * y)[size_t(c)] + src(m_frame->av, 2 * x + 1, 2 * y)[size_t(c)] +
                                     src(m_frame->av, 2 * x, 2 * y + 1)[size_t(c)] + src(m_frame->av, 2 * x + 1, 2 * y + 1)[size_t(c)] + 2) / 4;
                    worst = std::max(worst, std::abs(got[size_t(c)] - avg));
                }
            }
        QVERIFY2(worst <= 2, qPrintable(QString::number(worst)));
    }

    void unchangedFrameIsNotUploadedAgain()
    {
        RenderState s = stateFor(64, 48);
        m_renderer.render(s, m_target.fbo);
        m_renderer.render(s, m_target.fbo);
        QVERIFY(!m_renderer.timings().converted);
        auto other = std::make_shared<Frame>();
        other->av = av_frame_clone(m_frame->av);
        s.frames[0] = other;
        m_renderer.render(s, m_target.fbo);
        QVERIFY(m_renderer.timings().converted);
        QCOMPARE(int(m_gl.gl()->glGetError()), int(GL_NO_ERROR));
    }

    void sideBySidePanesArePixelIdentical()
    {
        CompareSettings c;
        c.mode = CompareMode::Split;
        RenderState s = compareState(64 * 2 + CompareLayout::kDivider, 48, c);
        QCOMPARE(s.view.scale(), 1.0);
        m_renderer.render(s, m_target.fbo);
        QCOMPARE(m_renderer.layout().orientation, SplitOrientation::SideBySide);
        const auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                QCOMPARE(m_target.at(px, x, y), src(m_frame->av, x, y));
                QCOMPARE(m_target.at(px, 64 + CompareLayout::kDivider + x, y), src(m_frameB->av, x, y));
            }
        QCOMPARE(m_target.at(px, 64, 10), (std::array<int, 3>{200, 100, 50}));  // divider
    }

    void autoPicksTopBottomForWideImages()
    {
        CompareSettings c;
        c.mode = CompareMode::Split;
        RenderState s = compareState(64, 48 * 2 + CompareLayout::kDivider, c);
        m_renderer.render(s, m_target.fbo);
        QCOMPARE(m_renderer.layout().orientation, SplitOrientation::TopBottom);
        const auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                QCOMPARE(m_target.at(px, x, y), src(m_frame->av, x, y));
                QCOMPARE(m_target.at(px, x, 48 + CompareLayout::kDivider + y), src(m_frameB->av, x, y));
            }
    }

    void wipeSplitsAtTheDivider()
    {
        CompareSettings c;
        c.mode = CompareMode::Wipe;
        c.wipe = 0.25;  // column 16
        RenderState s = compareState(64, 48, c);
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                const auto want = x < 16 ? src(m_frame->av, x, y) : x == 16 ? std::array<int, 3>{200, 100, 50} : src(m_frameB->av, x, y);
                QCOMPARE(m_target.at(px, x, y), want);
            }
    }

    void flipShowsOneVideo()
    {
        CompareSettings c;
        c.mode = CompareMode::Flip;
        c.flipShowsB = true;
        RenderState s = compareState(64, 48, c);
        m_renderer.render(s, m_target.fbo);
        auto px = m_target.read(m_gl.gl());
        QCOMPARE(m_target.at(px, 5, 7), src(m_frameB->av, 5, 7));
        s.compare.flipShowsB = false;
        m_renderer.render(s, m_target.fbo);
        px = m_target.read(m_gl.gl());
        QCOMPARE(m_target.at(px, 5, 7), src(m_frame->av, 5, 7));
        QVERIFY(!m_renderer.timings().converted);  // flipping does not re-convert
    }

    void differenceIsAbsoluteAndScaled()
    {
        CompareSettings c;
        c.mode = CompareMode::Difference;
        RenderState s = compareState(64, 48, c, m_frame);  // same frame twice
        m_renderer.render(s, m_target.fbo);
        auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x)
                QCOMPARE(m_target.at(px, x, y), (std::array<int, 3>{0, 0, 0}));
        s = compareState(64, 48, c);
        s.compare.diffGain = 2.0;
        m_renderer.render(s, m_target.fbo);
        px = m_target.read(m_gl.gl());
        for (int y = 0; y < 48; ++y)
            for (int x = 0; x < 64; ++x) {
                const auto a = src(m_frame->av, x, y), b = src(m_frameB->av, x, y), got = m_target.at(px, x, y);
                for (int ch = 0; ch < 3; ++ch)
                    QVERIFY(std::abs(got[size_t(ch)] - std::min(255, 2 * std::abs(a[size_t(ch)] - b[size_t(ch)]))) <= 1);
            }
    }

    void otherSizeBCoversTheSameArea()
    {
        // B at twice the resolution, view scale 2: each device pixel shows one B pixel.
        const FramePtr big = randomRgb(m_pool, 128, 96, 9);
        CompareSettings c;
        c.mode = CompareMode::Wipe;
        c.wipe = 0.0;  // all B (the line sits in column 0)
        RenderState s = compareState(128, 96, c, big);
        QCOMPARE(s.view.scale(), 2.0);
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        for (int y = 0; y < 96; ++y)
            for (int x = 1; x < 128; ++x)
                QCOMPARE(m_target.at(px, x, y), src(big->av, x, y));
    }

    void oneVideoIgnoresTheMode()
    {
        CompareSettings c;
        c.mode = CompareMode::Difference;
        RenderState s = compareState(64, 48, c);
        s.frames[1] = nullptr;
        s.view.setViewportSize(QSizeF(64, 48));
        m_renderer.render(s, m_target.fbo);
        const auto px = m_target.read(m_gl.gl());
        QCOMPARE(m_target.at(px, 3, 3), src(m_frame->av, 3, 3));
        s.frames = {nullptr, m_frameB};
        m_renderer.render(s, m_target.fbo);
        QCOMPARE(m_target.at(m_target.read(m_gl.gl()), 3, 3), src(m_frameB->av, 3, 3));
    }
};

QTEST_MAIN(TstDisplay)
#include "tst_display.moc"
