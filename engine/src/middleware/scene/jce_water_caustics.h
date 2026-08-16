/*
 * jce_water_caustics.h -- how bright the sea floor gets under a wave.
 *
 * Caustics are not a texture effect. A wave surface is a lens: where it
 * compresses horizontally it focuses the sunlight passing through it, and the
 * bright web on the floor is that focus. The quantity that measures the
 * compression is the horizontal Jacobian determinant of the displacement --
 * which this engine already computes, because a NEGATIVE Jacobian is the
 * physical definition of a fold, and folds are whitecaps.
 *
 * So caustics and whitecaps are the same measurement read at two ends: the
 * surface froths exactly where the light beneath it stops focusing and starts
 * crossing over itself. Deriving both from one number is what keeps them
 * consistent; a caustic texture scrolled independently of the waves is the
 * usual alternative, and it drifts out of step in a way that reads as the water
 * moving at two different speeds.
 *
 * Layer: Scene (L4) -- PRIVATE. Pure arithmetic, no bgfx.
 */

#ifndef JCE_WATER_CAUSTICS_H
#define JCE_WATER_CAUSTICS_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * Caustic gain from the surface Jacobian.
 *
 * Returns a MULTIPLIER on the light reaching the floor: 1 means unfocused,
 * greater than 1 is a bright band, less than 1 is the dim gap between bands.
 * Energy is not conserved across the map and is not meant to be -- the light
 * removed from the dim regions is what appears in the bright ones, but the
 * floor here is sampled per pixel and there is nothing to redistribute across.
 *
 *   jacobian  1.0  = undisturbed surface, no focusing, gain 1
 *   jacobian  ->0  = total compression, an infinitely bright line in theory
 *   jacobian  <0   = a FOLD; the surface has passed through itself and the
 *                    caustic caustic is undefined there
 *
 * `strength` scales the effect away from 1; 0 disables it exactly.
 *
 * The gain is CLAMPED, and the clamp is the whole reason this is a function
 * rather than a division at the call site. 1/jacobian diverges as the surface
 * approaches a fold, so an unclamped caustic produces a handful of pixels
 * thousands of times brighter than the scene -- which does not read as a bug,
 * it reads as bloom, and it will be "fixed" by turning bloom down.
 */
float jce_water_caustic_gain(float jacobian, float strength);

/*
 * Depth attenuation: caustics fade as the floor gets further from the surface.
 *
 * The focus forms at one distance below the wave; past it the rays have crossed
 * and spread again. Without this the bottom of a trench is as sharply patterned
 * as the shallows, which is the single clearest tell of a faked caustic.
 *
 * Returns 1 at the surface and falls smoothly to 0 by `max_depth_m`.
 */
float jce_water_caustic_depth_falloff(float depth_m, float max_depth_m);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WATER_CAUSTICS_H */
