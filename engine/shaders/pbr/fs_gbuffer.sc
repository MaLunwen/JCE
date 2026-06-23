$input v_normal

#include <bgfx_shader.sh>

/*
 * fs_gbuffer.sc — screen-space-reflection G-buffer.
 *
 * Pairs with vs_pbr (shares varying_pbr.def.sc) and writes the world-space
 * surface normal + material roughness into one RGBA8 target:
 *   rgb = world normal * 0.5 + 0.5   (decoded in fs_ssr as xyz*2-1)
 *   a   = roughness                  (SSR fades reflections by 1-roughness)
 *
 * Cleared to (0.5,0.5,1,1) => up-ish normal, roughness 1 (matte = no
 * reflection) for pixels not covered by a G-buffer submit (sky/skinned).
 */
uniform vec4 u_gbufferMat;   /* x = roughness */

void main()
{
	vec3 n = normalize(v_normal);
	gl_FragColor = vec4(n * 0.5 + 0.5, u_gbufferMat.x);
}
