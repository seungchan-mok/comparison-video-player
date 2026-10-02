#include "engine/FrameAllocator.h"
#include "render/Renderer.h"
#include "support/GlTestContext.h"

#include <QtTest>

using namespace scvp;
using namespace scvp::render;

namespace {

using Px = std::array<int, 3>;

FramePtr makeRgb(FramePool& pool, const std::vector<Px>& pixels, int w, int h)
{
    auto f = std::make_shared<Frame>();
    f->av = pool.allocFrame(AV_PIX_FMT_RGB24, w, h);
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c)
                f->av->data[0][size_t(y) * size_t(f->av->linesize[0]) + size_t(x * 3 + c)] = uint8_t(pixels[size_t(y * w + x)][size_t(c)]);
    return f;
}

FramePtr makeYuv444(FramePool& pool, AVPixelFormat fmt, int w, int h, const std::vector<Px>& codes)
{
    auto f = std::make_shared<Frame>();
    f->av = pool.allocFrame(fmt, w, h);
    const bool deep = fmt != AV_PIX_FMT_YUV444P;
    for (int y = 0; y < h; ++y)
        for (int x = 0; x < w; ++x)
            for (int c = 0; c < 3; ++c) {
                uint8_t* row = f->av->data[c] + ptrdiff_t(y) * f->av->linesize[c];
                const int v = codes[size_t(y * w + x)][size_t(c)];
                if (deep)
                    reinterpret_cast<uint16_t*>(row)[x] = uint16_t(v);
                else
                    row[x] = uint8_t(v);
            }
    f->av->color_range = AVCOL_RANGE_MPEG;
    f->av->colorspace = AVCOL_SPC_BT709;
    return f;
}

} // namespace

class TstDigits : public QObject {
    Q_OBJECT

    test::GlTestContext m_gl;
    Renderer m_renderer;
    test::RenderTarget m_target;
    FramePool m_pool;
    std::array<QImage, DigitAtlas::kGlyphs> m_glyphs;
    std::vector<uint8_t> m_px;
    RenderState m_state;

    void render(const FramePtr& a, const FramePtr& b, double scale, CompareMode mode = CompareMode::Split)
    {
        const int w = a->width(), h = a->height();
        const int vw = int(w * scale), vh = int(h * scale);
        m_target.create(m_gl.gl(), vw, vh);
        RenderState s;
        s.frames = {a, b};
        s.framebufferSize = QSize(vw, vh);
        s.compare.mode = mode;
        s.view.setImageSize(QSizeF(w, h));
        s.view.setViewportSize(QSizeF(vw, vh));
        s.overlay = m_state.overlay;
        m_state = s;
        m_renderer.render(s, m_target.fbo);
        m_px = m_target.read(m_gl.gl());
    }

    Px at(int x, int y) const { return m_target.at(m_px, x, y); }

