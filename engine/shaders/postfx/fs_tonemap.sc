/*
 * fs_tonemap.sc  Selectable tone mapping + gamma correction.
 *
 * Supports ACES (op=0), Khronos PBR-Neutral (op=1), and AgX (op=2) via
 * u_tonemapParams.z.  Default op=0 is ACES (byte-identical to the old
 * single-operator version when z=0).
 */

$input v_texcoord0

#include <bgfx_shader.sh>

SAMPLER2D(s_texColor, 0);
uniform vec4 u_tonemapParams;   /* x=exposure, y=gamma, z=tonemapOp(0/1/2), w=unused */

vec3 aces_film(vec3 x)
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
    return aces_film(c);
}

void main()
{
    vec3 hdr = texture2D(s_texColor, v_texcoord0).rgb;
    float exposure = u_tonemapParams.x;
    float gamma    = u_tonemapParams.y;

    vec3 mapped = selectTonemap(hdr * exposure);
    vec3 corrected = pow(mapped, vec3_splat(1.0 / gamma));

    gl_FragColor = vec4(corrected, 1.0);
}
