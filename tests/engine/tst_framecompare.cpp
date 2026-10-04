#include "engine/FrameCompare.h"

#include <QtTest>

using namespace scvp;

namespace {

ff::FramePtr makeFrame(AVPixelFormat fmt, int w, int h, int align = 0)
{
    ff::FramePtr f = ff::makeFrame();
    f->format = fmt;
    f->width = w;
    f->height = h;
    if (av_frame_get_buffer(f.get(), align) < 0)
        return nullptr;
    // Padding after each row's samples holds garbage in real frames: make it differ.
    for (int p = 0; p < 4 && f->buf[p]; ++p)
        std::memset(f->buf[p]->data, 0xA5 + p, f->buf[p]->size);
    return f;
}

// Y = (x + 3y) & mask, U = (5x + y) & mask, V = (x + 7y + 1) & mask, at the frame's depth.
int sample(int c, int x, int y, int mask)
{
    switch (c) {
    case 0: return (x + 3 * y) & mask;
    case 1: return (5 * x + y) & mask;
    default: return (x + 7 * y + 1) & mask;
    }
}

void fillPlanar(AVFrame* f, int bytes, int mask)
{
    for (int c = 0; c < 3; ++c) {
        const int w = c ? (f->width + 1) / 2 : f->width;
        const int h = c ? (f->height + 1) / 2 : f->height;
        for (int y = 0; y < h; ++y)
            for (int x = 0; x < w; ++x) {
                uint8_t* p = f->data[c] + ptrdiff_t(y) * f->linesize[c] + x * bytes;
                const int v = sample(c, x, y, mask);
                if (bytes == 1)
                    p[0] = uint8_t(v);
                else
                    reinterpret_cast<uint16_t*>(p)[0] = uint16_t(v);
            }
    }
}

// NV12 (bytes 1, shift 0) or P010 (bytes 2, the value in the top 10 bits, shift 6).
void fillSemiPlanar(AVFrame* f, int bytes, int shift, int mask)
{
    for (int y = 0; y < f->height; ++y)
        for (int x = 0; x < f->width; ++x) {
            uint8_t* p = f->data[0] + ptrdiff_t(y) * f->linesize[0] + x * bytes;
            const int v = sample(0, x, y, mask) << shift;
            if (bytes == 1)
                p[0] = uint8_t(v);
            else
                reinterpret_cast<uint16_t*>(p)[0] = uint16_t(v);
        }
    for (int y = 0; y < (f->height + 1) / 2; ++y)
        for (int x = 0; x < (f->width + 1) / 2; ++x)
            for (int c = 1; c <= 2; ++c) {
                uint8_t* p = f->data[1] + ptrdiff_t(y) * f->linesize[1] + (2 * x + c - 1) * bytes;
                const int v = sample(c, x, y, mask) << shift;
                if (bytes == 1)
                    p[0] = uint8_t(v);
                else
                    reinterpret_cast<uint16_t*>(p)[0] = uint16_t(v);
            }
}

} // namespace

class TstFrameCompare : public QObject {
    Q_OBJECT

private slots:
    void sameLayout()
    {
        auto a = makeFrame(AV_PIX_FMT_YUV420P, 33, 17, 1);
        auto b = makeFrame(AV_PIX_FMT_YUV420P, 33, 17, 64);  // other strides and padding
        fillPlanar(a.get(), 1, 0xFF);
        fillPlanar(b.get(), 1, 0xFF);
        QVERIFY(samePixelValues(a.get(), b.get()));
        b->data[2][b->linesize[2] * 8 + 16] ^= 1;  // the last V sample
        QVERIFY(!samePixelValues(a.get(), b.get()));
    }

    void nv12AgainstPlanar()
    {
        auto hw = makeFrame(AV_PIX_FMT_NV12, 33, 17);
        auto sw = makeFrame(AV_PIX_FMT_YUV420P, 33, 17);
        fillSemiPlanar(hw.get(), 1, 0, 0xFF);
        fillPlanar(sw.get(), 1, 0xFF);
        QVERIFY(samePixelValues(hw.get(), sw.get()));
        QVERIFY(samePixelValues(sw.get(), hw.get()));
        hw->data[1][hw->linesize[1] * 3 + 2 * 5 + 1] ^= 4;  // one V sample: luma still equal
        QVERIFY(!samePixelValues(hw.get(), sw.get()));
    }

    void p010AgainstPlanar()
    {
        auto hw = makeFrame(AV_PIX_FMT_P010LE, 20, 10);
        auto sw = makeFrame(AV_PIX_FMT_YUV420P10LE, 20, 10);
        fillSemiPlanar(hw.get(), 2, 6, 0x3FF);
        fillPlanar(sw.get(), 2, 0x3FF);
        QVERIFY(samePixelValues(hw.get(), sw.get()));
        reinterpret_cast<uint16_t*>(hw->data[0])[7] ^= 1 << 6;  // lowest bit of one luma value
        QVERIFY(!samePixelValues(hw.get(), sw.get()));
    }

    void incomparable()
    {
        auto a = makeFrame(AV_PIX_FMT_YUV420P, 16, 16);
        fillPlanar(a.get(), 1, 0xFF);
        auto smaller = makeFrame(AV_PIX_FMT_YUV420P, 16, 14);
        fillPlanar(smaller.get(), 1, 0xFF);
        QVERIFY(!samePixelValues(a.get(), smaller.get()));
        auto deeper = makeFrame(AV_PIX_FMT_YUV420P10LE, 16, 16);
        fillPlanar(deeper.get(), 2, 0xFF);  // same numbers, but 10-bit
        QVERIFY(!samePixelValues(a.get(), deeper.get()));
        auto other = makeFrame(AV_PIX_FMT_YUV422P, 16, 16);
        QVERIFY(!samePixelValues(a.get(), other.get()));
    }
};

QTEST_GUILESS_MAIN(TstFrameCompare)
#include "tst_framecompare.moc"
