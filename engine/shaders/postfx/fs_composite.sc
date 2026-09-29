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

SAMPLER2D(s_texColor,  0);  /* scene color (HDR) */
SAMPLER2D(s_texBloom,  1);  /* blurred bloom     */
SAMPLER3D(s_texLUT,    2);  /* 3D colour-grading LUT (N×N×N RGBA8) */
SAMPLER2D(s_texDepth,  4);  /* scene depth, for depth of field.
                             * Stage 4: 0 colour, 1 bloom, 2 the 3D grade
                             * LUT, 3 motion vectors are all taken.
                             * The CoC below never LINEARISES it: the four
                             * depth edges arrive already projected by the CPU
                             * through the same matrix that produced this
                             * buffer, so reverse-Z and the GL/D3D clip-range
                             * difference cannot make this shader wrong. */
SAMPLER2D(s_texMotion, 3);  /* RG motion vectors, same encoding as fs_motion_vec.sc.
                             * ALWAYS BOUND, even with motion blur off: a dangling
                             * sampler makes WebGL2 reject the whole draw, which is
                             * the defect the 3D LUT placeholder above exists for. */

uniform vec4 u_compositeFlags;   /* x=bloom y=tonemap z=chromatic w=vignette */
uniform vec4 u_compositeFlags2;  /* x=grayscale y=motionBlur z=dof w=dofMaxCoc */
/* Depth of field, in STORED depth units, sorted numerically low..high:
 *   x,y = the in-focus band            (CoC 0 between them)
 *   z,w = where the blur reaches full  (z <= x, w >= y)
 * Sorted rather than named near/far ON PURPOSE: which numeric end is nearer
 * depends on the depth convention, and CoC only depends on being OUTSIDE the
 * band -- so this shader needs no per-backend branch at all. */
uniform vec4 u_dofParams;
uniform vec4 u_motionBlurParams; /* x=strength y=maxUvLen z=halfTexelUv w=0  */
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

/* The scene sample, with per-pixel motion blur folded in.
 *
 * WHY HERE AND NOT AS ITS OWN PASS.  It costs no extra view and no extra
 * render target, and it lands on the HDR colour BEFORE the bloom add and the
 * tonemap -- which is the ordering that makes a smeared highlight bloom as a
 * streak instead of blooming first and then being smeared into a band.
 *
 * The vector is decoded EXACTLY as fs_motion_vec.sc encoded it:
 *   mv = (cur_ndc - prev_ndc) * 0.5 + 0.5   =>   d_ndc = (mv - 0.5) * 2
 * and that producer builds its ndc as `v_texcoord0 * 2 - 1`, i.e. it treats
 * uv as ndc with NO Y flip.  So the inverse is d_uv = d_ndc * 0.5, also with
 * no flip.  Agreeing with the producer is the whole job: this repository has
 * shipped a mirrored `uv*2-1` twelve times in one campaign, and every one of
 * them looked plausible on its own.
 *
 * Taps are FIXED at 8 rather than driven by a uniform loop bound: the desktop
 * GL floor is 3.1 and the GLSL that reaches it goes through glsl-optimizer, so
 * a constant trip count is the portable one.  Strength lengthens the trail
 * instead. */
#define MB_TAPS 8

/* Circle of confusion at `uv`, 0 (sharp) .. 1 (fully blurred).
 *
 * NO LINEARISATION, and that is the point: the four edges arrive already
 * projected by the CPU through the same matrix that filled this depth buffer,
 * and they arrive SORTED numerically rather than named near/far.  Which
 * numeric end is nearer depends on the depth convention (reverse-Z, and GL's
 * [-1,1] clip range against D3D's [0,1]); being OUTSIDE the in-focus band
 * does not.  So this function has no per-backend branch, in a repository that
 * has shipped twelve mirrored ones in a single campaign. */
float dofCoc(vec2 uv)
{
    float d = texture2D(s_texDepth, uv).r;
    if (d < u_dofParams.x)
        return clamp((u_dofParams.x - d)
                   / max(u_dofParams.x - u_dofParams.z, 1e-6), 0.0, 1.0);
    if (d > u_dofParams.y)
        return clamp((d - u_dofParams.y)
                   / max(u_dofParams.w - u_dofParams.y, 1e-6), 0.0, 1.0);
    return 0.0;
}

/* A 16-tap golden-angle disk.  Constant, so the loop bound is constant --
 * the GLSL-120 rule this tree's shaders are written to. */
#define DOF_TAPS 16

