#ifndef JCE_CLOUD_SHADOW_SH
#define JCE_CLOUD_SHADOW_SH

/* Top-down cloud transmittance, sampled by WORLD XZ.
 *
 * Baked on the CPU (jce_cloud_shadow.h) into a map that covers `extent` metres
 * centred on `centre`, re-baked only when the sun, the camera or the wind have
 * moved it enough to matter.
 *
 * Shared rather than copied because the four surfaces under the sky must agree
 * about where the clouds are. They did not: terrain read the map directly,
 * meshes read a screen-space copy the SSAO pass wrote into its blue channel --
 * and so had cloud shadows only when SSAO was on -- while grass, foliage and
 * water read nothing at all and stayed lit under a cloud that darkened the
 * ground they stood on.
 *
 * Every including shader must declare, at ITS OWN stage:
 *     SAMPLER2D(s_cloudShadow, <stage>)
 *     uniform vec4 u_cloudShadow;   // x=extent  yz=centre XZ  w=strength
 * Stage 3 everywhere it is possible: terrain has all sixteen occupied and 3 is
 * the only one it can give up. */
float cloud_shadow_at(vec2 world_xz)
{
	if (u_cloudShadow.x <= 0.0 || u_cloudShadow.w <= 0.0) return 1.0;
	vec2 muv = (world_xz - (u_cloudShadow.yz - vec2_splat(0.5 * u_cloudShadow.x)))
	         / u_cloudShadow.x;
	/* Outside the baked footprint there is no information, and clamping would
	 * smear the edge texel across the rest of the world. Full sun is the
	 * honest answer. */
	if (muv.x < 0.0 || muv.x > 1.0 || muv.y < 0.0 || muv.y > 1.0) return 1.0;
	float t = texture2D(s_cloudShadow, muv).r;
	return mix(1.0, clamp(t, 0.0, 1.0), u_cloudShadow.w);
}

#endif /* JCE_CLOUD_SHADOW_SH */
