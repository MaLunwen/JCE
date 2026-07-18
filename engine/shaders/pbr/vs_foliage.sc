$input a_position, a_normal, a_texcoord0, i_data0, i_data1
$output v_texcoord0, v_normal, v_worldpos

#include <bgfx_shader.sh>

/*
 * vs_foliage.sc -- camera-facing billboard foliage card + vertex wind.
 *
 * Stylized-canopy foliage (paired with fs_foliage.sc, NOT fs_pbr_body):
 * one 1x1 quad instanced per leaf card.
 *   i_data0 = card center (world) .xyz, card size .w
 *   i_data1 = shell OUTWARD normal .xyz (toon shading key), wind phase .w
 * Wind: two value-noise octaves (breeze + squall) displace the card along
 * the camera axes, scaled by a height mask so card bottoms sway less.
 */

uniform vec4 u_foliage_time;   // x = time (seconds)

float fol_hash(vec2 p)
{
    return fract(sin(dot(p, vec2(127.1, 311.7))) * 43758.5453);
}

float fol_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = fol_hash(i);
    float b = fol_hash(i + vec2(1.0, 0.0));
    float c = fol_hash(i + vec2(0.0, 1.0));
    float d = fol_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main()
{
    vec3  center = i_data0.xyz;
    float size   = i_data0.w;
    float t      = u_foliage_time.x;

    /* Camera right/up in world space, for the view-facing billboard.
     * MUST use mul() — NOT raw element indexing like u_invView[0][0].  Raw
     * m[i][j] means column-i/row-j on GLSL but row-i/col-j on HLSL, so
     * `vec3(u_invView[0][0], u_invView[1][0], u_invView[2][0])` reads column 0
     * on D3D (correct) but row 0 — the TRANSPOSE — on OpenGL.  A transposed
     * basis skews every card, worst at diagonal (~45°) view angles, smearing
     * the leaves into long streaks on GL only.  mul(M, axis) extracts the
     * column consistently on every backend (same class of bug as the mat3 TBN
     * transpose that halved the light pools on GLSL). */
    vec3 cam_right = mul(u_invView, vec4(1.0, 0.0, 0.0, 0.0)).xyz;
    vec3 cam_up    = mul(u_invView, vec4(0.0, 1.0, 0.0, 0.0)).xyz;

    // Wind: breeze + squall noise, height-masked.  Rates/scales matched to
    // the reference's EFFECTIVE speeds: its uTime accumulates 0.001/frame
    // (~0.06/s at 60fps), so breezeSpeed 16.25 x 0.06 = 0.975/s and
    // squallSpeed 4.02 x 0.06 = 0.2412/s of phase in real seconds.
    float heightMask = clamp(a_position.y + 0.5, 0.0, 1.0);
    heightMask = pow(heightMask, 1.5);
    /* PER-CARD wind seed (NOT per-vertex), kept SMALL via fract().  The raw
     * seed center.xz + a_position.xz fed sin() (inside fol_hash) unbounded
     * world coordinates, and GL/GLES sin() loses precision at large arguments
     * where D3D's stays exact — a cross-backend divergence hazard that grows
     * with distance from the origin (large worlds) and with time.  A wrapped
     * per-card seed keeps sin()'s argument small on every backend AND makes
     * all 4 corners share one wind value, so a card sways as a rigid unit —
     * the per-vertex heightMask below still bends it (bottoms sway less).
     * Octave scales map the reference's spatial frequencies through the 0.13
     * seed wrap: 0.13*47.7 ~= breezeScale 6.2/unit, 0.13*16.5 ~= squall
     * 2.15/unit — neighbouring cards flutter independently like the
     * reference's per-leaf noise instead of the whole canopy heaving. */
    /* Reference bush wind (Bush.class vertex.glsl): sample a CONTINUOUS world-XZ
     * value-noise field so neighbouring cards sway TOGETHER in a travelling gust
     * (the reference's whole-hedge coherence), plus the per-vertex local xz so the
     * 4 corners of a card decorrelate into an intra-card shimmer.  The old
     * fract(center*0.13)+i_data1.w seed gave every card an INDEPENDENT random
     * phase (flutter, no gust waves) and seamed every ~7.7u — both killed the
     * reference feel.  We only wrap to a wide 64u tile so fol_hash's sin() stays
     * precise on large worlds; the tile dwarfs any clump so motion is continuous
     * within it.  Scales/speeds are the reference's EFFECTIVE 60fps rates: breeze
     * uBreezeScale 6.2 @ uBreezeSpeed 16.25*0.06=0.975/s, squall uSquallScale*0.5
     * =2.15 @ uSquallSpeed 4.02*0.06=0.2412/s. */
    vec2 wbase  = center.xz - floor(center.xz * (1.0 / 64.0)) * 64.0;
    vec2 wpos_n = wbase + a_position.xz;
    float breeze = fol_noise(wpos_n * 6.2  + t * 0.975)  - 0.5;
    float squall = fol_noise(wpos_n * 2.15 + t * 0.2412) - 0.5;
    float wind = (breeze * 2.5 + squall * 0.5) * heightMask;
    vec3 windOffset = cam_right * wind * 0.25 + cam_up * wind * 0.1;

    vec3 world = center
               + cam_right * (a_position.x * size)
               + cam_up    * (a_position.y * size)
               + windOffset;

    gl_Position = mul(u_viewProj, vec4(world, 1.0));
    v_texcoord0 = a_texcoord0.xy;
    v_normal    = i_data1.xyz;
    v_worldpos  = world;
}
