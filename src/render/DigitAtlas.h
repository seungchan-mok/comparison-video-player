#pragma once

#include "render/GlCommon.h"

#include <QImage>

#include <array>

namespace scvp::render {

// Glyphs for pixel values: 0-9 and '-' as layers of a mipmapped R8 2D array texture, drawn once
// with QPainter from the system monospace font. The display shader samples it with explicit
// gradients, so the digits cost nothing on the CPU per frame and stay smooth at any zoom.
class DigitAtlas {
public:
    static constexpr int kGlyphs = 11;  // '0'..'9', '-'
    static constexpr int kMinus = 10;
    static constexpr int kWidth = 40;   // glyph cell, texels
    static constexpr int kHeight = 64;

    bool create(GL* gl);
    void release(GL* gl);
    GLuint texture() const { return m_tex; }
    // Glyph width / height of the cell.
    static constexpr double aspect() { return double(kWidth) / double(kHeight); }
    // Coverage masks (Format_Grayscale8, white = ink), for tests.
    const QImage& glyph(int i) const { return m_glyphs[size_t(i)]; }
    static std::array<QImage, kGlyphs> render();

private:
    GLuint m_tex = 0;
    std::array<QImage, kGlyphs> m_glyphs;
};

} // namespace scvp::render
