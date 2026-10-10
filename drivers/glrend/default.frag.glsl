##ifdef GL_ES
#version 300 es
precision highp float;
precision mediump int;
precision lowp usampler2D;
##endif
##ifdef GL_CORE
#version 140
#extension GL_ARB_explicit_attrib_location:require
##endif

in vec3 colour;
in vec2 uv;

uniform sampler2D uSampler;
uniform float uFlipVertically;
uniform int uDiscardPurplePixels;

// How the 2D layer (menus, HUD, text, videos) is stretched over the window:
// 0 nearest, 1 sharp bilinear, 2 bilinear.
uniform int uScaleMode;
// Size of the 2D layer in pixels, and window pixels per 2D layer pixel.
uniform vec2 uTexSize;
uniform vec2 uScale;

layout (location = 0) out vec4 mainColour;

// One pixel of the 2D layer, premultiplied by its alpha. Pixels the game left
// pure purple are transparent: working with alpha, instead of discarding, lets
// smoothed edges blend into what is behind without a pink fringe.
vec4 fetchPixel(ivec2 p)
{
    ivec2 size = ivec2(uTexSize);
    vec3 c = texelFetch(uSampler, clamp(p, ivec2(0), size - ivec2(1)), 0).rgb;
    float a = (uDiscardPurplePixels == 1 && c == vec3(1.0, 0.0, 1.0)) ? 0.0 : 1.0;
    return vec4(c * a, a);
}

void main()
{
    vec2 texel = vec2(uv.x, abs(uFlipVertically - uv.y)) * uTexSize;

    if (uScaleMode == 0) {
        vec4 p = fetchPixel(ivec2(floor(texel)));
        if (p.a == 0.0) {
            discard;
        }
        mainColour = vec4(p.rgb, 1.0);
        return;
    }

    vec2 pos = texel;
    if (uScaleMode == 1) {
        // Sharp bilinear: each pixel of the 2D layer stays crisp across the
        // middle of its on-screen block and only blends into its neighbours
        // over about one screen pixel at the edge. Pixels come out the same
        // size whatever the scale, with no blur and none of the uneven widths
        // nearest filtering gives at a non-integer scale.
        vec2 whole = floor(texel);
        vec2 centre_dist = (texel - whole) - 0.5;
        vec2 crisp = max(0.5 - 0.5 / uScale, 0.0);
        vec2 f = (centre_dist - clamp(centre_dist, -crisp, crisp)) * uScale + 0.5;
        pos = whole + f;
    }

    // Bilinear between pixel centres
    vec2 p = pos - 0.5;
    vec2 base = floor(p);
    vec2 w = p - base;
    ivec2 b = ivec2(base);
    vec4 c = mix(mix(fetchPixel(b), fetchPixel(b + ivec2(1, 0)), w.x),
                 mix(fetchPixel(b + ivec2(0, 1)), fetchPixel(b + ivec2(1, 1)), w.x), w.y);
    if (c.a <= 0.0) {
        discard;
    }
    // Premultiplied: the caller blends with (ONE, ONE_MINUS_SRC_ALPHA)
    mainColour = c;
}
