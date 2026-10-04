#include "engine/FrameAllocator.h"
#include "engine/GpuFrame.h"
#include "engine/HwDevice.h"
#include "render/Renderer.h"
#include "support/GlTestContext.h"

#include <QtTest>

#include <random>

using namespace scvp;
using namespace scvp::render;

class TstCudaInterop : public QObject {
    Q_OBJECT
    test::GlTestContext m_gl;
    std::shared_ptr<GpuFramePool> m_gpu;
    FramePool m_cpu;
    ShaderLibrary m_shaders;

    std::shared_ptr<Frame> cpuFrame(AVPixelFormat fmt, int w, int h, uint32_t seed)
    {
        auto f = std::make_shared<Frame>();
        f->av = m_cpu.allocFrame(fmt, w, h);
        std::mt19937 rng(seed);
        size_t sizes[4] = {};
        ptrdiff_t ls[4] = {f->av->linesize[0], f->av->linesize[1], f->av->linesize[2], f->av->linesize[3]};
        av_image_fill_plane_sizes(sizes, fmt, h, ls);
        for (int i = 0; i < 4; ++i)
            for (size_t b = 0; b < sizes[i]; ++b)
                f->av->data[i][b] = uint8_t(rng());
        f->av->colorspace = AVCOL_SPC_BT709;
        f->av->color_range = AVCOL_RANGE_MPEG;
        f->bytes = ff::frameBufferBytes(f->av);
        return f;
    }

private slots:
    void initTestCase()
    {
        if (!m_gl.create())
            QSKIP("no OpenGL 3.3 context");
        if (!m_gl.renderer().contains(QLatin1String("NVIDIA")))
            QSKIP("GL is not on an NVIDIA GPU");
        AVBufferRef* dev = HwDevices::instance().get(AV_HWDEVICE_TYPE_CUDA);
        m_gpu = GpuFramePool::create(dev);
        av_buffer_unref(&dev);
        if (!m_gpu)
            QSKIP("no CUDA device");
    }

    void zeroCopyMatchesUpload_data()
    {
        QTest::addColumn<int>("format");
        QTest::newRow("nv12") << int(AV_PIX_FMT_NV12);
        QTest::newRow("p010") << int(AV_PIX_FMT_P010LE);
        QTest::newRow("yuv444p") << int(AV_PIX_FMT_YUV444P);
    }

    // The same frame through the zero-copy path and through the CPU upload must convert to
    // bit-identical RGB.
    void zeroCopyMatchesUpload()
    {
        QFETCH(int, format);
        GL* gl = m_gl.gl();
        auto cpu = cpuFrame(AVPixelFormat(format), 333, 187, uint32_t(format));
        auto gpu = makeGpuFrame(m_gpu->upload(cpu->av), cpu->av);
        QVERIFY(gpu->gpu);

        CpuUploadSource up;
        CudaInteropSource interop;
        RgbConverter a, b;
        const auto layout = *PixelLayout::of(AVPixelFormat(format));
        const ColorParams c = ColorParams::resolve(layout, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, 187);
        QVERIFY(up.update(gl, *cpu));
        QVERIFY(a.convert(gl, m_shaders, up, c, true));
        QVERIFY(interop.update(gl, *gpu));
        QVERIFY(b.convert(gl, m_shaders, interop, c, true));
        QCOMPARE(b.readRgb(gl), a.readRgb(gl));
        QCOMPARE(b.readCodes(gl), a.readCodes(gl));
        up.release(gl);
        interop.release(gl);
        a.release(gl);
        b.release(gl);
    }

    void rendererUsesZeroCopy()
    {
        GL* gl = m_gl.gl();
        Renderer r;
        r.initialize(gl);
        auto cpu = cpuFrame(AV_PIX_FMT_NV12, 640, 360, 3);
        RenderState s;
        s.frames[0] = makeGpuFrame(m_gpu->upload(cpu->av), cpu->av);
        s.framebufferSize = QSize(640, 360);
        s.view.setViewportSize(QSizeF(640, 360));
        s.view.setImageSize(QSizeF(640, 360));
        GLuint tex = 0, fbo = 0;
        gl->glGenTextures(1, &tex);
        gl->glBindTexture(GL_TEXTURE_2D, tex);
        gl->glTexImage2D(GL_TEXTURE_2D, 0, GL_RGBA8, 640, 360, 0, GL_RGBA, GL_UNSIGNED_BYTE, nullptr);
        gl->glGenFramebuffers(1, &fbo);
        gl->glBindFramebuffer(GL_FRAMEBUFFER, fbo);
        gl->glFramebufferTexture2D(GL_FRAMEBUFFER, GL_COLOR_ATTACHMENT0, GL_TEXTURE_2D, tex, 0);
        r.render(s, fbo);
        QVERIFY(r.timings().zeroCopy);
        QCOMPARE(r.timings().uploadBytes, size_t(0));
        QVERIFY(r.timings().converted);
        r.release();
        gl->glDeleteFramebuffers(1, &fbo);
        gl->glDeleteTextures(1, &tex);
        QCOMPARE(int(gl->glGetError()), int(GL_NO_ERROR));
    }
};

QTEST_MAIN(TstCudaInterop)
#include "tst_cudainterop.moc"
