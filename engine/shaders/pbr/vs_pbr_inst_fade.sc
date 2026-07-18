$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/* Instanced PBR vertex shader with LOD cross-fade (千万 ②).
 *
 * Identical to vs_pbr_inst.sc EXCEPT the per-instance matrix's last column w
 * lane smuggles the cull's band-transition coverage:
 *   i_data3.w = +f  -> primary band copy, keep dithered pixels where ign <  f
 *   i_data3.w = -f  -> next-band complement, keep pixels where ign >= f
 *   i_data3.w =  1  -> solid (the overwhelmingly common fast path)
 * The matrix itself is reconstructed with w = 1.0; the coverage rides to the
 * fragment side (fs_pbr_fade) in v_tint.a (v_tint.rgb stays 1 = no tint). */

void main()
{
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2,
                             vec4(i_data3.xyz, 1.0));

    vec3 wpos    = mul(model, vec4(a_position, 1.0)).xyz;
    vec4 viewPos = mul(u_view, vec4(wpos, 1.0));
    gl_Position  = mul(u_proj, viewPos);

    v_normal    = normalize(mul(model, vec4(a_normal, 0.0)).xyz);
    v_tangent   = normalize(mul(model, vec4(a_tangent.xyz, 0.0)).xyz);
    v_bitangent = cross(v_normal, v_tangent) * a_tangent.w;
    v_texcoord0 = a_texcoord0;
    v_worldpos  = wpos;
    v_localpos  = a_position;
    v_viewdepth = -viewPos.z;
    v_tint      = vec4(1.0, 1.0, 1.0, i_data3.w);
}
