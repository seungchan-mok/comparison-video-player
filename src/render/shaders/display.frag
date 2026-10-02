// Draws the converted RGB image(s) into a viewport with zoom/pan. Positions are in "view image"
// pixels (video A's grid); video B is sampled at x * kB so a B of another size covers the same
// area. Magnified (lod <= 0) pixels are fetched exactly with texelFetch so screen pixels map to
// source pixels without filtering and agree with ViewTransform::deviceToImage(); minified views
// use the mip chain.
//
// mode 0: img0 only, 1: wipe (img0 left of wipeX, img1 right), 2: |img0 - img1| x gain.
// Magnified far enough, a grid separates the pixels and each pixel shows its values as digits
// (ValueGeometry): displayed RGB at source bit depth, the source codes, or signed A - B.

uniform sampler2D img0;
uniform sampler2D img1;
uniform usampler2D codes0;
uniform usampler2D codes1;
uniform sampler2DArray digits;
uniform vec2 size0;    // texture sizes, source pixels
uniform vec2 size1;
uniform vec2 k0;       // view image px -> texture px
uniform vec2 k1;
uniform vec2 viewSize; // view image size (bounds)
uniform vec4 vp;       // viewport x, y, w, h in framebuffer pixels (GL origin bottom-left)
uniform vec2 T;        // translation, device px, top-left origin
uniform float S;       // device px per view image px
uniform float lod;     // > 0: minifying, sample this mip level
uniform vec3 bg;
uniform int mode;
uniform float wipeX;   // device px from the viewport's left edge
uniform float gain;
uniform vec3 lineColor;

uniform int values;      // 0 off, 1 RGB, 2 codes
uniform float maxCode0;  // 2^bits - 1 of each RGB image
uniform float maxCode1;
uniform int rows;        // 1 (gray) or 3
uniform int field;       // glyphs per row
uniform vec2 glyph;      // glyph width, height (cell units)
uniform float rowH;
uniform float gridAlpha; // 0: no grid
uniform vec2 cursor;     // view pixel outlined (linked cursor), negative: none

out vec4 o;

vec3 fetch(sampler2D img, vec2 size, vec2 k, vec2 x)
{
    vec2 t = x * k;
    if (lod > 0.0)
        return textureLod(img, t / size, lod + log2(max(k.x, k.y))).rgb;
    return texelFetch(img, ivec2(min(floor(t), size - 1.0)), 0).rgb;
}

ivec3 fetchCodes(usampler2D img, vec2 size, vec2 k, vec2 x)
{
    return ivec3(texelFetch(img, ivec2(min(floor(x * k), size - 1.0)), 0).rgb);
}

ivec3 quantize(vec3 c, float maxCode) { return ivec3(floor(c * maxCode + 0.5)); }

float luminance(vec3 c)
{
    vec3 l = mix(c / 12.92, pow((c + 0.055) / 1.055, vec3(2.4)), step(0.04045, c));
    return dot(l, vec3(0.2126, 0.7152, 0.0722));
}

// Coverage of the digit glyph at cell position u for this pixel's values.
float digitCoverage(vec2 u, ivec3 v)
{
    float top = 0.5 - float(rows) * rowH * 0.5;
    int r = int(floor((u.y - top) / rowH));
    if (r < 0 || r >= rows)
        return 0.0;
    float gy = (u.y - top - float(r) * rowH - (rowH - glyph.y) * 0.5) / glyph.y;
    float left = 0.5 - float(field) * glyph.x * 0.5;
    float cx = (u.x - left) / glyph.x;
    int col = int(floor(cx));
    if (gy < 0.0 || gy >= 1.0 || col < 0 || col >= field)
        return 0.0;
    int value = r == 0 ? v.r : r == 1 ? v.g : v.b;
    int a = abs(value);
    int nd = 1;
    for (int t = a; t >= 10; t /= 10)
        nd++;
    int pos = field - 1 - col;  // power of ten of this column (right aligned)
    int layer;
    if (pos < nd) {
        int p = 1;
        for (int i = 0; i < pos; i++)
            p *= 10;
        layer = (a / p) % 10;
    } else if (pos == nd && value < 0) {
        layer = 10;  // minus sign
    } else {
        return 0.0;  // leading zero suppressed
    }
    vec2 g = vec2(cx - float(col), gy);
    vec2 dx = vec2(1.0 / (S * glyph.x), 0.0);
    vec2 dy = vec2(0.0, 1.0 / (S * glyph.y));
    return textureGrad(digits, vec3(g, float(layer)), dx, dy).r;
}

void main()
{
    vec2 p = vec2(gl_FragCoord.x - vp.x, vp.y + vp.w - gl_FragCoord.y);  // pixel centre, top-left origin
    if (mode == 1 && abs(p.x - wipeX) < 0.5) {
        o = vec4(lineColor, 1.0);
        return;
    }
    vec2 x = (p - T) / S;
    if (any(lessThan(x, vec2(0.0))) || any(greaterThanEqual(x, viewSize))) {
        o = vec4(bg, 1.0);
        return;
    }
    bool second = mode == 1 && p.x >= wipeX;
    vec3 c0 = second ? vec3(0.0) : fetch(img0, size0, k0, x);
    vec3 c1 = mode != 0 ? fetch(img1, size1, k1, x) : vec3(0.0);
    vec3 c = second ? c1 : mode == 2 ? min(abs(c0 - c1) * gain, vec3(1.0)) : c0;

    if (lod <= 0.0) {
        vec2 u = fract(x);
        if (gridAlpha > 0.0 && (u.x * S < 1.0 || u.y * S < 1.0))
            c = mix(c, vec3(luminance(c) > 0.179 ? 0.0 : 1.0), gridAlpha);
        if (cursor.x >= 0.0 && floor(x) == cursor) {
            vec2 e = min(u, 1.0 - u) * S;  // device px to the cell edge
            if (min(e.x, e.y) < 2.0) {
                o = vec4(luminance(c) > 0.179 ? vec3(0.0) : vec3(1.0, 0.85, 0.2), 1.0);
                return;
            }
        }
        if (values != 0) {
            ivec3 v;
            if (values == 2) {
                v = second ? fetchCodes(codes1, size1, k1, x) : fetchCodes(codes0, size0, k0, x);
                if (mode == 2)
                    v -= fetchCodes(codes1, size1, k1, x);
            } else {
                v = second ? quantize(c1, maxCode1) : quantize(c0, maxCode0);
                if (mode == 2)
                    v -= quantize(c1, maxCode0);
            }
            float cov = digitCoverage(u, v);
            c = mix(c, vec3(luminance(c) > 0.179 ? 0.0 : 1.0), cov);
        }
    }
    o = vec4(c, 1.0);
}
