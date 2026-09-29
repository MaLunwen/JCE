$input v_normal, v_curClip, v_prevClip

#include <bgfx_shader.sh>

/*
 * fs_gbuffer_vel_skinned.sc — SKINNED G-buffer + per-bone motion-vector (MRT).
 *
 *   gl_FragData[0] = world normal*0.5+0.5 (rgb) + roughness (a)  [SSR]
 *   gl_FragData[1] = screen motion (rg) encoded (cur_ndc-prev_ndc)*0.5+0.5 [TAA]
 *
 * Same encoding as fs_gbuffer_vel.sc / fs_motion_vec.sc.  Zero motion ->
 * (0.5,0.5), matching the shared MRT clear.
 */
uniform vec4 u_gbufferMat;
uniform vec4 u_gbufferAlbedo;/* rgb = base colour */   /* x = roughness */

void main()
{
	vec3 n = normalize(v_normal);

	vec2 cur_ndc  = v_curClip.xy  / v_curClip.w;
	vec2 prev_ndc = v_prevClip.xy / v_prevClip.w;
	vec2 delta = (cur_ndc - prev_ndc);

	gl_FragData[0] = vec4(n * 0.5 + 0.5, u_gbufferMat.x);
	/* Albedo at 1, velocity at 2 -- see fs_gbuffer_vel.sc for why the
	 * index is fixed rather than following the attachment count. */
	gl_FragData[1] = vec4(u_gbufferAlbedo.rgb, 1.0);
	gl_FragData[2] = vec4(delta * 0.5 + 0.5, 0.0, 1.0);
}
