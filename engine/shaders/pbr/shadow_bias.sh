#ifndef JCE_SHADOW_BIAS_SH
#define JCE_SHADOW_BIAS_SH

/*
 * shadow_bias.sh -- the cascade depth bias, in units that mean something.
 *
 * WHAT WAS WRONG.  The bias was a constant in NORMALIZED shadow-map depth.  A
 * shadow map stores depth normalised over its cascade's orthographic range, so
 * the same bias number is a different WORLD distance in every cascade, and the
 * factor is the cascade fit's geometry -- a quantity nothing in the expression
 * accounted for and nobody chose.  Measured on one frame, camera at 1.8 m,
 * scene 12 m tall, shadow_far 300, at a mid slope:
 *
 *     world depth bias    c0 89 mm   c1 259 mm   c2 1131 mm   c3 8969 mm
 *
 * Nine metres in cascade 3.  It also moved with the PROJECT: cascade 0 is
 * 89 mm at shadow_far 300 and 17 mm at shadow_far 40, so shortening the shadow
 * distance for performance silently retuned every shadow in the game.  And the
 * two hand-tuned ladders that had grown on top of it -- `mix(1,2,cascade)` and
 * a cap rising `mix(0.01, 0.08, cascade)` -- were compensating for exactly
 * this, one constant at a time, which is why cascade 3 was allowed eight times
 * cascade 0's slack.
 *
 * WHAT IT IS NOW.  The bias is a number of SHADOW TEXELS of world offset,
 * converted to normalised depth by the cascade's own scale.  Acne is a
 * texel-quantisation artifact -- the depth gradient across one shadow texel --
 * so texels is the unit the quantity was always in.  The per-cascade growth
 * then comes out of the geometry instead of being hand-laddered: a wider
 * cascade has bigger texels and gets proportionally more bias, and the same
 * authored number means the same thing at any shadow distance.
 *
 * u_csmPenumbra already carries the bridge -- per-cascade depth-range units
 * per shadow texel, uploaded every frame for the contact-hardening search
 * (jce_sr_shadow.c).  Its reciprocal is what converts texels to normalised
 * depth, so this needed no new uniform: the number the engine had to know was
 * already being told to the shader, and only one consumer was listening.
 *
 * CALIBRATED, NOT GUESSED.  Matching cascade 0's old behaviour exactly gives
 * `texels = depth_range(0) / (2 * radius(0))`, which over shadow distances
 * from 40 to 600 m and sun elevations from 12 to 45 degrees lands between
 * 1.35 and 2.09 and clusters at 1.6.  So 1.6 texels is the terrain and water
 * figure and cascade 0 stays within +/-15% of what it rendered before;
 * everything that moves is a cascade whose bias was wrong for a reason.
 * fs_pbr's meshes keep their historical 3x (4.8 texels) and their steeper
 * slope term rather than having that asymmetry silently removed here -- it is
 * a separate decision, and this change is already moving every shadow.
 *
 * THE CAP IS IN TEXELS TOO, for the same reason the bias is: 0.01 normalised
 * at cascade 0 was 33 texels, and that is what the number meant.  The absolute
 * 0.08 ceiling stays as a backstop for a degenerate cascade, where the scale
 * itself would be nonsense.
 *
 * NOT CHANGED: the normal-offset term (u_csmParams.z * bias_scale), which is
 * already a world distance and pushes along the surface normal rather than
 * along the light.  u_csmBiasScales keeps its meaning for that term only.
 *
 * REQUIRES, from the includer, declared before this file:
 *   float pcss_scale_for(int cascade)   -- pcss.sh
 */

/* Per-cascade normalised depth per shadow texel.  The guard matters: a cascade
 * the CSM pass never built uploads 0 here, and 1/0 would make every fragment
 * in it shadowed. */
float jce_shadow_texel_depth(int cascade)
{
	return 1.0 / max(pcss_scale_for(cascade), 1.0);
}

/*
 * `texels`  world offset, in shadow texels of this cascade
 * `slope`   sin(theta)/max(ndotl, 0.1) -- the depth gradient across a texel
 * `slope_k` how hard the slope term pulls (the two surface classes differ)
 */
float jce_shadow_depth_bias(int cascade, float texels, float slope,
                            float slope_k)
{
	float td = jce_shadow_texel_depth(cascade);
	float b  = texels * td * (1.0 + slope * slope_k);
	/* 32 texels: the translation of the old 0.01 normalised ceiling at
	 * cascade 0, which is where that number was tuned. */
	return min(b, min(32.0 * td, 0.08));
}

#endif /* JCE_SHADOW_BIAS_SH */
