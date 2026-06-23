$input a_position, a_normal
$output v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * vs_gbuffer_vel.sc — STATIC G-buffer + per-object motion-vector vertex shader.
 *
 * MRT prepass (when TAA is on): writes world normal+roughness (slot 0, for SSR)
 * AND per-object screen motion (slot 1, for TAA).  Emits the vertex's CURRENT
 * and PREVIOUS clip-space positions so fs computes the NDC delta.  Both
 * projections use the UN-JITTERED view*proj; the current world comes from
 * u_model[0] (set via bgfx_set_transform), the previous from u_prevModel.
 */
uniform mat4 u_prevModel;
uniform mat4 u_curViewProj;
uniform mat4 u_prevViewProj;

void main()
{
	vec4 curWorld  = mul(u_model[0],   vec4(a_position, 1.0));
	vec4 prevWorld = mul(u_prevModel,  vec4(a_position, 1.0));

	vec4 curClip  = mul(u_curViewProj,  curWorld);
	vec4 prevClip = mul(u_prevViewProj, prevWorld);

	gl_Position = curClip;
	v_curClip   = curClip;
	v_prevClip  = prevClip;
	v_normal    = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
}
