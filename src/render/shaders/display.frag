// Draws the converted RGB image into a viewport with zoom/pan. Magnified (lod <= 0) pixels are
// fetched exactly with texelFetch so screen pixels map to source pixels without filtering and
// agree with ViewTransform::deviceToImage(); minified views use the mip chain.

uniform sampler2D img;
uniform vec2 imgSize;  // source pixels
uniform vec4 vp;       // viewport x, y, w, h in framebuffer pixels (GL origin bottom-left)
uniform vec2 T;        // translation, device px, top-left origin
uniform float S;       // device px per source px
uniform float lod;     // > 0: minifying, sample this mip level
uniform vec3 bg;

out vec4 o;

void main()
{
    vec2 p = vec2(gl_FragCoord.x - vp.x, vp.y + vp.w - gl_FragCoord.y);  // pixel centre, top-left origin
    vec2 x = (p - T) / S;
    if (any(lessThan(x, vec2(0.0))) || any(greaterThanEqual(x, imgSize))) {
        o = vec4(bg, 1.0);
        return;
    }
    vec4 c = lod > 0.0 ? textureLod(img, x / imgSize, lod) : texelFetch(img, ivec2(x), 0);
    o = vec4(c.rgb, 1.0);
}
