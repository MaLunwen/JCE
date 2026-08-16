/*
 * jce_water_optics.h -- how far light gets through water.
 *
 * Water does not dim light; it dims RED light. Clear water attenuates red
 * roughly twenty times faster than blue, which is the entire reason deep water
 * looks blue and a red object two metres down looks brown. A scalar extinction
 * -- one number for all three channels -- produces water that goes GREY with
 * depth, which reads as fog in a swimming pool rather than as water, and which
 * no amount of tuning the water's surface colour fixes.
 *
 * That per-channel split is the whole content of this file, and it is the thing
 * that was missing: the surface shader lerped between an authored shallow and
 * deep colour by view angle, so depth never entered the result at all. A
 * shallow pool and an ocean trench rendered identically as long as you looked
 * at them from the same angle.
 *
 * Layer: Scene (L4) -- PRIVATE. Pure arithmetic, no bgfx.
 */

#ifndef JCE_WATER_OPTICS_H
#define JCE_WATER_OPTICS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Per-channel extinction coefficients, in inverse metres. */
typedef struct JceWaterExtinction {
    float sigma[3];   /* r, g, b */
} JceWaterExtinction;

/*
 * Derive the three coefficients from ONE authorable number.
 *
 * `clarity_m` is roughly how far you can see through the water -- the distance
 * at which the green channel has fallen to about 1/e. Designers can judge that
 * by eye; they cannot judge three extinction coefficients, and exposing the raw
 * numbers is how water ends up tuned into something that is not any real
 * liquid. The RATIO between channels is fixed to clear natural water (red is
 * absorbed far faster than blue), so changing clarity makes water murkier or
 * cleaner without changing what it IS.
 *
 * Returns coefficients for very clear water when `clarity_m` is not positive:
 * failing toward transparent means a misconfigured value looks like water that
 * has not been tuned, rather than like an opaque black slab that hides the
 * scene and looks deliberate.
 */
JceWaterExtinction jce_water_extinction_from_clarity(float clarity_m);

/*
 * Beer-Lambert transmittance along a path: T = exp(-sigma * distance).
 *
 * `path_m` is the distance travelled THROUGH WATER, not the distance to the
 * camera. Using the latter tints objects by how far away they are rather than
 * by how much water is in front of them, so a distant object beside a pond is
 * as blue as one at the bottom of it.
 *
 * Negative or zero path returns exactly 1 in every channel. It must be exact:
 * a waterline where transmittance starts at 0.99 instead of 1.0 draws a visible
 * band along every shore.
 */
void jce_water_transmittance(const JceWaterExtinction *e, float path_m,
                             float out_rgb[3]);

/*
 * The colour arriving at the viewer from a point `path_m` deep in water.
 *
 * Two terms: what survives of the background, plus what the water itself
 * scatters back in. The second is what keeps deep water BLUE instead of BLACK
 * -- extinction alone drives every channel to zero, and water that goes black
 * with depth looks like a hole in the world rather than like depth.
 */
void jce_water_apply_absorption(const JceWaterExtinction *e, float path_m,
                                const float background_rgb[3],
                                const float water_tint_rgb[3],
                                float out_rgb[3]);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WATER_OPTICS_H */
