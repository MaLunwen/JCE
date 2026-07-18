$input a_position, a_normal, a_texcoord0, i_data0, i_data1
$output v_texcoord0, v_normal, v_worldpos

#include <bgfx_shader.sh>

/*
 * vs_foliage_shadow.sc -- foliage-cluster card positioning for the CSM
 * depth pass (paired with fs_foliage_shadow.sc).
 *
 * NOT the color-pass vs_foliage: that one billboards cards against
 * u_invView, which in a cascade view is the LIGHT's inverse view — every
 * card in a cluster turns to face the light coherently and their union
 * casts one solid straight band across the ground at grazing light angles
 * (the "diagonal shadow stripe" bug).  Here each card instead lies TANGENT
 * to its shell (basis built from the per-card outward normal, i_data1.xyz):
 * deterministic per card like the reference's fixed leaf rotations, so the
 * cluster's shadow is a broken, dappled canopy silhouette from any light
 * direction.
 *
 * The wind displacement matches vs_foliage's (same seed, same octaves) so
 * leaf flutter still animates the shadows in step with the color pass.
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
    vec3  n      = normalize(i_data1.xyz);

    /* Shell-tangent basis: card plane perpendicular to the outward normal.
     * Guard the cross() against a near-vertical normal. */
    vec3 refUp   = (abs(n.y) > 0.95) ? vec3(1.0, 0.0, 0.0)
                                     : vec3(0.0, 1.0, 0.0);
    vec3 basisR  = normalize(cross(refUp, n));
    vec3 basisU  = cross(n, basisR);

    /* PER-CARD random spin around the normal (wind phase doubles as the
     * angle seed, uniform in [0,2pi)).  Without it every card on the
     * cluster's light-silhouette shares one tangent orientation: those
     * edge-on cards project coherent LONG thin strips that resolve into
     * near-black parallel bands on the grass once the camera is close
     * enough for cascade 0 to sharpen them.  Random spin (the reference's
     * fixed random leaf rotations) breaks the strips into dapple. */
    float ca = cos(i_data1.w), sa = sin(i_data1.w);
    vec3 r2  = basisR * ca + basisU * sa;
    basisU   = basisU * ca - basisR * sa;
    basisR   = r2;

    float heightMask = clamp(a_position.y + 0.5, 0.0, 1.0);
    heightMask = pow(heightMask, 1.5);
    /* MUST match vs_foliage.sc's wind VALUE exactly (same continuous world-XZ
     * seed, scales 6.2/2.15, speeds 0.975/0.2412, weights 2.5/0.5) or the leaf
     * shadow flutters out of step with the leaf — the old 1.4/0.9 scale + 2.6/1.0
     * speed ran ~2.7x too fast.  Offset rides the shadow's shell-tangent basis
     * (not the camera) so the cast canopy stays dappled. */
    vec2 wbase  = center.xz - floor(center.xz * (1.0 / 64.0)) * 64.0;
    vec2 wpos_n = wbase + a_position.xz;
    float breeze = fol_noise(wpos_n * 6.2  + t * 0.975)  - 0.5;
    float squall = fol_noise(wpos_n * 2.15 + t * 0.2412) - 0.5;
    float wind = (breeze * 2.5 + squall * 0.5) * heightMask;
    vec3 windOffset = basisR * wind * 0.25 + basisU * wind * 0.1;

    vec3 world = center
               + basisR * (a_position.x * size)
               + basisU * (a_position.y * size)
               + windOffset;

    gl_Position = mul(u_viewProj, vec4(world, 1.0));
    v_texcoord0 = a_texcoord0.xy;
    v_normal    = n;
    v_worldpos  = world;
}
