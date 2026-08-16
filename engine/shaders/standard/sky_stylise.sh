/*
 * sky_stylise.sh -- GPU twin of jce_sky_stylise.c.
 *
 * The physical core computes radiance; this grades it.  Same split, same
 * constants, same order of operations -- the CPU copy is what the tests pin,
 * because the property that matters (identity is a bit-exact no-op) is one a
 * shader cannot be asked about.
 *
 *   u_sky_stylise.x  = bands (0 or 1 = off)
 *   u_sky_stylise.y  = rim strength (0 = off)
 *   u_sky_stylise.z  = saturation (1 = identity)
 *   u_sky_stylise.w  = master enable; 0 skips the whole grade
 *   u_sky_tint_shadow / _mid / _high = three-point palette, (1,1,1) = identity
 */

#ifndef SKY_STYLISE_SH
#define SKY_STYLISE_SH

uniform vec4 u_sky_stylise;
uniform vec4 u_sky_tint_shadow;
uniform vec4 u_sky_tint_mid;
uniform vec4 u_sky_tint_high;

float sky_stylise_luma(vec3 c)
{
	// Rec.709, matching the tonemapper.  A different set here would make the
	// grade disagree with exposure about which parts of the sky are bright,
	// and the horizon would band at a different threshold than the zenith.
	return dot(c, vec3(0.2126, 0.7152, 0.0722));
}

vec3 sky_stylise(vec3 c, float rim)
{
	// The enable is checked FIRST and returns the input untouched.  The graded
	// path ends in a clamp that drops negatives and non-finite values, which is
	// right for a grade and wrong for a pass-through -- so a disabled grade
	// that still ran the arithmetic would rewrite whatever the scattering core
	// produced, and the sky would depend on a switch that is off.
	if (u_sky_stylise.w < 0.5) return c;

	float l = sky_stylise_luma(c);

	// Palette position from a tone-mapped proxy, not from luminance directly:
	// radiance is unbounded, and a clamp would pin every pixel above 1.0 to the
	// highlight tint, flattening the upper half of an HDR sky to one colour.
	float t = l / (1.0 + l);
	vec3 tint = (t < 0.5)
		? mix(u_sky_tint_shadow.rgb, u_sky_tint_mid.rgb,  t * 2.0)
		: mix(u_sky_tint_mid.rgb,    u_sky_tint_high.rgb, (t - 0.5) * 2.0);
	c *= tint;

	// Posterise LUMINANCE and rescale, never per channel: per-channel banding
	// shifts hue at every step, so a smooth blue gradient bands into blue, then
	// cyan, then green -- which reads as a colour bug, not as posterisation.
	if (u_sky_stylise.x > 1.5)
	{
		float lum = sky_stylise_luma(c);
		if (lum > 1e-6)
		{
			float n  = u_sky_stylise.x;
			float tt = lum / (1.0 + lum);
			float q  = floor(tt * n) / n + (0.5 / n);
			float target = q / (1.0 - min(q, 0.999));
			c *= target / lum;
		}
	}

	// Rim: what makes stylised clouds read as shapes rather than as fog.
	if (u_sky_stylise.y != 0.0 && rim > 0.0)
		c *= 1.0 + u_sky_stylise.y * clamp(rim, 0.0, 1.0);

	// Saturation around the CURRENT luminance -- the tint and bands have
	// already moved it, and pulling toward a stale one would partly undo them.
	if (u_sky_stylise.z != 1.0)
	{
		float lc = sky_stylise_luma(c);
		c = vec3_splat(lc) + (c - vec3_splat(lc)) * u_sky_stylise.z;
	}

	return max(c, vec3_splat(0.0));
}

#endif // SKY_STYLISE_SH
