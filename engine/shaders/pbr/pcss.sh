/*
 * pcss.sh -- contact-hardening penumbra width for the sun's cascaded shadows.
 *
 * WHAT IS MISSING WITHOUT IT.  A fixed filter radius gives every shadow the
 * same softness at every distance, so an object resting on the floor has
 * exactly as blurry a contact edge as its shadow twenty metres away.  That
 * reads as "the shadows are not attached to anything", and it is the one
 * shadow property a tap-count ladder cannot express: Godot 4 has a per-light
 * soft-shadow size and HDRP has PCSS, so a fixed radius is behind two of the
 * three engines this one is measured against.
 *
 * THE METHOD (Fernando 2005), in three steps:
 *   1. search a neighbourhood for OCCLUDERS -- texels nearer the light than
 *      this fragment -- and average their depth;
 *   2. the penumbra is proportional to how far the receiver is BEHIND that
 *      average blocker, scaled by the light's apparent size;
 *   3. widen the PCF kernel by that amount.
 * Step 1 is why this cannot be a cheaper trick: the width of a real penumbra
 * depends on the geometry the shadow was cast BY, and nothing in the
 * receiver's own data knows it.
 *
 * WHY THIS FILE EXISTS RATHER THAN TWO COPIES OF THE MATH.  The cascade
 * filter is written out twice in this tree -- once in fs_pbr_body.sh for
 * meshes and once in csm_shadow.sh for the terrain and the water -- and a
 * penumbra added to only one of them puts hard-edged terrain shadows next to
 * soft mesh shadows in the same frame.  (Grass and foliage take a single hard
 * tap and have no filter to widen, so they are correctly untouched.)
 *
 * REQUIRES, from the includer, declared before this file:
 *   float csm_sample_depth(int cascade, vec2 uv)
 *   uniform vec4 u_shadowQuality   (x = filter tier, y = sun angular diameter
 *                                   in DEGREES, z = penumbra cap in texels)
 *   uniform vec4 u_csmPenumbra     (per-cascade world-units-per-shadow-texel
 *                                   scale; see jce_sr_shadow.c)
 */

/* Per-cascade penumbra scale, selected without indexing a vec4 by a variable
 * -- the same shape u_csmSplits is read with, for the same GLSL 1.20 reason. */
float pcss_scale_for(int cascade)
{
    float k = u_csmPenumbra.x;
    if      (cascade == 1) k = u_csmPenumbra.y;
    else if (cascade == 2) k = u_csmPenumbra.z;
    else if (cascade == 3) k = u_csmPenumbra.w;
    return k;
}

/*
 * Returns the filter radius in shadow texels: `base_radius` when contact
 * hardening is off or no occluder is found, otherwise the widened radius,
 * never narrower than `base_radius` and never wider than the cap.
 *
 * `u_shadowQuality.y` is the sun's ANGULAR DIAMETER IN DEGREES -- the same
 * quantity, in the same unit, as Godot's DirectionalLight3D.angular_distance,
 * so a value copied from a Godot scene means here what it meant there.  The
 * real sun is 0.53 degrees; larger values are the usual artistic licence.
 * Choosing a physical unit is not decoration: the alternative was a unitless
 * "softness" whose meaning would have changed with the shadow resolution, and
 * the whole point of the blocker search is to be resolution-independent.
 */
float pcss_filter_radius(int cascade, vec2 csm_uv, float csm_z,
                         float depth_bias, vec2 texel, float base_radius)
{
    float deg = u_shadowQuality.y;
    float k   = pcss_scale_for(cascade);
    // k == 0 is a cascade this frame never built (cascade_count < 4, or the
    // CSM data was zeroed).  Widening by a scale of zero would be silent; the
    // authored radius is the honest answer.
    if (deg <= 0.0 || k <= 0.0) return base_radius;

    float cap = max(u_shadowQuality.z, 1.0);

    // Search the WHOLE region a penumbra could ever be drawn from, i.e. the
    // cap.  A search window narrower than the cap does not merely cost less:
    // it truncates the penumbra to the search radius while the cap sits
    // unreached, so the cap stops being the thing that bounds the result and
    // the knob quietly saturates early.
    float search = cap;

    // 4x4 over the search window.  Constant loop bounds and no `continue`:
    // the GLSL-120 rule this tree's shadow code is written to.
    float blocker_sum = 0.0;
    float blocker_n   = 0.0;
    for (int y = 0; y < 4; y++)
    {
        for (int x = 0; x < 4; x++)
        {
            vec2 g = vec2(float(x), float(y)) * (2.0 / 3.0) - vec2_splat(1.0);
            float bd = csm_sample_depth(cascade, csm_uv + g * texel * search);
            // Strictly IN FRONT of the receiver, using the same bias the
            // occlusion test uses.  Without the bias a sloped receiver finds
            // itself as its own blocker, the separation collapses to zero,
            // and contact hardening does nothing on exactly the geometry it
            // was added for.
            if (bd < csm_z - depth_bias)
            {
                blocker_sum += bd;
                blocker_n   += 1.0;
            }
        }
    }

    // No occluder in the window: this fragment is lit, or its caster is
    // outside the cap.  There is no penumbra to size either way.
    if (blocker_n <= 0.0) return base_radius;

    float d_blocker = blocker_sum / blocker_n;

    // (csm_z - d_blocker) is a NORMALIZED depth difference; `k` converts it to
    // shadow texels, and radians(deg) is the sun's angular radius-to-width
    // factor.  Doing this with the point-light ratio (dz / d_blocker) instead
    // -- which is what PCSS is usually written as -- is wrong for a
    // DIRECTIONAL light in an ORTHOGRAPHIC cascade: the divisor is a
    // perspective foreshortening term that an ortho projection does not have,
    // and against normalized ortho depth the result came out ~1e-3 of the
    // needed magnitude.  Measured 2026-09-06: with the ratio form, sun sizes
    // of 0, 3 and 8 produced three images that differed by less than the
    // run-to-run noise of the capture tool.
    float widened = (0.01745329 * deg) * (csm_z - d_blocker) * k;

    // Never NARROWER than the authored radius -- PCSS adds softness with
    // distance, it does not take away the softness the artist set.
    return min(max(base_radius, widened), cap);
}
