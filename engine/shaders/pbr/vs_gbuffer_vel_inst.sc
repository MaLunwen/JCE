$input a_position, a_normal, i_data0, i_data1, i_data2, i_data3
$output v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * vs_gbuffer_vel_inst.sc — GPU-instanced sibling of vs_gbuffer_vel.sc.
 *
 * Per-instance model matrix from i_data0..3 (4 vec4 columns), exactly like
 * vs_shadow_inst / vs_pbr_inst.  Instanceable objects are STATIC (no
 * per-instance motion this frame), so the PREVIOUS world matrix equals the
 * current one; the per-object motion comes purely from camera movement via
 * u_prevViewProj.  Fragment side reuses fs_gbuffer_vel unchanged.
 */
uniform mat4 u_curViewProj;
uniform mat4 u_prevViewProj;

void main()
{
	mat4 model    = mtxFromCols(i_data0, i_data1, i_data2, i_data3);
	vec4 curWorld = mul(model, vec4(a_position, 1.0));

	vec4 curClip  = mul(u_curViewProj,  curWorld);
	vec4 prevClip = mul(u_prevViewProj, curWorld);   /* prev model == cur (static) */

	gl_Position = curClip;
	v_curClip   = curClip;
	v_prevClip  = prevClip;
	v_normal    = normalize(mul(model, vec4(a_normal, 0.0)).xyz);
}
