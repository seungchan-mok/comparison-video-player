#include "render/PixelReadback.h"

#include "render/RgbConverter.h"

#include <cmath>
#include <cstring>

namespace scvp::render {
namespace {

constexpr int kSlotBytes = 16;  // RGBA16 colour (8) + RGBA16UI codes (8)

} // namespace

void PixelReadback::request(GL* gl, const std::array<const RgbConverter*, 2>& conv, const std::array<QPoint, 2>& texel,
                            const std::array<CodeKind, 2>& kinds, QPointF viewPos)
{
    if (m_fence)
        return;  // one read in flight at a time; the next frame asks again
    if (!m_pbo) {
        gl->glGenBuffers(1, &m_pbo);
        gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
        gl->glBufferData(GL_PIXEL_PACK_BUFFER, 2 * kSlotBytes, nullptr, GL_STREAM_READ);
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
    gl->glPixelStorei(GL_PACK_ALIGNMENT, 1);
    m_inFlight = {};
    m_inFlight.viewPos = viewPos;
    m_discard = false;
    bool any = false;
    for (int i = 0; i < 2; ++i) {
        const RgbConverter* c = conv[size_t(i)];
        SlotSample& s = m_inFlight.videos[size_t(i)];
        if (!c || !c->rgbTexture() || texel[size_t(i)].x() < 0 || texel[size_t(i)].y() < 0 || texel[size_t(i)].x() >= c->width() ||
            texel[size_t(i)].y() >= c->height())
            continue;
        s.valid = true;
        s.texel = texel[size_t(i)];
        s.bitDepth = c->bitDepth();
        s.kind = kinds[size_t(i)];
        s.gray = s.kind == CodeKind::Gray;
        s.hasCodes = c->codesTexture() != 0;
        m_deep[size_t(i)] = c->deep();
        gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, c->framebuffer());
        gl->glReadBuffer(GL_COLOR_ATTACHMENT0);
        const auto offset = reinterpret_cast<void*>(intptr_t(i * kSlotBytes));
        gl->glReadPixels(s.texel.x(), s.texel.y(), 1, 1, GL_RGBA, c->deep() ? GL_UNSIGNED_SHORT : GL_UNSIGNED_BYTE, offset);
        if (s.hasCodes) {
            gl->glReadBuffer(GL_COLOR_ATTACHMENT1);
            gl->glReadPixels(s.texel.x(), s.texel.y(), 1, 1, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT,
                             reinterpret_cast<void*>(intptr_t(i * kSlotBytes + 8)));
        }
        any = true;
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    gl->glBindFramebuffer(GL_READ_FRAMEBUFFER, 0);
    if (!any) {
        m_result = {};
        return;
    }
    m_fence = gl->glFenceSync(GL_SYNC_GPU_COMMANDS_COMPLETE, 0);
}

bool PixelReadback::poll(GL* gl, bool wait)
{
    if (!m_fence)
        return false;
    const GLenum st = gl->glClientWaitSync(m_fence, GL_SYNC_FLUSH_COMMANDS_BIT, wait ? GLuint64(1000000000) : 0);
    if (st != GL_ALREADY_SIGNALED && st != GL_CONDITION_SATISFIED)
        return false;
    gl->glDeleteSync(m_fence);
    m_fence = nullptr;
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, m_pbo);
    const auto* p = static_cast<const uint8_t*>(gl->glMapBufferRange(GL_PIXEL_PACK_BUFFER, 0, 2 * kSlotBytes, GL_MAP_READ_BIT));
    if (p) {
        for (int i = 0; i < 2; ++i) {
            SlotSample& s = m_inFlight.videos[size_t(i)];
            if (!s.valid)
                continue;
            const uint8_t* b = p + i * kSlotBytes;
            const double maxCode = double((1u << s.bitDepth) - 1u);
            for (int c = 0; c < 3; ++c) {
                if (m_deep[size_t(i)]) {
                    uint16_t v;
                    std::memcpy(&v, b + 2 * c, 2);
                    s.rgb[size_t(c)] = int(std::lround(double(v) * maxCode / 65535.0));
                } else {
                    s.rgb[size_t(c)] = b[c];
                }
                if (s.hasCodes) {
                    uint16_t v;
                    std::memcpy(&v, b + 8 + 2 * c, 2);
                    s.codes[size_t(c)] = v;
                }
            }
        }
        gl->glUnmapBuffer(GL_PIXEL_PACK_BUFFER);
        m_inFlight.valid = true;
        if (!m_discard)
            m_result = m_inFlight;
    }
    gl->glBindBuffer(GL_PIXEL_PACK_BUFFER, 0);
    m_discard = false;
    return true;
}

void PixelReadback::clear()
{
    m_result = {};
    m_discard = m_fence != nullptr;
}

void PixelReadback::release(GL* gl)
{
    if (m_fence)
        gl->glDeleteSync(m_fence);
    m_fence = nullptr;
    if (m_pbo)
        gl->glDeleteBuffers(1, &m_pbo);
    m_pbo = 0;
    m_result = {};
}

} // namespace scvp::render
