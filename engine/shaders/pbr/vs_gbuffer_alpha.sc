$input a_texcoord0, a_position, a_normal
$output v_texcoord0, v_normal

#include <bgfx_shader.sh>

/*
 * MASK normal G-buffer variant: preserve the source UV for alpha testing.
 * The normal output and transform match the opaque G-buffer program.
 * Shares varying_pbr.def.sc with vs_pbr so it consumes the same mesh layout.
 */
void main()
{
    v_texcoord0 = a_texcoord0.xy;
	vec3 wpos = mul(u_model[0], vec4(a_position, 1.0)).xyz;
	gl_Position = mul(u_proj, mul(u_view, vec4(wpos, 1.0)));
	v_normal = normalize(mul(u_model[0], vec4(a_normal, 0.0)).xyz);
}
