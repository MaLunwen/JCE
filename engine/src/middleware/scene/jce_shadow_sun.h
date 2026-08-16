/*
 * jce_shadow_sun.h -- sun-motion quantisation for the CSM cache.
 *
 * The cascade cache is reused only when the snapped light VP is BIT-IDENTICAL
 * to the one the depth buffer already holds.  That is the right test, but it
 * means a day/night cycle defeats the cache completely: the sun moves a few
 * thousandths of a degree per frame, the VP differs in its last mantissa bits,
 * and every cascade re-renders every frame for a shadow map that is texel-for-
 * texel the same one.  The cost is real and the benefit is zero.
 *
 * So the sun the SHADOWS use is held still until the sun has moved far enough
 * to change the map.  Note what that is not: it is not a tuned epsilon.  The
 * quantum is derived -- it is the angle at which the tallest caster's shadow
 * slides by one shadow-map texel, below which re-rendering rounds to the same
 * depth values anyway.
 *
 * Layer: Scene (L4) -- PRIVATE.
 */

#ifndef JCE_SHADOW_SUN_H
#define JCE_SHADOW_SUN_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* The sun direction the shadow maps were last rendered for.  Zero-initialise. */
typedef struct JceShadowSunHold {
    float dir[3];
    bool  held;
} JceShadowSunHold;

/*
 * The angle the sun may move before the shadow map changes.
 *
 * shadow displacement = caster height x tan(dtheta); one texel of slide is the
 * most that can happen without altering a single depth sample, so
 *
 *     dtheta = (cascade_extent / map_size) / max_caster_height
 *
 * Every degenerate input returns 0, and 0 means "re-render every frame".  That
 * direction is deliberate: a wrong quantum that is too SMALL costs some frame
 * time and someone will measure it, while one that is too LARGE freezes the
 * shadows and nothing reports it at all.
 */
float jce_shadow_sun_quantum(float cascade_extent_m, uint32_t map_size,
                             float max_caster_height_m);

/*
 * Decide whether the shadow maps must be re-rendered for a new sun direction.
 *
 * Returns true when the caller must re-render, having written the new anchor to
 * `out_dir`; returns false when the held direction still stands, having written
 * THAT to `out_dir`.  Either way `out_dir` is what the caller must build the
 * light VP from -- passing the raw sun through on a false return would change
 * the VP without re-rendering, which is the one combination that is wrong.
 *
 * The comparison is against the HELD direction, not the previous frame's.  A
 * per-frame delta looks equivalent and is not: motion slower than the quantum
 * never trips a per-frame delta, so the sun walks arbitrarily far from the map
 * that is still being displayed and the shadows detach from the light with no
 * frame ever showing a jump.
 */
bool jce_shadow_sun_update(JceShadowSunHold *hold, const float dir[3],
                           float quantum_rad, float out_dir[3]);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADOW_SUN_H */
