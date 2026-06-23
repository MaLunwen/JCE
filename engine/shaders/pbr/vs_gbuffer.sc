$input a_position, a_normal
$output v_normal

#include <bgfx_shader.sh>

/*
 * vs_gbuffer.sc — minimal vertex shader for the SSR normal G-buffer.
 * Outputs only the world-space normal (fs_gbuffer encodes it + roughness).
 * Shares varying_pbr.def.sc with vs_pbr so it consumes the same mesh layout.
 */
void main()
{
	vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
	gl_Position = mul(u_proj, mul(u_view, vec4(wpos, 1.0)));
	v_normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
}