    // Reads back the digits of one cell by matching each glyph box against the atlas glyphs.
    QStringList readCell(int cx, int cy, int maxValue, bool gray = false, bool signedValues = false) const
    {
        const double S = m_state.view.scale();
        const QPointF t = m_state.view.renderTranslation();
        const ValueGeometry g = ValueGeometry::compute(maxValue, gray, signedValues, S, 1.0);
        const double ox = t.x() + S * cx, oy = t.y() + S * cy;
        const Px bgc = at(int(ox + 0.1 * S), int(oy + 0.5 * S));  // left margin: no ink, no grid
        const double lum = 0.2126 * bgc[0] + 0.7152 * bgc[1] + 0.0722 * bgc[2];
        Q_UNUSED(lum);
        QStringList rows;
        for (int r = 0; r < g.rows; ++r) {
            QString text;
            for (int col = 0; col < g.field; ++col) {
                const int x0 = int(std::lround(ox + S * (g.left() + col * g.glyphW)));
                const int y0 = int(std::lround(oy + S * (g.top() + r * g.rowH + (g.rowH - g.glyphH) / 2)));
                const int bw = int(std::lround(S * g.glyphW)), bh = int(std::lround(S * g.glyphH));
                // Ink coverage of every pixel in the box (ink is black or white).
                std::vector<double> cov(size_t(bw * bh));
                bool any = false;
                for (int y = 0; y < bh; ++y)
                    for (int x = 0; x < bw; ++x) {
                        const Px p = at(x0 + x, y0 + y);
                        double best = 0;
                        for (int ink : {0, 255}) {
                            double num = 0, den = 0;
                            for (int c = 0; c < 3; ++c) {
                                num += double(p[size_t(c)] - bgc[size_t(c)]) * (ink - bgc[size_t(c)]);
                                den += double(ink - bgc[size_t(c)]) * (ink - bgc[size_t(c)]);
                            }
                            if (den > 0)
                                best = std::max(best, num / den);
                        }
                        cov[size_t(y * bw + x)] = best;
                        any |= best > 0.5;
                    }
                if (!any) {
                    text += QLatin1Char(' ');
                    continue;
                }
                int bestGlyph = -1;
                double bestErr = 1e300;
                for (int gi = 0; gi < DigitAtlas::kGlyphs; ++gi) {
                    const QImage tpl = m_glyphs[size_t(gi)].scaled(bw, bh, Qt::IgnoreAspectRatio, Qt::SmoothTransformation);
                    double err = 0;
                    for (int y = 0; y < bh; ++y)
                        for (int x = 0; x < bw; ++x) {
                            const double d = cov[size_t(y * bw + x)] - tpl.pixel(x, y) % 256 / 255.0;
                            err += d * d;
                        }
                    if (err < bestErr) {
                        bestErr = err;
                        bestGlyph = gi;
                    }
                }
                text += bestGlyph == DigitAtlas::kMinus ? QLatin1Char('-') : QLatin1Char(char('0' + bestGlyph));
            }
            rows << text;
        }
        return rows;
    }

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
        QVERIFY(m_renderer.initialize(m_gl.gl()));
        m_glyphs = DigitAtlas::render();
    }

    void cleanupTestCase()
    {
        if (m_gl.gl()) {
            m_target.destroy(m_gl.gl());
            m_renderer.release();
        }
    }

    void glyphsHaveInk()
    {
        for (const QImage& g : m_glyphs)
            QVERIFY(!g.isNull());
        QVERIFY(m_glyphs[1] != m_glyphs[7]);
    }

    void rgbValuesReadBack()
    {
        const FramePtr f = makeRgb(m_pool, {{255, 7, 0}, {0, 0, 0}, {128, 64, 32}, {12, 200, 99}}, 2, 2);
        render(f, nullptr, 96);
        QCOMPARE(readCell(0, 0, 255), (QStringList{QStringLiteral("255"), QStringLiteral("  7"), QStringLiteral("  0")}));
        QCOMPARE(readCell(1, 0, 255), (QStringList{QStringLiteral("  0"), QStringLiteral("  0"), QStringLiteral("  0")}));
        QCOMPARE(readCell(0, 1, 255), (QStringList{QStringLiteral("128"), QStringLiteral(" 64"), QStringLiteral(" 32")}));
        QCOMPARE(readCell(1, 1, 255), (QStringList{QStringLiteral(" 12"), QStringLiteral("200"), QStringLiteral(" 99")}));
    }

    void inkContrastsWithThePixel()
    {
        const FramePtr f = makeRgb(m_pool, {{255, 255, 255}, {0, 0, 0}}, 2, 1);
        render(f, nullptr, 96);
        int dark = 0, light = 0;
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x) {
                dark += at(x, y)[0] < 64 ? 1 : 0;        // ink on the white pixel
                light += at(96 + x, y)[0] > 192 ? 1 : 0;  // ink on the black pixel
            }
        QVERIFY(dark > 100);
        QVERIFY(light > 100);
    }

    void smallCellsHaveNoDigitsButAGrid()
    {
        const FramePtr f = makeRgb(m_pool, {{200, 100, 50}, {200, 100, 50}}, 2, 1);
        render(f, nullptr, 16);
        int odd = 0;
        for (int y = 0; y < 16; ++y)
            for (int x = 1; x < 16; ++x)  // column 0 of each cell is the grid line
                odd += at(x, y) != at(1, 1) && y > 0 ? 1 : 0;
        QCOMPARE(odd, 0);
        QVERIFY(at(16, 5) != at(17, 5));  // grid line between the two pixels
        render(f, nullptr, 4);            // below the grid threshold: flat
        QCOMPARE(at(4, 2), at(5, 2));
    }

    void valuesCanBeHidden()
    {
        const FramePtr f = makeRgb(m_pool, {{255, 7, 0}}, 1, 1);
        m_state.overlay.values = false;
        m_state.overlay.grid = false;
        render(f, nullptr, 96);
        m_state.overlay = {};
        for (int y = 0; y < 96; ++y)
            for (int x = 0; x < 96; ++x)
                QCOMPARE(at(x, y), (Px{255, 7, 0}));
    }

    void differenceIsSigned()
    {
        const FramePtr a = makeRgb(m_pool, {{10, 200, 50}}, 1, 1);
        const FramePtr b = makeRgb(m_pool, {{30, 100, 50}}, 1, 1);
        render(a, b, 128, CompareMode::Difference);
        QCOMPARE(readCell(0, 0, 255, false, true), (QStringList{QStringLiteral(" -20"), QStringLiteral(" 100"), QStringLiteral("   0")}));
    }

    void yuvCodes()
    {
        m_state.overlay.mode = ValueMode::Codes;
        const FramePtr f = makeYuv444(m_pool, AV_PIX_FMT_YUV444P, 1, 1, {{235, 16, 128}});
        render(f, nullptr, 96);
        QCOMPARE(readCell(0, 0, 255), (QStringList{QStringLiteral("235"), QStringLiteral(" 16"), QStringLiteral("128")}));
        const FramePtr deep = makeYuv444(m_pool, AV_PIX_FMT_YUV444P10LE, 1, 1, {{940, 64, 1023}});
        render(deep, nullptr, 128);
        QCOMPARE(readCell(0, 0, 1023), (QStringList{QStringLiteral(" 940"), QStringLiteral("  64"), QStringLiteral("1023")}));
        m_state.overlay = {};
    }

    void tenBitRgbUsesFourDigits()
    {
        const FramePtr deep = makeYuv444(m_pool, AV_PIX_FMT_YUV444P10LE, 1, 1, {{940, 512, 512}});
        render(deep, nullptr, 128);
        // Limited-range white: RGB 1023 at 10 bits.
        QCOMPARE(readCell(0, 0, 1023), (QStringList{QStringLiteral("1023"), QStringLiteral("1023"), QStringLiteral("1023")}));
        QCOMPARE(int(m_gl.gl()->glGetError()), int(GL_NO_ERROR));
    }
};

QTEST_MAIN(TstDigits)
#include "tst_digits.moc"
