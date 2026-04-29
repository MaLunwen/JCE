$input a_position, i_data0, i_data1, i_data2, i_data3
$output v_texcoord0

#include <bgfx_shader.sh>

/* Instanced shadow vertex shader.
 *
 * Per-instance model matrix from i_data0..3 (4 vec4 columns).  Otherwise
 * identical to vs_shadow.sc — depth-only output, fragment side reuses
 * fs_shadow unchanged. */

void main()
{
    mat4 model  = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
    vec3 wpos   = mul(model, vec4(a_position, 1.0)).xyz;
    gl_Position = mul(u_viewProj, vec4(wpos, 1.0));
    v_texcoord0 = vec2(0.0, 0.0);
}
