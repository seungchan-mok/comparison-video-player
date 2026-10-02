// Source planes -> RGB at source resolution, one fragment per source pixel. Exactly one of
// PLANAR_YUV, SEMI_YUV, PACKED_422, PLANAR_RGB, PACKED_RGB, GRAY is defined; EMIT_CODES adds a
// second output with the raw code values (for the YUV value display and the inspector).
// Chroma is sampled nearest (each pixel uses the chroma sample covering it), so every output
// value follows from exact integer codes; ColorParams::toRgb() mirrors this on the CPU.

uniform usampler2D t0;
uniform usampler2D t1;
uniform usampler2D t2;
uniform ivec2 chromaShift;
uniform uint shift;
uniform uint mask;
uniform ivec4 swz;
uniform vec3 off;
uniform vec3 scl;
uniform mat3 M;
uniform float outMax;
uniform int flipY;
uniform int height;

layout(location = 0) out vec4 oRgb;
#ifdef EMIT_CODES
layout(location = 1) out uvec4 oCodes;
#endif

uint C(uint v) { return (v >> shift) & mask; }

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);  // row 0 = top image row, as uploaded
    if (flipY != 0)
        p.y = height - 1 - p.y;
    uvec3 k;
#if defined(PLANAR_YUV)
    ivec2 c = p >> chromaShift;
    k = uvec3(C(texelFetch(t0, p, 0).r), C(texelFetch(t1, c, 0).r), C(texelFetch(t2, c, 0).r));
#elif defined(SEMI_YUV)
    ivec2 c = p >> chromaShift;
    uvec4 uv = texelFetch(t1, c, 0);
    k = uvec3(C(texelFetch(t0, p, 0).r), C(uv[swz.x]), C(uv[swz.y]));
#elif defined(PACKED_422)
    uvec4 q = texelFetch(t0, ivec2(p.x >> 1, p.y), 0);
    k = uvec3(q[(p.x & 1) == 0 ? swz.x : swz.y], q[swz.z], q[swz.w]);
#elif defined(PLANAR_RGB)
    // FFmpeg GBRP plane order: t0 = G, t1 = B, t2 = R.
    k = uvec3(C(texelFetch(t2, p, 0).r), C(texelFetch(t0, p, 0).r), C(texelFetch(t1, p, 0).r));
#elif defined(PACKED_RGB)
    uvec4 q = texelFetch(t0, p, 0);
    k = uvec3(C(q[swz.x]), C(q[swz.y]), C(q[swz.z]));
#elif defined(GRAY)
    k = uvec3(C(texelFetch(t0, p, 0).r));
#endif
    vec3 v = (vec3(k) - off) * scl;
    vec3 rgb = clamp(M * v, 0.0, 1.0);
    oRgb = vec4(floor(rgb * outMax + 0.5) / outMax, 1.0);
#ifdef EMIT_CODES
    oCodes = uvec4(k, 0u);
#endif
}
