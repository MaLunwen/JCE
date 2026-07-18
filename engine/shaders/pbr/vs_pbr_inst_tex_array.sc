$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3, i_data4
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos, v_tint

#include <bgfx_shader.sh>

/* Texture-diverse instanced PBR vertex shader (texture-array batcher).
 *
 * Like vs_pbr_inst, but the 5th per-instance attribute (i_data4.x) carries the
 * albedo 2D-array LAYER index.  It is forwarded through the existing v_tint
 * varying's .x lane (the array variant does not tint — baseColor is a shared
 * uniform for the batch — so v_tint is repurposed as the layer carrier, which
 * avoids adding a new varying).  The matching fragment program
 * fs_pbr_inst_tex_array samples texture2DArray(s_albedo, vec3(uv, v_tint.x)),
 * so copies of one mesh that differ only by their albedo TEXTURE collapse into
 * one instanced submit — the gap the (mesh,tint) batcher cannot close.
 *
 * Instance buffer = 80 bytes (5 vec4): mat4 model (i_data0..3) + i_data4.x = layer. */

void main()
{
    /* mtxFromCols handles HLSL row-major vs GLSL column-major correctly. */
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
    /* .x = albedo array layer; other lanes unused by the array fs. */
    v_tint      = vec4(i_data4.x, 0.0, 0.0, 0.0);
}
