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
};

QTEST_MAIN(TstDisplay)
#include "tst_display.moc"
