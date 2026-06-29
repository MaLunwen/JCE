$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/* Tint-aware instanced PBR vertex shader (large-world-opt P1 #7).
 *
 * Identical to vs_pbr_inst.sc EXCEPT it reads a 5th per-instance attribute
 * (i_data4 = the per-instance RGBA tint) and forwards it as v_tint.  The
 * matching fragment program fs_pbr_tint multiplies albedo by v_tint, so a
 * tinted instanced copy looks bit-identical to the same mesh drawn solo with
 * that colour set as u_baseColorFactor — letting per-entity baseColor copies
 * COLLAPSE into one instanced submit instead of falling back to solo draws.
 *
 * The instance buffer is 80 bytes (5 vec4): the mat4 model (i_data0..3) then
 * the tint (i_data4).  Models with no per-entity tint keep using the stride-64
 * vs_pbr_inst path (this shader is only bound for tinted runs). */

void main()
{
    /* mtxFromCols handles HLSL row-major vs GLSL column-major correctly — see
     * vs_pbr_inst.sc for the DXBC input-signature rationale. */
    mat4 model = mtxFromCols(i_data0, i_data1, i_data2, i_data3);

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
    v_tint      = i_data4;
}
