$input v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * fs_gbuffer_vel.sc — STATIC G-buffer + per-object motion-vector (MRT).
 *
 *   gl_FragData[0] = world normal*0.5+0.5 (rgb) + roughness (a)  [SSR]
 *   gl_FragData[1] = screen motion (rg) encoded (cur_ndc-prev_ndc)*0.5+0.5 [TAA]
 *
 * The velocity encoding is identical to fs_motion_vec.sc so fs_taa.sc consumes
 * it unchanged.  Zero motion -> (0.5,0.5), matching the shared MRT clear.
 * This engine maps screen UV -> NDC as uv*2-1 with NO Y flip on every backend
 * (fs_motion_vec reconstructs world from v_texcoord0*2-1, validated by the
 * camera path), so the screen-space delta equals the clip-NDC delta.
 */
uniform vec4 u_gbufferMat;   /* x = roughness */

void main()
{
	vec3 n = normalize(v_normal);

	vec2 cur_ndc  = v_curClip.xy  / v_curClip.w;
	vec2 prev_ndc = v_prevClip.xy / v_prevClip.w;
	vec2 delta = (cur_ndc - prev_ndc);

	gl_FragData[0] = vec4(n * 0.5 + 0.5, u_gbufferMat.x);
	gl_FragData[1] = vec4(delta * 0.5 + 0.5, 0.0, 1.0);
}
