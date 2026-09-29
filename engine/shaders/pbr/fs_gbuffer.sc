$input v_normal

#include <bgfx_shader.sh>

/*
 * fs_gbuffer.sc — screen-space-reflection G-buffer.
 *
 * Pairs with vs_pbr (shares varying_pbr.def.sc) and writes, as MRT:
 *   [0] rgb = world normal * 0.5 + 0.5   (decoded in fs_ssr as xyz*2-1)
 *       a   = roughness                  (SSR fades reflections by 1-roughness)
 *   [1] rgb = base-colour ALBEDO         (SSGI's receiver term)
 *
 * Cleared to (0.5,0.5,1,1) => up-ish normal, roughness 1 (matte = no
 * reflection) for pixels not covered by a G-buffer submit (sky/skinned).
 *
 * THE ALBEDO IS THE MATERIAL'S BASE-COLOUR FACTOR, not its texture.  Binding
 * every material's albedo map in a pre-pass that today binds nothing would
 * make the pre-pass as state-heavy as the colour pass, on a stated
 * integrated-GPU baseline, to refine a term that is already a screen-space
 * estimate.  What it fixes is the part that was WRONG rather than coarse:
 * fs_ssgi.sc used the receiver's LIT COLOUR as its albedo, which counts the
 * direct lighting twice.  A flat base colour does not.
 */
uniform vec4 u_gbufferMat;   /* x = roughness                    */
uniform vec4 u_gbufferAlbedo;/* rgb = base colour, a = unused    */

void main()
{
	vec3 n = normalize(v_normal);
	gl_FragData[0] = vec4(n * 0.5 + 0.5, u_gbufferMat.x);
	gl_FragData[1] = vec4(u_gbufferAlbedo.rgb, 1.0);
}
