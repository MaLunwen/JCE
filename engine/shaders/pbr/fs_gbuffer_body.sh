#include <bgfx_shader.sh>

/* Shared normal/albedo MRT encoding. Albedo is the material's base-colour
 * factor, not its lit colour or sampled texture. The matte clear keeps
 * uncovered pixels out of SSR. Velocity is not an attachment of this pass. */
uniform vec4 u_gbufferMat;    /* x = roughness */
uniform vec4 u_gbufferAlbedo; /* rgb = base colour */

void main()
{
	vec3 n = normalize(v_normal);
	gl_FragData[0] = vec4(n * 0.5 + 0.5, u_gbufferMat.x);
	gl_FragData[1] = vec4(u_gbufferAlbedo.rgb, 1.0);
}
