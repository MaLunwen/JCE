#ifndef JCE_SCREEN_RECONSTRUCT_SH
#define JCE_SCREEN_RECONSTRUCT_SH

/*
 * screen_reconstruct.sh -- screen uv <-> clip space, once, for every pass that
 * reads the depth buffer back.
 *
 * WHY THIS IS A FILE.  `uv * 2.0 - 1.0` and its inverse are three characters
 * apart from being wrong on half the backends, and this tree has paid for that
 * family twelve times in one campaign: D3D's texture origin is top-left and
 * GL's is bottom-left, so the y flip belongs on one side and not the other,
 * and GL's depth NDC is [-1,1] where every other backend's is [0,1].  Every
 * one of those bugs looked like a rendering artefact rather than an axis
 * error, and the round trips are the worst of them -- a pass that flips going
 * in and flips coming out is self-consistent in a MIRRORED space, so it only
 * disagrees with whatever else reads the same pixel.  fs_ssr.sc carries that
 * exact comment about its own history.
 *
 * So: one pair of inverses, used by both sides, rather than a copy per pass.
 * A caller that needs only one direction still gets the other's convention.
 */

/* Screen uv (0..1, texture origin) + a depth-buffer sample -> world space. */
vec3 jce_uv_depth_to_world(mat4 inv_view_proj, vec2 uv, float d)
{
#if BGFX_SHADER_LANGUAGE_GLSL
	float ndc_z = d * 2.0 - 1.0;   /* GL: depth-buffer NDC z is [-1,1] */
#else
	float ndc_z = d;               /* D3D/Vulkan/Metal/WebGPU: NDC z is [0,1] */
#endif
	vec2 ndc_xy = uv * 2.0 - 1.0;
#if !BGFX_SHADER_LANGUAGE_GLSL
	ndc_xy.y = -ndc_xy.y;          /* top-left origin; inverse below matches */
#endif
	vec4 ndc = vec4(ndc_xy, ndc_z, 1.0);
	vec4 wp = mul(inv_view_proj, ndc);
	return wp.xyz / wp.w;
}

/* Clip space -> screen uv (0..1, texture origin).  The exact inverse of the
 * xy half of jce_uv_depth_to_world; change one and change the other. */
vec2 jce_clip_to_uv(vec4 clip)
{
	vec2 uv = clip.xy / clip.w * 0.5 + 0.5;
#if !BGFX_SHADER_LANGUAGE_GLSL
	uv.y = 1.0 - uv.y;
#endif
	return uv;
}

#endif /* JCE_SCREEN_RECONSTRUCT_SH */
