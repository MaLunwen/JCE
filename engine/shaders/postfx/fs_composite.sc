/*
 * fs_composite.sc  Uber post-FX composite.
 *
 * Folds bloom-combine, ACES/Neutral/AgX tonemap, chromatic aberration,
 * vignette, grayscale, and optional 3D-LUT colour grade into ONE fullscreen
 * pass (each gated by a flag), so the post chain costs a single view /
 * RT-switch instead of up to six. FXAA stays a separate trailing pass because
 * it needs the composited LDR image as input.
 *
 * Stage order: combine -> tonemap -> chromatic (on tonemapped) -> vignette ->
 * grayscale -> 3D-LUT grade -> dither.
 *
 * LUT grade note: applied on LDR (post-tonemap, post-gamma) colour so the
 * grade sees the same 0..1 range as a look artist would in a grading tool.
 * The dither fires LAST (after grade) so the grade is dithered too; exactly
 * one dither fires on every tonemapped path.  Per-fragment PBR look items
 * (albedo, normals, lights) are unaffected by this pass.
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);   /* scene color (HDR) */
SAMPLER2D(s_texBloom, 1);   /* blurred bloom     */
SAMPLER3D(s_texLUT,   2);   /* 3D colour-grading LUT (N×N×N RGBA8) */

uniform vec4 u_compositeFlags;   /* x=bloom y=tonemap z=chromatic w=vignette */
uniform vec4 u_compositeFlags2;  /* x=grayscale                              */
uniform vec4 u_bloomParams;      /* y=intensity                              */
uniform vec4 u_tonemapParams;    /* x=exposure y=gamma z=tonemapOp(0/1/2)   */
uniform vec4 u_chromaticParams;  /* x=strength                               */
uniform vec4 u_vignetteParams;   /* x=intensity y=smoothness                 */
uniform vec4 u_gradeParams;      /* x=enabled y=strength z=N(LUT edge) w=0  */

vec3 acesFilm(vec3 x)
{
    float a = 2.51;
    float b = 0.03;
    float c = 2.43;
    float d = 0.59;
    float e = 0.14;
    return clamp((x * (a * x + b)) / (x * (c * x + d) + e), 0.0, 1.0);
}

/* Khronos PBR-neutral tonemapper. */
vec3 neutralTonemap(vec3 c)
{
    float startCompression = 0.8 - 0.04;
    float desaturation = 0.15;
    float x = min(c.r, min(c.g, c.b));
    float offset = x < 0.08 ? x - 6.25 * x * x : 0.04;
    c -= offset;
    float peak = max(c.r, max(c.g, c.b));
    if (peak < startCompression) return c;
    float d = 1.0 - startCompression;
    float newPeak = 1.0 - d * d / (peak + d - startCompression);
    c *= newPeak / peak;
    float g = 1.0 - 1.0 / (desaturation * (peak - newPeak) + 1.0);
    return mix(c, vec3_splat(newPeak), g);
}

/* AgX (Troy Sobotka, minimal). 3x3 matrices via mtxFromCols (GL-safe, no raw mat3 ctor). */
vec3 agxTonemap(vec3 val)
{
    mat3 agx_in = mtxFromCols(
        vec3(0.842479062253094,  0.0423282422610123, 0.0423756549057051),
        vec3(0.0784335999999992, 0.878468636469772,  0.0784336),
        vec3(0.0792237451477643, 0.0791661274605434, 0.879142973793104));
    mat3 agx_out = mtxFromCols(
        vec3( 1.19687900512017,  -0.0528968517574562, -0.0529716355144438),
        vec3(-0.0980208811401368, 1.15190312990417,   -0.0980434501171241),
        vec3(-0.0990297440797205,-0.0989611768448433,  1.15107367264116));
    val = mul(agx_in, val);
    val = clamp(log2(max(val, vec3_splat(1e-10))), vec3_splat(-12.47393), vec3_splat(4.026069));
    val = (val + 12.47393) / (4.026069 + 12.47393);
    /* 6th-order sigmoid (default AgX look). */
    vec3 x2 = val * val; vec3 x4 = x2 * x2;
    val = 15.5 * x4 * x2 - 40.14 * x4 * val + 31.96 * x4 - 6.868 * x2 * val + 0.4298 * x2 + 0.1191 * val - 0.00232;
    val = mul(agx_out, val);
    return clamp(val, vec3_splat(0.0), vec3_splat(1.0));
}

/* Dispatch on u_tonemapParams.z via step (no == on floats). */
vec3 selectTonemap(vec3 c)
{
    float op = u_tonemapParams.z;
    if (op > 1.5)  return agxTonemap(c);
    if (op > 0.5)  return neutralTonemap(c);
    return acesFilm(c);
}

/* scene (+ optional bloom) then optional tonemap, evaluated at one uv. */
vec3 combineTonemap(vec2 uv)
{
    vec3 c = texture2D(s_texColor, uv).rgb;
    if (u_compositeFlags.x > 0.5)
        c += texture2D(s_texBloom, uv).rgb * u_bloomParams.y;
    if (u_compositeFlags.y > 0.5)
    {
        vec3 mapped = selectTonemap(c * u_tonemapParams.x);
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

    /* 3D-LUT colour grade (post-tonemap, post-gamma LDR; half-texel trilinear).
     * Algebraic no-op when u_gradeParams.x < 0.5 (disabled or no LUT handle).
     * The grade is inside the tonemap path only (showcased with tonemap on). */
    if (u_gradeParams.x > 0.5)
    {
        float N = u_gradeParams.z;
        vec3 cc = clamp(col, 0.0, 1.0);
        /* Half-texel scale/bias keeps endpoints inside texel centres. */
        vec3 uvw = cc * ((N - 1.0) / N) + (0.5 / N);
        vec3 graded = texture3D(s_texLUT, uvw).rgb;
        col = mix(col, graded, u_gradeParams.y);
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
