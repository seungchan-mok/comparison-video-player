#include "engine/FrameAllocator.h"
#include "render/RgbConverter.h"
#include "support/GlTestContext.h"
#include <QGuiApplication>
#include <chrono>
#include <cstdio>
#include <cstring>
using namespace scvp; using namespace scvp::render;
int main(int argc, char** argv) {
  QGuiApplication app(argc, argv);
  test::GlTestContext gl; if (!gl.create()) { printf("no GL\n"); return 1; }
  printf("GL: %s\n", qPrintable(gl.renderer()));
  FramePool pool; ShaderLibrary sh;
  for (auto [fmt, name] : {std::pair{AV_PIX_FMT_NV12, "nv12 4K"}, std::pair{AV_PIX_FMT_P010LE, "p010 4K"}, std::pair{AV_PIX_FMT_YUV420P10LE, "yuv420p10 4K"}, std::pair{AV_PIX_FMT_YUV420P, "yuv420p 1080p"}}) {
    const int w = fmt == AV_PIX_FMT_YUV420P ? 1920 : 3840, h = fmt == AV_PIX_FMT_YUV420P ? 1080 : 2160;
    std::vector<std::shared_ptr<Frame>> frames;
    for (int i = 0; i < 8; ++i) {
      auto f = std::make_shared<Frame>();
      f->av = pool.allocFrame(fmt, w, h);
      for (int p = 0; p < 4; ++p)
        if (f->av->data[p])
          memset(f->av->data[p], i * 7, size_t(f->av->linesize[p]) * size_t(h / (p ? 2 : 1)));
      frames.push_back(f);
    }
    CpuUploadSource up; RgbConverter conv; const auto l = *PixelLayout::of(fmt); const auto c = ColorParams::resolve(l, AVCOL_SPC_BT709, AVCOL_RANGE_MPEG, h);
    double upMs = 0, convMs = 0; const int n = 60;
    for (int i = 0; i < n; ++i) {
      auto t0 = std::chrono::steady_clock::now();
      up.update(gl.gl(), *frames[size_t(i) % 8]);
      auto t1 = std::chrono::steady_clock::now();
      conv.convert(gl.gl(), sh, up, c, false);
      gl.gl()->glFinish();
      auto t2 = std::chrono::steady_clock::now();
      if (i >= 5) { upMs += std::chrono::duration<double, std::milli>(t1 - t0).count(); convMs += std::chrono::duration<double, std::milli>(t2 - t1).count(); }
    }
    printf("%-14s upload (CPU side) %6.2f ms/frame, convert+finish %6.2f ms/frame, %zu bytes\n", name, upMs / (n - 5), convMs / (n - 5), up.lastUploadBytes());
    up.release(gl.gl()); conv.release(gl.gl());
  }
}
