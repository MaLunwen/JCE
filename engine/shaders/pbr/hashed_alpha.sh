/*
 * hashed_alpha.sh -- stochastic alpha testing (Wyman & McGuire 2017).
 *
 * GPU twin of jce_hashed_alpha.c.  The CPU copy is what the tests pin, because
 * the property that makes this correct is statistical -- the fraction of
 * fragments surviving the threshold must equal the alpha value -- and a shader
 * cannot be asked that question.  The two must stay in step; every constant
 * here has a counterpart there.
 *
 * Keyed on OBJECT space, never world space.  Foliage is wind-animated, so a
 * world-space hash resamples the noise as the leaf sways and the stipple
 * crawls across the surface.  Object space is invariant under both the wind
 * and the camera.
 */

#ifndef HASHED_ALPHA_SH
#define HASHED_ALPHA_SH

// Hoskins hash, fract-only and deliberately WITHOUT sin(): sin() of a large
// argument loses precision differently on GL/GLES than on D3D, so a sin-based
// hash gives a different stipple per backend far from the origin -- and since
// this drives an alpha test, that is different GEOMETRY, not merely different
// noise.
float hashed_alpha_hash3(vec3 p)
{
	p = fract(p * vec3(0.1031, 0.1030, 0.0973));
	p += dot(p, p.yxz + 33.33);
	return fract((p.x + p.y) * p.z);
}

float hashed_alpha_threshold(vec3 obj_pos, float scale)
{
	// One pixel's worth of object space.  Without it the noise is fixed in
	// object space and aliases into per-pixel static as the object recedes,
	// which looks like a broken texture rather than an alpha test.
	float pix_deriv = max(length(dFdx(obj_pos)), length(dFdy(obj_pos)));
	if (pix_deriv <= 0.0) return 0.5;

	float pix_scale = 1.0 / (scale * pix_deriv);

	// The two nearest LOG-DISCRETISED noise scales, lerped.  Snapping to one
	// makes the stipple visibly change size as the object moves; this pair is
	// the paper's stability result.
	float lg   = log2(pix_scale);
	float s_lo = exp2(floor(lg));
	float s_hi = exp2(ceil(lg));

	float a0 = hashed_alpha_hash3(floor(s_lo * obj_pos));
	float a1 = hashed_alpha_hash3(floor(s_hi * obj_pos));

	float lerp_t = lg - floor(lg);
	float v = mix(a0, a1, lerp_t);

	// The lerp of two uniform variables is TRIANGULAR, and a non-uniform
	// threshold biases coverage -- the surviving fraction stops equalling
	// alpha, so foliage systematically thins or thickens with distance.  This
	// CDF maps it back to uniform and is what makes the technique correct
	// rather than merely dithered.
	float a = min(lerp_t, 1.0 - lerp_t);
	vec3 cases = vec3(v * v / (2.0 * a * (1.0 - a)),
	                  (v - 0.5 * a) / (1.0 - a),
	                  1.0 - ((1.0 - v) * (1.0 - v) / (2.0 * a * (1.0 - a))));
	float t = (v < (1.0 - a)) ? ((v < a) ? cases.x : cases.y) : cases.z;

	// Never exactly 0: a zero threshold passes a fully transparent fragment,
	// drawing an opaque dot in a leaf's empty margin.
	return clamp(t, 1.0e-6, 1.0);
}

#endif // HASHED_ALPHA_SH
