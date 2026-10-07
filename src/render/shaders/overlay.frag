// One overlay layer (premultiplied RGBA, top row first as QImage stores it) drawn 1:1 into a
// viewport of its size; blended with ONE, ONE_MINUS_SRC_ALPHA.

uniform sampler2D layer;
uniform vec4 rect;  // viewport x, y, w, h in framebuffer pixels (GL origin bottom-left)

out vec4 o;

void main()
{
    ivec2 p = ivec2(gl_FragCoord.xy - rect.xy);
    o = texelFetch(layer, ivec2(p.x, int(rect.w) - 1 - p.y), 0);
}
