$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/*
 * vs_grass.sc -- GPU-instanced procedural grass blade with vertex wind.
 * Stage 1b.6.  Standalone (paired with fs_grass.sc, NOT fs_pbr_body.sh).
 *
 * Instance buffer = stride-80 tinted layout (vs_pbr_inst_tint twin):
 *   i_data0..3 = per-blade model mat4 (TRS from jce_foliage_scatter)
 *   i_data4    = per-blade RGBA tint (hue jitter baked CPU-side)
 * a_texcoord0.y = 0(root)→1(tip) bend/gradient parameter; .x = card U.
 *
 * Wind clock mirrors vs_water's u_water_time: phase = dot(world_xz,dir)*freq +
 * u_grass_time.x*speed; the tip sways, the root is pinned (bend ∝ uv.y^2).
 * Distance collapse scales blade height to 0 between fade_start..fade_end so
 * far blades cost no overdraw (cheaper than CPU per-blade frustum cull).
 * NO raw mat3()/mat ctor (GL TBN bug) — mtxFromCols + vec math only.
 */
uniform vec4 u_grass_time;   // x=time
uniform vec4 u_grass_wind;   // xy=dir, z=speed, w=amplitude
uniform vec4 u_grass_fade;   // x=fade_start, y=fade_end, z=hue_jitter
uniform vec4 u_cameraPos;    // xyz=world camera position (PBR global bind)

void main()
{
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);

    // World-space blade root + this vertex (un-bent).
    vec3 wpos = mul(model, vec4(a_position, 1.0)).xyz;
    vec3 root = mul(model, vec4(0.0, 0.0, 0.0, 1.0)).xyz;

    float bend = a_texcoord0.y;            // 0 at root, 1 at tip
    float bend2 = bend * bend;             // root-pinned curve

    // Distance height-collapse: shrink toward the root past fade_start.
    float dist = length(u_cameraPos.xyz - root);
    float fadeT = clamp((dist - u_grass_fade.x) /
                        max(u_grass_fade.y - u_grass_fade.x, 1e-3), 0.0, 1.0);
    float heightScale = 1.0 - fadeT;       // 1 near -> 0 far
    wpos = root + (wpos - root) * heightScale;

    // Wind sway (world XZ), tip-weighted, phase from world position.
    vec2 wdir = u_grass_wind.xy;
    float wl2 = dot(wdir, wdir);
    wdir = (wl2 > 1e-8) ? (wdir * inversesqrt(wl2)) : vec2(1.0, 0.0);
    float phase = dot(root.xz, wdir) * 0.35 + u_grass_time.x * u_grass_wind.z;
    float sway  = sin(phase) * u_grass_wind.w * bend2 * heightScale;
    wpos.xz += wdir * sway;

    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position  = mul(u_proj, viewPos);

    v_normal    = normalize(mul(model, vec4(a_normal, 0.0)).xyz);
    v_tangent   = vec3(1.0, 0.0, 0.0);
    v_bitangent = vec3(0.0, 0.0, 1.0);
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = a_position;
    v_viewdepth = -viewPos.z;
    v_tint      = i_data4;
}
