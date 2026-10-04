#pragma once

#include "render/PixelLayout.h"

#include <QOpenGLFunctions_3_3_Core>

namespace scvp::render {

using GL = QOpenGLFunctions_3_3_Core;

struct GlTexFormat {
    GLint internal;
    GLenum format;
    GLenum type;
};

inline GlTexFormat glFormat(TexFormat f)
{
    switch (f) {
    case TexFormat::R8UI: return {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE};
    case TexFormat::R16UI: return {GL_R16UI, GL_RED_INTEGER, GL_UNSIGNED_SHORT};
    case TexFormat::RG8UI: return {GL_RG8UI, GL_RG_INTEGER, GL_UNSIGNED_BYTE};
    case TexFormat::RG16UI: return {GL_RG16UI, GL_RG_INTEGER, GL_UNSIGNED_SHORT};
    case TexFormat::RGB8UI: return {GL_RGB8UI, GL_RGB_INTEGER, GL_UNSIGNED_BYTE};
    case TexFormat::RGBA8UI: return {GL_RGBA8UI, GL_RGBA_INTEGER, GL_UNSIGNED_BYTE};
    case TexFormat::RGB16UI: return {GL_RGB16UI, GL_RGB_INTEGER, GL_UNSIGNED_SHORT};
    case TexFormat::RGBA16UI: return {GL_RGBA16UI, GL_RGBA_INTEGER, GL_UNSIGNED_SHORT};
    }
    return {GL_R8UI, GL_RED_INTEGER, GL_UNSIGNED_BYTE};
}

// Largest texture dimension the driver allows (16384 on most GPUs).
inline int maxTextureSize(GL* gl)
{
    GLint v = 0;
    gl->glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);
    return v > 0 ? v : 4096;
}

} // namespace scvp::render