vec3 sceneSample(vec2 uv)
{
    if (u_compositeFlags2.y > 0.5)
    {
        vec2 mv = texture2D(s_texMotion, uv).rg * 2.0 - 1.0;
        /* NDC delta -> UV delta, WITH the per-backend Y branch.  On GL
         * (bottom-left origin, vs_postfx already flipped v) duv = +dndc/2; on
         * D3D/VK/Metal uv.y = (1-ndc.y)/2 so duv.y = -dndc.y/2.  Copied from
         * fs_taa.sc rather than re-derived, and it stays in lockstep with the
         * two writers (fs_motion_vec.sc, fs_gbuffer_vel.sc) that both emit a
         * TRUE clip-NDC delta.  Unconditional, it is right on GL and inverted
         * on D3D -- which smears vertically instead of along the motion, and
         * this repository has shipped that exact mirror twelve times in one
         * campaign because every one of them looks plausible alone. */
#if BGFX_SHADER_LANGUAGE_GLSL
        vec2 v = mv * 0.5;
#else
        vec2 v = vec2(mv.x, -mv.y) * 0.5;
#endif
        v *= u_motionBlurParams.x;

        /* Clamp the trail. Without it a camera cut -- or the first frame after
         * a teleport, where the previous camera has nothing to do with this
         * one -- samples right across the screen and the frame turns to soup.
         * A cut is exactly when the motion vector is both huge and meaningless. */
        float len = length(v);
        if (len > u_motionBlurParams.y)
            v *= u_motionBlurParams.y / max(len, 1e-6);

        /* Sub-pixel motion must be a no-op, not a slightly softer image.
         *
         * THE THRESHOLD IS HALF A TEXEL, passed in, not a bare constant.  A
         * still camera still produces a little numerical drift through the
         * depth reconstruction and the inverse view-proj, and a fixed 1e-4 is
         * 0.13 px at 1280 wide and 0.05 px at 3840 -- so the same constant
         * means different things on different monitors.  Measured: with the
         * bare constant, a STATIC frame moved 33.3% of its pixels (>4 at
         * 3.09%) against a 0.38% / 0.00% capture noise floor, and the image
         * std fell 55.69 -> 55.49.  That is a permanent, unexplained softness
         * that reads as "the tonemap is wrong", on a frame where nothing is
         * moving.  Below half a texel every tap lands in the same texel and
         * the only thing the loop can add is bilinear mush. */
        if (len > u_motionBlurParams.z)
        {
            vec3 acc = vec3_splat(0.0);
            for (int i = 0; i < MB_TAPS; ++i)
            {
                /* Backwards along the trail: the vector points current ->
                 * previous, so the smear belongs BEHIND the moving pixel. */
                float t = float(i) / float(MB_TAPS - 1);
                acc += texture2D(s_texColor, uv - v * t).rgb;
            }
            return acc / float(MB_TAPS);
        }
    }
    if (u_compositeFlags2.z > 0.5)
    {
        float coc = dofCoc(uv);
        /* Sub-texel radius is a NO-OP, not a slightly softer image -- the same
         * rule motion blur above needed, learnt the same way: a threshold of
         * zero leaves an in-focus frame measurably blurred by bilinear mush
         * from 16 taps that all land in the same texel. */
        float r = coc * u_compositeFlags2.w;
        if (r > u_motionBlurParams.z)
        {
            vec3  acc  = vec3_splat(0.0);
            float wsum = 0.0;
            for (int i = 0; i < DOF_TAPS; ++i)
            {
                /* Golden-angle spiral: even coverage without a lookup table,
                 * and no two taps land on the same ring the way a regular
                 * grid's do. */
                float fi = float(i) + 0.5;
                float a  = fi * 2.39996323;
                float rr = sqrt(fi / float(DOF_TAPS)) * r;
                vec2  o  = vec2(cos(a), sin(a)) * rr;
                /* Weighted by the SAMPLE's own CoC: a sharp foreground pixel
                 * must not bleed into a blurred background one, which is the
                 * classic gather-DoF halo.  +0.05 keeps a fully sharp
                 * neighbourhood from dividing by zero. */
                float wgt = dofCoc(uv + o) + 0.05;
                acc += texture2D(s_texColor, uv + o).rgb * wgt;
                wsum += wgt;
            }
            return acc / max(wsum, 1e-6);
        }
    }
    return texture2D(s_texColor, uv).rgb;
}

/* scene (+ optional bloom) then optional tonemap, evaluated at one uv. */
vec3 combineTonemap(vec2 uv)
{
    vec3 c = sceneSample(uv);
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
