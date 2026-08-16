/*
 * jce_shadow_lod.h -- is a caster's shadow big enough to be worth rendering?
 *
 * Standard-engine parity (UE r.Shadow.RadiusThreshold, Unity small-shadow
 * culling): a caster whose shadow spans less than a texel or two in a given
 * cascade cannot resolve there, so rendering it costs a draw and produces
 * nothing.  Distant clutter dominates the caster count, so this is where the
 * savings are.
 *
 * The decision is PER CASCADE, and that is the whole subtlety.  The same bush
 * is a real shadow in the tight near cascade (small texels, large footprint)
 * and sub-texel noise in the far one.  A cull keyed on world size alone, or on
 * distance from the camera alone, gets both ends wrong -- a big tree far away
 * is still a valid caster, and a pebble underfoot is not.
 *
 * Every failure here is silent in the same direction: too aggressive and
 * shadows simply are not there, which reads as "this engine's shadows are
 * subtle" rather than as a bug.  So every degenerate input keeps the caster.
 *
 * Layer: Scene (L4) -- PRIVATE.  Pure arithmetic, no bgfx: see jce_shadow_key.h
 * for why that matters.
 */

#ifndef JCE_SHADOW_LOD_H
#define JCE_SHADOW_LOD_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/*
 * The caster's shadow footprint measured in shadow-map texels.
 *
 * A cascade of radius R covers 2R world units across `map_size` texels, so one
 * texel is 2R/map_size across and a caster of extent `diag` spans
 * diag * map_size / (2R) of them.
 *
 * Returns 0 for any degenerate input.  Callers must treat 0 as "unknown", not
 * as "too small" -- see jce_shadow_caster_resolvable().
 */
static inline float jce_shadow_caster_texel_span(float diag_m,
                                                 float cascade_radius_m,
                                                 uint32_t map_size)
{
    if (!(diag_m > 0.0f) || !(cascade_radius_m > 0.0f) || map_size == 0u)
        return 0.0f;
    return diag_m * (float)map_size / (2.0f * cascade_radius_m);
}

/*
 * Should this caster be rendered into this cascade?
 *
 * `min_texels <= 0` disables the cull entirely and must keep EVERY caster --
 * that is the A/B switch, and an A/B switch that changes the picture is not one.
 */
static inline bool jce_shadow_caster_resolvable(float diag_m,
                                                float cascade_radius_m,
                                                uint32_t map_size,
                                                float min_texels)
{
    if (!(min_texels > 0.0f)) return true;          /* cull disabled */

    const float span = jce_shadow_caster_texel_span(diag_m, cascade_radius_m,
                                                    map_size);
    if (span <= 0.0f) return true;                  /* unknown -> keep */
    return span >= min_texels;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADOW_LOD_H */
