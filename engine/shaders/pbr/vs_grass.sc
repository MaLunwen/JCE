$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/*
 * vs_grass.sc -- GPU-instanced grass blade, CAMERA-BILLBOARDED + vertex wind.
 * Standalone (paired with fs_grass.sc, NOT fs_pbr_body.sh).
 *
 * Reference-faithful (Elemental-Serenity GrassManager): each blade's local
 * XZ is rotated around its root by the yaw TO the camera so the flat blade
 * always faces the viewer -- which is what reads as a dense lush tuft
 * instead of edge-on slivers at grazing angles.  Only translation + uniform
 * scale are taken from the instance matrix; the blade's own yaw is REPLACED
 * by the camera-facing yaw.
 *
 * Instance buffer = stride-80 tinted layout:
 *   i_data0..3 = per-blade model mat4 (TRS from jce_foliage_scatter)
 *   i_data4    = per-blade RGBA tint (hue jitter baked CPU-side)
 * a_texcoord0.y = 0(root)->1(tip) bend/gradient parameter; .x = card U.
 * NO raw mat3()/mat ctor (GL TBN bug) -- vec math only.
 */
uniform vec4 u_grass_time;   // x=time
uniform vec4 u_grass_wind;   // xy=dir, z=speed, w=amplitude
uniform vec4 u_grass_fade;   // x=fade_start, y=fade_end, z=hue_jitter
uniform vec4 u_cameraPos;    // xyz=world camera position (PBR global bind)

/* Reference GrassManager value-noise (hash 12.9898/78.233).  Its "fbm" loops
 * i<1 -> a single octave, i.e. 0.5 * smoothNoise2D; no real multi-octave. */
float gr_hash(vec2 p)
{
    return fract(sin(dot(p, vec2(12.9898, 78.233))) * 43758.5453);
}

float gr_noise(vec2 p)
{
    vec2 i = floor(p);
    vec2 f = fract(p);
    f = f * f * (3.0 - 2.0 * f);
    float a = gr_hash(i);
    float b = gr_hash(i + vec2(1.0, 0.0));
    float c = gr_hash(i + vec2(0.0, 1.0));
    float d = gr_hash(i + vec2(1.0, 1.0));
    return mix(mix(a, b, f.x), mix(c, d, f.x), f.y);
}

void main()
{
    // Extract translation (root) + uniform scale from the instance matrix.
    vec3 root = i_data3.xyz;
    float sc  = length(i_data0.xyz);
    if (sc < 1e-5) sc = 1.0;

    float bend  = a_texcoord0.y;       // 0 at root, 1 at tip
    float bend2 = bend * bend;         // root-pinned curve

    // Camera-facing yaw (ES angleToCamera): rotate the blade's XZ so its
    // face points at the camera.  Y (height) is untouched.
    float ang = atan2(root.z - u_cameraPos.z, root.x - u_cameraPos.x)
              - 1.5707963267948966;
    float sa = sin(ang), ca = cos(ang);
    vec3 lp = a_position * sc;
    vec3 bp = vec3(lp.x * ca - lp.z * sa, lp.y, lp.x * sa + lp.z * ca);
    vec3 wpos = root + bp;

    // Distance height-collapse: shrink toward the root past fade_start.
    float dist  = length(u_cameraPos.xyz - root);
    float fadeT = clamp((dist - u_grass_fade.x) /
                        max(u_grass_fade.y - u_grass_fade.x, 1e-3), 0.0, 1.0);
    float heightScale = 1.0 - fadeT;
    wpos = root + (wpos - root) * heightScale;

    // Directional wind WAVE — the "wind blowing through" look: a long travelling
    // wave that sweeps along the authored wind direction.  Blades on a line
    // PERPENDICULAR to the wind share one phase (they bend in UNISON), and the
    // crest travels across the field over time — a coherent gust sheet, NOT
    // per-blade noise.  The whole blade bends (length-preserving rotation about a
    // FIXED axis perpendicular to the wind) by a SIGNED angle growing with
    // height^2, so it arcs forward/back like real grass; the fixed axis means it
    // never spins.  A slow large-scale gust envelope makes the wave breathe.
    // u_grass_wind.z = speed, .w = amplitude (radians of tip bend).  A subtle
    // per-region phase jitter keeps it from reading perfectly mechanical.
    vec2  wdir = u_grass_wind.xy;
    float wl2  = dot(wdir, wdir);
    wdir = (wl2 > 1e-8) ? (wdir * inversesqrt(wl2)) : vec2(1.0, 0.0);
    float spd   = u_grass_wind.z;
    float along = dot(root.xz, wdir);                       // distance along the wind
    float jit   = (gr_noise(root.xz * 0.12) - 0.5) * 0.9;   // gentle spatial phase jitter
    float phase = along * 0.30 - u_grass_time.x * spd + jit;
    float gust  = 0.65 + 0.35 * sin(u_grass_time.x * spd * 0.21 - along * 0.09);
    float sway  = (sin(phase) + 0.22 * sin(phase * 2.7 + 1.3)) * gust;
    float bendAng = sway * u_grass_wind.w * bend2;          // signed; tip bends most
    vec3  axis  = normalize(cross(vec3(wdir.x, 0.0, wdir.y), vec3(0.0, 1.0, 0.0)));
    float cs = cos(bendAng), sn = sin(bendAng);
    vec3  off = wpos - root;                                // faded blade offset from root
    off = off * cs + cross(axis, off) * sn + axis * dot(axis, off) * (1.0 - cs);
    wpos = root + off;

    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position  = mul(u_proj, viewPos);

    // Camera-facing-ish normal for the toon ramp (up-biased).
    vec3 toCam = normalize(u_cameraPos.xyz - wpos);
    v_normal    = normalize(vec3(toCam.x * 0.4, 0.85, toCam.z * 0.4));
    v_tangent   = vec3(1.0, 0.0, 0.0);
    v_bitangent = vec3(0.0, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = a_position;
    v_viewdepth = -viewPos.z;
    v_tint      = i_data4;
}
