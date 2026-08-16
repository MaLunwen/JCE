$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_underwater.sc -- Beer-Lambert absorption for a submerged camera.
 *
 * The water SURFACE shader already absorbs what is behind the surface when you
 * look into water from above.  It cannot do the job from below: it only runs on
 * pixels where the water geometry rasterises, and underwater that is the patch
 * of surface above your head.  Everything else -- the sea floor, the rocks
 * beside you, the whole rest of the view -- has no water polygon in front of
 * it and would render bone dry while the camera is submerged.
 *
 * So this is a fullscreen pass, and it runs in TWO submits because the result
 * is per-channel:
 *
 *     dst = dst * T(rgb) + tint * (1 - T(rgb))
 *
 * A single fixed-function blend cannot express that -- there is one source
 * colour and it would have to be both T and the in-scattered tint at once.
 * Splitting it into `dst *= T` then `dst += tint*(1-T)` is exact, needs no
 * read-back of the colour target being written, and costs two fullscreen
 * quads whose fragments are trivial.
 *
 *   u_uw_sigma.xyz   = per-channel extinction (1/m)
 *   u_uw_sigma.w     = pass select: 0 = multiply T, 1 = add in-scatter
 *   u_uw_tint.rgb    = colour the water converges to with distance
 *   u_uw_params.xy   = near, far
 *   u_uw_params.z    = max path (m); beyond this the medium is saturated and
 *                      marching further only costs precision
 */

SAMPLER2D(s_uw_depth, 0);

uniform vec4 u_uw_sigma;
uniform vec4 u_uw_tint;
uniform vec4 u_uw_params;

float uw_linear(float d)
{
	float n = u_uw_params.x;
	float f = u_uw_params.y;
#if BGFX_SHADER_LANGUAGE_GLSL
	float z = d * 2.0 - 1.0;
	return (2.0 * n * f) / (f + n - z * (f - n));
#else
	return (n * f) / (f - d * (f - n));
#endif
}

void main()
{
	float d = texture2D(s_uw_depth, v_texcoord0).r;
	/* Sky/background: the far plane is not "infinitely deep water", it is
	 * wherever the scene stopped.  Clamping to max path keeps the horizon a
	 * saturated water colour instead of exactly the tint at infinite depth --
	 * which is the same value, but reached without an exp() of a huge number. */
	float dist = (d >= 1.0) ? u_uw_params.z : min(uw_linear(d), u_uw_params.z);

	vec3 T = exp(-u_uw_sigma.xyz * dist);

	if (u_uw_sigma.w < 0.5)
	{
		/* Pass 1: dst *= T.  Blend (ZERO, SRC_COLOR). */
		gl_FragColor = vec4(T, 1.0);
	}
	else
	{
		/* Pass 2: dst += tint * (1 - T).  Blend (ONE, ONE).
		 * This term is what stops the view going black with distance: the
		 * exponential alone drives every channel to zero, and a black horizon
		 * underwater reads as a missing skybox. */
		gl_FragColor = vec4(u_uw_tint.rgb * (1.0 - T), 1.0);
	}
}
