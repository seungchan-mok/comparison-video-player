// BT.709 limited range 4:2:0 planes of the drawn picture, for the video encoder. The arithmetic
// is VideoEncoder's CPU conversion in integers (8-bit codes, 16.16 fixed point, the same
// coefficients as uniforms), so both give identical codes. Plane row r is picture row r from
// the top, so the read-back planes are in file order; beyond an odd picture the last column/row
// repeats.
//
// LUMA: one Y per pixel. Otherwise one Cb, Cr per 2x2 block from the sum of its four pixels:
// in .rg (NV12) or, with I420, in two outputs.

uniform sampler2D picture;  // RGBA8, GL rows bottom-up
uniform ivec2 size;         // picture width, height
uniform ivec3 coef;         // R, G, B coefficients of this plane (Y, or U and V below)
uniform ivec3 coefV;

layout(location = 0) out vec4 o0;
#ifdef I420
layout(location = 1) out vec4 o1;
#endif

ivec3 rgbAt(int x, int yTop)
{
    x = min(x, size.x - 1);
    yTop = min(yTop, size.y - 1);
    return ivec3(round(texelFetch(picture, ivec2(x, size.y - 1 - yTop), 0).rgb * 255.0));
}

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy);
#ifdef LUMA
    ivec3 c = rgbAt(p.x, p.y);
    int y = (coef.r * c.r + coef.g * c.g + coef.b * c.b + (16 << 16) + (1 << 15)) >> 16;
    o0 = vec4(float(y) / 255.0, 0.0, 0.0, 1.0);
#else
    ivec3 s = rgbAt(2 * p.x, 2 * p.y) + rgbAt(2 * p.x + 1, 2 * p.y) + rgbAt(2 * p.x, 2 * p.y + 1) + rgbAt(2 * p.x + 1, 2 * p.y + 1);
    int u = (coef.r * s.r + coef.g * s.g + coef.b * s.b + (128 << 18) + (1 << 17)) >> 18;
    int v = (coefV.r * s.r + coefV.g * s.g + coefV.b * s.b + (128 << 18) + (1 << 17)) >> 18;
#ifdef I420
    o0 = vec4(float(u) / 255.0, 0.0, 0.0, 1.0);
    o1 = vec4(float(v) / 255.0, 0.0, 0.0, 1.0);
#else
    o0 = vec4(float(u) / 255.0, float(v) / 255.0, 0.0, 1.0);
#endif
#endif
}
