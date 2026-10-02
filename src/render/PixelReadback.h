#pragma once

#include "render/GlCommon.h"

#include <QPoint>
#include <QPointF>

#include <array>

namespace scvp::render {

class RgbConverter;

// Values of the source pixel under the cursor in one video.
struct SlotSample {
    bool valid = false;
    QPoint texel;                 // source pixel
    int bitDepth = 8;
    std::array<int, 3> rgb{};     // displayed RGB at the source bit depth
    bool hasCodes = false;
    std::array<int, 3> codes{};   // source code values (Y U V, R G B, gray)
    bool gray = false;
};

struct PixelSample {
    bool valid = false;
    QPointF viewPos;              // view image coordinates that were sampled
    std::array<SlotSample, 2> videos;
};

// Reads one pixel of each converted image back without stalling the pipeline: glReadPixels
// into a pixel buffer object plus a fence; the result is collected on a later frame once the
// GPU has got there. Reads the same textures the display pass samples, so the numbers always
// equal the digits on screen, for frames uploaded from memory and zero-copy GPU frames alike.
class PixelReadback {
public:
    // Starts reading `texel` of each valid converter (nullptr: slot absent).
    void request(GL* gl, const std::array<const RgbConverter*, 2>& conv, const std::array<QPoint, 2>& texel,
                 const std::array<bool, 2>& gray, QPointF viewPos);
    // Collects a finished read; with wait, blocks until it is (tests).
    bool poll(GL* gl, bool wait = false);
    bool pending() const { return m_fence != nullptr; }
    const PixelSample& result() const { return m_result; }
    void clear();  // nothing under the cursor: drop the result and any read in flight
    void release(GL* gl);

private:
    GLuint m_pbo = 0;
    GLsync m_fence = nullptr;
    PixelSample m_inFlight;
    std::array<bool, 2> m_deep{};
    bool m_discard = false;
    PixelSample m_result;
};

} // namespace scvp::render
