/*
 * fs_reveal_dissolve.sc — generic intro-reveal wipe (JCE_POSTFX_CUSTOM).
 *
 * Covers the frame with a flat fill color that dissolves away center-out
 * through fbm noise as progress runs 0 -> 1.  Fully data-driven:
 *   u_postfxParams[0].x = progress   (0 = fully covered, 1 = fully revealed)
 *   u_postfxParams[0].y = noise scale     (default-ish 3.0)
 *   u_postfxParams[1].xyz = fill color    (linear)
 * Anything beyond progress >= 1 is a pass-through, so leaving the pass
 * enabled at progress 1 renders the scene untouched.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);

uniform vec4 u_postfxParams[8];

float rv_hash12(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float rv_vnoise2(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    vec2 u = f * f * (3.0 - 2.0 * f);
    float a = rv_hash12(i);
    float b = rv_hash12(i + vec2(1.0, 0.0));
    float c = rv_hash12(i + vec2(0.0, 1.0));
    float d = rv_hash12(i + vec2(1.0, 1.0));
    return mix(mix(a, b, u.x), mix(c, d, u.x), u.y);
}

float rv_fbm(vec2 p)
{
    /* 5 octaves: the extra high-frequency detail breaks up the coarse
     * lattice cells that otherwise read as rectangular "blocks" at the
     * reveal boundary. */
    float v = 0.0;
    float a = 0.5;
    for (int i = 0; i < 5; i++) {
        v += a * rv_vnoise2(p);
        p = p * 2.03 + vec2(17.13, 9.71);
        a *= 0.5;
    }
    return v;
}

void main()
{
    vec3 scene = texture2D(s_texColor, v_texcoord0).rgb;

    float progress = u_postfxParams[0].x;
    float nscale   = max(u_postfxParams[0].y, 0.001);
    vec3  fill     = u_postfxParams[1].xyz;

    /* Aspect ratio of the target: WITHOUT this both the reveal circle and
     * the noise lattice are sampled in [0,1]x[0,1] and stretched by the
     * (16:9) screen into wide rectangular cells — the "分块" blocks the
     * user saw.  u_viewRect.zw is the postfx pass's pixel size. */
    float aspect = (u_viewRect.w > 0.5) ? (u_viewRect.z / u_viewRect.w) : 1.0;
    vec2  ar     = vec2(aspect, 1.0);

    /* organic center-out wipe: radial distance pushed by fbm noise, both
     * measured in aspect-corrected (square-pixel) space so cells stay round */
    vec2  c    = (v_texcoord0 - vec2(0.5, 0.5)) * ar;
    float dist = length(c) * 2.0 / max(aspect, 1.0);
    float n    = rv_fbm(v_texcoord0 * ar * nscale);
    float reveal = progress * 1.2 + n * 0.4 - dist * 0.6;
    float mask   = smoothstep(0.30, 0.70, reveal);   /* 0 = fill, 1 = scene */

    gl_FragColor = vec4(mix(fill, scene, mask), 1.0);
}
