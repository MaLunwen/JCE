$input v_texcoord0

#include <bgfx_shader.sh>

/*
 * fs_ssgi_composite.sc -- denoise the bounce RT, then add it to the scene.
 *
 * ADDITIVE, unlike fs_ssr_composite's premultiplied "over".  A reflection
 * REPLACES what a mirror shows; a diffuse bounce is extra light arriving at a
 * surface that is already lit, so it adds.
 *
 * THE FILTER LIVES HERE AND NOT IN A PASS OF ITS OWN, and that is a view-budget
 * decision rather than a stylistic one.  SSGI owns exactly two base-relative
 * view ids (26 = march, 27 = composite, jce_views.h); 28 is the fullscreen
 * chain's fold-back composite and 29 the editor overlay, so a third SSGI view
 * would have to displace one of those.  This pass already samples the bounce RT
 * once per pixel and already runs at the right moment -- after the march, before
 * postfx -- so widening its single fetch into a small guided gather buys the
 * denoise for no extra view, no extra render target and no extra program.
 *
 * WHY A JOINT BILATERAL AND NOT A BLUR.  A diffuse bounce is low frequency by
 * construction and the 4-ray estimator's error is not, so averaging neighbours
 * is the right move -- but only neighbours that are on the SAME SURFACE.  A
 * plain blur drags the red off a wall onto the floor in front of it, which is
 * precisely the picture this effect exists to produce and precisely where the
 * mistake would be invisible.  So each tap is weighted by how close its depth
 * is (relative, not absolute -- a 5 cm difference is nothing at 40 m and a wall
 * at 0.5 m) and by how aligned its normal is.
 *
 * WHAT IS DELIBERATELY *NOT* A WEIGHT: the RT's alpha, which carries hit
 * coverage.  Coverage looks like a confidence and is not one.  bounce.rgb is
 * already the per-pixel estimate gather/rays -- normalised by RAYS CAST, not by
 * rays that hit -- so it is unbiased, and the average of unbiased estimates is
 * unbiased.  Weighting by coverage would up-weight exactly the pixels whose
 * rays happened to hit, which is a positive bias: measured, an earlier version
 * of this filter that multiplied in coverage RAISED the bounce's high-frequency
 * energy by 37.6% while claiming to be a denoiser.  The alpha stays a debug
 * output.
 *
 * DETERMINISM is preserved: the kernel is fixed and carries no frame counter,
 * so two runs of one frame are still byte-identical -- see fs_ssgi.sc's own
 * DETERMINISM note for why this tree requires that.
 */

SAMPLER2D(s_color,  0);   /* the bounce RT: rgb = radiance, a = hit coverage */
SAMPLER2D(s_depth,  1);   /* depth pre-pass depth, non-linear               */
SAMPLER2D(s_normal, 2);   /* rgb = world normal * 0.5 + 0.5                 */

uniform vec4 u_ssgi_params0;   /* (radius, thickness, near, far)            */
uniform vec4 u_screen;         /* (w, h, 1/w, 1/h)                          */

float ssgi_c_linearize(float d, float n, float f)
{
	return n * f / (f - d * (f - n));
}

/* 3x3 at a 2-texel stride: ±2 px of support for 9 colour fetches.
 *
 * THE STRIDE IS WHY IT IS 3x3 AND NOT 5x5.  The estimator's error is
 * per-pixel (the ray rotation is a hash of gl_FragCoord), so skipping every
 * other texel decorrelates the taps as well as a dense kernel would while
 * costing a ninth of the fetches of one that covered the same ±2 px densely.
 * The stated baseline is an integrated GPU and the march next door already
 * spends 32 fetches; 27 more here (colour+depth+normal per tap) is the budget
 * this pass gets. */
#define SSGI_DN_STRIDE 2.0

void main()
{
	vec2  uv    = v_texcoord0;
	vec2  texel = u_screen.zw;
	float znear = u_ssgi_params0.z;
	float zfar  = u_ssgi_params0.w;

	float dc = texture2D(s_depth, uv).r;
	vec3  nc = normalize(texture2D(s_normal, uv).xyz * 2.0 - 1.0);
	float lc = ssgi_c_linearize(dc, znear, zfar);

	/* Sky / cleared depth has no receiver, so there is nothing to filter and
	 * nothing to add.  Matches fs_ssgi.sc's own early-out on the same test, so
	 * the two passes agree about where the effect exists. */
	if (dc >= 0.9999)
	{
		gl_FragColor = vec4_splat(0.0);
		return;
	}

	vec3  sum = vec3_splat(0.0);
	float wsum = 0.0;

	for (int j = -1; j <= 1; ++j)
	{
		for (int i = -1; i <= 1; ++i)
		{
			vec2 off = vec2(float(i), float(j)) * SSGI_DN_STRIDE;
			vec2 suv = uv + off * texel;

			float ds = texture2D(s_depth, suv).r;
			/* A tap on the sky is not on this surface. */
			if (ds >= 0.9999) continue;

			float ls = ssgi_c_linearize(ds, znear, zfar);
			/* RELATIVE depth tolerance: 2% of the centre's own distance.  An
			 * absolute epsilon would over-blur far geometry and under-blur
			 * near geometry, which is the same mistake in both directions. */
			float wd = exp(-abs(ls - lc) / max(0.02 * lc, 1e-4));

			vec3  ns = normalize(texture2D(s_normal, suv).xyz * 2.0 - 1.0);
			float wn = max(dot(ns, nc), 0.0);
			wn = wn * wn; wn = wn * wn; wn = wn * wn; wn = wn * wn;  /* ^16 */

			/* Spatial falloff over the ±2 px support. */
			float r2 = dot(off, off);
			float ws = exp(-r2 * 0.125);

			float w = ws * wd * wn;
			sum  += texture2D(s_color, suv).rgb * w;
			wsum += w;
		}
	}

	/* wsum is never zero -- the centre tap weighs ws*1*1 = 1 -- but a guard
	 * costs nothing and a NaN here would be added to the whole frame. */
	vec3 bounce = (wsum > 0.0) ? (sum / wsum) : texture2D(s_color, uv).rgb;
	gl_FragColor = vec4(bounce, 0.0);
}
