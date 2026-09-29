$input v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * fs_gbuffer_vel.sc — STATIC G-buffer + per-object motion-vector (MRT).
 *
 *   gl_FragData[0] = world normal*0.5+0.5 (rgb) + roughness (a)  [SSR]
 *   gl_FragData[1] = base-colour albedo (rgb)                     [SSGI]
 *   gl_FragData[2] = screen motion (rg) encoded (cur_ndc-prev_ndc)*0.5+0.5 [TAA]
 *
 * ALBEDO SITS AT 1 WHETHER OR NOT SSGI IS ON, and velocity moved to 2 for it.
 * A shifting index is how a shader writes albedo into the velocity target on
 * the frames the albedo attachment is absent -- and TAA would then ghost for a
 * reason nothing in the TAA code could explain.
 *
 * The velocity encoding is identical to fs_motion_vec.sc so fs_taa.sc consumes
 * it unchanged.  Zero motion -> (0.5,0.5), matching the shared MRT clear.
 * The invariant is that s_texMotion carries a TRUE CLIP-NDC delta on every
 * backend -- which is what v_curClip / v_prevClip give here for free, straight
 * out of the vertex pipeline.  Screen UV -> NDC is NOT uv*2-1 everywhere: that
 * holds only on GL (bottom-left origin, after vs_postfx.sc's V-flip); on
 * D3D/VK/Metal uv.y grows DOWN so ndc.y = 1 - 2*uv.y.  fs_motion_vec.sc
 * branches on that to produce the same encoding, and fs_taa.sc / fs_tsr.sc
 * branch on it again turning the NDC delta back into a UV offset.
 */
uniform vec4 u_gbufferMat;   /* x = roughness                 */
uniform vec4 u_gbufferAlbedo;/* rgb = base colour, a = unused */

void main()
{
	vec3 n = normalize(v_normal);

	vec2 cur_ndc  = v_curClip.xy  / v_curClip.w;
	vec2 prev_ndc = v_prevClip.xy / v_prevClip.w;
	vec2 delta = (cur_ndc - prev_ndc);

	gl_FragData[0] = vec4(n * 0.5 + 0.5, u_gbufferMat.x);
	gl_FragData[1] = vec4(u_gbufferAlbedo.rgb, 1.0);
	gl_FragData[2] = vec4(delta * 0.5 + 0.5, 0.0, 1.0);
}
