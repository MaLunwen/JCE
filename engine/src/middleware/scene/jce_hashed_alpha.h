/*
 * jce_hashed_alpha.h -- stochastic alpha testing (Wyman & McGuire 2017).
 *
 * A fixed alpha cutoff makes foliage POP. As a leaf card recedes its alpha mask
 * mips down, whole leaves cross the threshold at once, and a tree loses a
 * visible chunk of canopy in a single frame. Every LOD transition does it, so
 * it reads as the LOD system being badly tuned rather than as the alpha test.
 *
 * Hashed alpha replaces the constant threshold with a per-fragment one drawn
 * from a hash of the fragment's OBJECT-SPACE position. Coverage then converges
 * smoothly as the mask blurs, because the fraction of fragments surviving a
 * uniformly-distributed threshold IS the alpha value.
 *
 * TWO THINGS THAT MAKE OR BREAK IT:
 *
 * 1. The hash must key on OBJECT space, not world space. Foliage here is
 *    wind-animated, so world position moves every frame -- a world-space hash
 *    would resample the noise as the leaf sways and the stipple would crawl
 *    across the surface. Object space is invariant under both the wind and the
 *    camera, which is what makes the pattern sit still on the leaf.
 *
 * 2. The threshold must be scaled to the DERIVATIVE of that position, so the
 *    noise stays at a constant size on screen. A hash of raw object space
 *    aliases into per-pixel static the moment the object is more than a few
 *    metres away -- which looks like a broken texture rather than like an alpha
 *    test, and is exactly what the paper's discretised scaling exists to fix.
 *
 * Layer: Scene (L4) -- PRIVATE. The GPU twin lives in pbr/hashed_alpha.sh; this
 * is the CPU reference the tests pin, because the properties that matter are
 * statistical and a shader cannot be asked about them.
 */

#ifndef JCE_HASHED_ALPHA_H
#define JCE_HASHED_ALPHA_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Uniformly-distributed hash of an object-space position, in [0,1). */
float jce_hashed_alpha_hash3(float x, float y, float z);

/*
 * The per-fragment alpha threshold.
 *
 * `pix_deriv` is the object-space distance covered by one pixel (the max of the
 * screen-space derivatives). `scale` is the noise size in pixels; the paper's
 * useful range is roughly 0.3 to 1.5.
 *
 * The returned threshold is clamped strictly above 0 and at or below 1. Zero
 * would make a fully transparent fragment survive -- one pixel of a leaf's
 * empty margin drawn opaque, which appears as a sparse haze of stray dots that
 * no one attributes to the alpha test.
 */
float jce_hashed_alpha_threshold(float x, float y, float z,
                                 float pix_deriv, float scale);

#ifdef __cplusplus
}
#endif

#endif /* JCE_HASHED_ALPHA_H */
