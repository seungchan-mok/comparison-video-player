// Draws the converted RGB image(s) into a viewport with zoom/pan. Positions are in "view image"
// pixels (video A's grid); video B is sampled at x * kB so a B of another size covers the same
// area. Magnified (lod <= 0) pixels are fetched exactly with texelFetch so screen pixels map to
// source pixels without filtering and agree with ViewTransform::deviceToImage(); minified views
// use the mip chain.
//
// mode 0: img0 only, 1: wipe (img0 left of wipeX, img1 right), 2: |img0 - img1| x gain.

uniform sampler2D img0;
uniform sampler2D img1;
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

out vec4 o;

vec3 fetch(sampler2D img, vec2 size, vec2 k, vec2 x)
{
    vec2 t = x * k;
    if (lod > 0.0)
        return textureLod(img, t / size, lod + log2(max(k.x, k.y))).rgb;
    return texelFetch(img, ivec2(min(floor(t), size - 1.0)), 0).rgb;
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
    vec3 c;
    if (mode == 1)
        c = p.x < wipeX ? fetch(img0, size0, k0, x) : fetch(img1, size1, k1, x);
    else if (mode == 2)
        c = min(abs(fetch(img0, size0, k0, x) - fetch(img1, size1, k1, x)) * gain, vec3(1.0));
    else
        c = fetch(img0, size0, k0, x);
    o = vec4(c, 1.0);
}
