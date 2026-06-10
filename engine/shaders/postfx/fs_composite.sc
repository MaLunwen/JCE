/*
 * fs_composite.sc  Uber post-FX composite.
 *
 * Folds bloom-combine, ACES tonemap, chromatic aberration, vignette and
 * grayscale into ONE fullscreen pass (each gated by a flag), so the post
 * chain costs a single view / RT-switch instead of up to five. FXAA stays a
 * separate trailing pass because it needs the composited LDR image as input.
 *
 * Stage order mirrors the original chain: combine -> tonemap, then chromatic
 * sampling the tonemapped result, then vignette, then grayscale.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);   /* scene color (HDR) */
SAMPLER2D(s_texBloom, 1);   /* blurred bloom     */

uniform vec4 u_compositeFlags;   /* x=bloom y=tonemap z=chromatic w=vignette */
uniform vec4 u_compositeFlags2;  /* x=grayscale                              */
uniform vec4 u_bloomParams;      /* y=intensity                              */
uniform vec4 u_tonemapParams;    /* x=exposure y=gamma                       */
uniform vec4 u_chromaticParams;  /* x=strength                               */
uniform vec4 u_vignetteParams;   /* x=intensity y=smoothness                 */

vec3 acesFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

/* scene (+ optional bloom) then optional tonemap, evaluated at one uv. */
vec3 combineTonemap(vec2 uv)
{
    vec3 c = texture2D(s_texColor, uv).rgb;
    if (u_compositeFlags.x > 0.5)
        c += texture2D(s_texBloom, uv).rgb * u_bloomParams.y;
    if (u_compositeFlags.y > 0.5)
    {
        vec3 mapped = acesFilm(c * u_tonemapParams.x);
        c = pow(mapped, vec3_splat(1.0 / u_tonemapParams.y));
    }
    return c;
}

void main()
{
    vec2 uv = v_texcoord0;
    vec3 col;

    if (u_compositeFlags.z > 0.5)
    {
        /* chromatic aberration: per-channel offset on the composited image */
        vec2 off = (uv - vec2_splat(0.5)) * u_chromaticParams.x;
        col.x = combineTonemap(uv + off).x;
        col.y = combineTonemap(uv).y;
        col.z = combineTonemap(uv - off).z;
    }
    else
    {
        col = combineTonemap(uv);
    }

    if (u_compositeFlags.w > 0.5)
    {
        vec2 vuv = uv * 2.0 - 1.0;
        float dist = dot(vuv, vuv);
        float vig = 1.0 - smoothstep(1.0 - u_vignetteParams.y, 1.0,
                                     dist * u_vignetteParams.x);
        col *= vig;
    }

    if (u_compositeFlags2.x > 0.5)
    {
        float g = dot(col, vec3(0.2126, 0.7152, 0.0722));
        col = vec3_splat(g);
    }

    /* Dither: break up 8-bit banding of smooth tonemapped gradients — when
       this pass tonemaps it IS the final quantization point, so the
       concentric light-falloff "ring" bands under spot/point lights are
       removed here (the HDR scene target keeps the gradient smooth up to this
       pass; the ~1/255 screen-space noise hides the last step). Gated on the
       tonemap flag: bloom/vignette-only chains run on LDR input the PBR pass
       already gamma-encoded AND dithered (fs_pbr.sc) — dithering again would
       double the noise. Exactly one dither fires on every path. */
    if (u_compositeFlags.y > 0.5)
    {
        float _cdither = fract(sin(dot(gl_FragCoord.xy, vec2(12.9898, 78.233)))
                               * 43758.5453);
        col += vec3_splat((_cdither - 0.5) / 255.0);
    }

    gl_FragColor = vec4(col, 1.0);
}
