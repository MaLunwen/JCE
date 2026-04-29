$input a_position, a_normal, a_tangent, a_texcoord0, i_data0, i_data1, i_data2, i_data3
$output v_texcoord0, v_worldpos, v_normal, v_tangent, v_bitangent, v_viewdepth, v_localpos

#include <bgfx_shader.sh>

/* Instanced PBR vertex shader.
 *
 * The model matrix comes from the per-instance buffer (4 vec4 columns
 * packed into i_data0..3) instead of the u_model[] uniform.  Otherwise
 * identical to vs_pbr.sc — fragment side reuses fs_pbr unchanged. */

void main()
{
    /* Use bgfx_shader.sh's mtxFromCols helper — handles HLSL row-major
     * vs GLSL column-major matrix conventions correctly.  Direct row
     * assignment (model[0] = i_dataN) was producing inconsistent DXBC
     * input signatures on D3D12, leading to PSO creation failure. */
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
}
