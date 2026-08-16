/*
 * jce_water_shoreline.h -- foam where the water gets thin.
 *
 * A shoreline is not a place on a texture, it is a DEPTH: water is a shore
 * wherever the bottom comes close enough to the surface. Deriving it from the
 * water column's own thickness means it works on any body of any shape, follows
 * a rock in the middle of a lake, moves with the tide, and needs nothing
 * authored.
 *
 * What this replaces: the existing `shore_ripple` term measures
 * `length(v_texcoord0 * 2 - 1)` -- the radius from the centre of the quad --
 * and calls the rim of the mesh "the shore". That is exactly right for a
 * circular pond drawn on a square plane and wrong for everything else: a river,
 * an L-shaped harbour, or an island produce foam rings floating in open water
 * and no foam at all where the water actually meets land. Nothing about that
 * reads as a bug; it reads as a stylistic ripple effect.
 *
 * The threshold is in METRES, and that is the point. A threshold in UV or in
 * screen space gives a foam band whose physical width changes with the size of
 * the water body and with the viewing angle -- so the same setting is a thin
 * lace on a pond and a hundred-metre white shelf on an ocean.
 *
 * Layer: Scene (L4) -- PRIVATE. Pure arithmetic, no bgfx.
 */

#ifndef JCE_WATER_SHORELINE_H
#define JCE_WATER_SHORELINE_H

#ifdef __cplusplus
extern "C" {
#endif

/*
 * How much foam belongs at a point where the water column is `depth_m` thick.
 *
 * Returns [0,1]: 1 at the waterline, falling to 0 by `band_m`. Zero or
 * negative `band_m` disables the term exactly.
 *
 * The curve is deliberately not linear. Real surf piles up at the very edge and
 * thins fast, and a linear ramp instead paints a uniform grey-white wedge whose
 * outer edge is a hard line -- on a shallow beach that line sits far offshore
 * and looks like a texture seam in the water.
 */
float jce_water_shore_foam(float depth_m, float band_m);

/*
 * Animated surf: the foam band advances and retreats instead of sitting still.
 *
 * `phase` is a time in seconds; `period_s` is one full advance-and-retreat.
 * Returns a multiplier around 1 that moves the EFFECTIVE waterline in and out,
 * so the caller applies it to the band width rather than to the foam's opacity.
 *
 * Modulating opacity is the tempting shortcut and it is visibly wrong: the foam
 * would fade in place like a blinking light instead of running up the sand.
 */
float jce_water_shore_surge(float phase, float period_s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_WATER_SHORELINE_H */
