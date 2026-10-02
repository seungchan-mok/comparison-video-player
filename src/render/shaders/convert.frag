// Source planes -> RGB at source resolution, one fragment per source pixel. Exactly one of
// PLANAR_YUV, SEMI_YUV, PACKED_422, PLANAR_RGB, PACKED_RGB, GRAY is defined; EMIT_CODES adds a
// second output with the raw code values (for the YUV value display and the inspector).
// Chroma is sampled nearest by default (each pixel uses the chroma sample covering it), so every
// output value follows from exact integer codes; ColorParams::toRgb() mirrors this on the CPU.
// With CHROMA_LINEAR (planar / semi-planar YUV) chroma is interpolated bilinearly between the
// samples around the pixel, honouring the chroma siting; the code output stays nearest.

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
uniform vec2 siting;     // chroma sample position within its luma block (CHROMA_LINEAR)
uniform ivec2 chromaSize;

layout(location = 0) out vec4 oRgb;
#ifdef EMIT_CODES
layout(location = 1) out uvec4 oCodes;
#endif

uint C(uint v) { return (v >> shift) & mask; }

#ifdef CHROMA_LINEAR
// Bilinear chroma at luma pixel p: chroma sample j sits at luma position j * 2^s + siting * (2^s - 1).
vec2 linearChroma(ivec2 p)
{
    vec2 f = vec2(ivec2(1) << chromaShift);
    vec2 c = (vec2(p) - siting * (f - 1.0)) / f;
    ivec2 i0 = ivec2(floor(c));
    vec2 w = c - vec2(i0);
    vec2 acc = vec2(0.0);
    for (int dy = 0; dy < 2; dy++)
        for (int dx = 0; dx < 2; dx++) {
            ivec2 q = clamp(i0 + ivec2(dx, dy), ivec2(0), chromaSize - 1);
            float wt = (dx == 1 ? w.x : 1.0 - w.x) * (dy == 1 ? w.y : 1.0 - w.y);
#if defined(PLANAR_YUV)
            acc += wt * vec2(float(C(texelFetch(t1, q, 0).r)), float(C(texelFetch(t2, q, 0).r)));
#else
            uvec4 uv = texelFetch(t1, q, 0);
            acc += wt * vec2(float(C(uv[swz.x])), float(C(uv[swz.y])));
#endif
        }
    return acc;
}
#endif

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
    vec3 kf = vec3(k);
#ifdef CHROMA_LINEAR
    kf.yz = linearChroma(p);
#endif
    vec3 v = (kf - off) * scl;
    vec3 rgb = clamp(M * v, 0.0, 1.0);
    oRgb = vec4(floor(rgb * outMax + 0.5) / outMax, 1.0);
#ifdef EMIT_CODES
    oCodes = uvec4(k, 0u);
#endif
}
