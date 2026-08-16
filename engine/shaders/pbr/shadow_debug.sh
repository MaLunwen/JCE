#ifndef SHADOW_DEBUG_SH
#define SHADOW_DEBUG_SH

/*
 * shadow_debug.sh -- colour conventions for the shadow/depth debug views.
 *
 * Its own file because the two shading paths cannot share the one they would
 * otherwise live in: fs_pbr_body.sh defines its own sample_csm_shadow(), so it
 * cannot include csm_shadow.sh without a redefinition, while fs_terrain.sc and
 * fs_water.sc go through csm_shadow.sh. That split is a real hazard of its own
 * -- there are two copies of the cascade-selection logic in this directory --
 * and the least this file can do is make sure the VIEW that would expose such a
 * divergence is not itself divergent. If meshes and terrain ever disagree about
 * which cascade covers a fragment, these views must render that disagreement,
 * not hide it behind two different palettes.
 *
 * No uniforms are referenced here on purpose: every input is a parameter, so
 * this include is safe to pull into any fragment shader regardless of which
 * uniform blocks it happens to declare.
 */

/* Cascade index -> tint.  Red / green / blue / yellow is the convention every
 * engine with this view uses, so the picture is readable without a legend.
 * Grey is "past every cascade", which is NOT cascade 3 -- that distinction is
 * the entire diagnostic value of the view: a surface rendered grey is lit
 * because no shadow map covers it, and no amount of shadow tuning will change
 * it until the range does. */
vec3 shadow_debug_cascade_color(float cascade)
{
    if (cascade < -0.5) return vec3(0.25, 0.25, 0.25);
    if (cascade <  0.5) return vec3(0.90, 0.25, 0.25);
    if (cascade <  1.5) return vec3(0.25, 0.85, 0.30);
    if (cascade <  2.5) return vec3(0.30, 0.45, 0.95);
    return vec3(0.95, 0.85, 0.25);
}

/* View depth -> greyscale, normalised against `far_d` (pass the shadow far
 * plane, u_csmSplits.w).
 *
 * GREYSCALE, not a rainbow ramp, for two reasons and the second matters more.
 *
 * First, it is what every engine's Scene Depth visualisation shows, so the
 * picture needs no legend.
 *
 * Second, it is INVERTIBLE. A ramp is pretty and answers "is this farther than
 * that"; a greyscale channel answers "how far, in metres" -- decode the pixel,
 * multiply by far_d -- which is what turns this view from an illustration into
 * an instrument. Correlating surface colour against actual distance is the
 * measurement this view exists to make possible, and a hue cycle cannot do it.
 *
 * It also makes coverage gaps self-evident: every shading path that honours the
 * view mode emits R == G == B here, so any COLOURED geometry pixel is a shader
 * that ignored it. That check found fs_foliage.sc and fs_water.sc, which had
 * been silently ignoring the material debug views since those were added.
 *
 * Normalised against the SHADOW range rather than the camera far plane: the
 * editor's orbit camera sets its far to orbit_distance * 100, and a ramp
 * against that is nearly flat everywhere. */
vec3 shadow_debug_depth_color(float view_depth, float far_d)
{
    float f = max(far_d, 1.0);
    return vec3_splat(clamp(view_depth / f, 0.0, 1.0));
}

#endif // SHADOW_DEBUG_SH
