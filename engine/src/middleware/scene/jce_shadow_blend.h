/*
 * jce_shadow_blend.h -- how two cascades meet.
 *
 * Without blending, the boundary between cascades is a hard line across the
 * ground where the shadow filter width changes.  Three ways to hide it, and the
 * design fixes which tier gets which:
 *
 *   HARD    a straight switch.  Cheapest; the seam is visible.
 *   DITHER  interleaved-gradient noise picks one cascade or the other per pixel
 *           over a narrow band.  One PCF, seam gone -- but the result is NOISE,
 *           and it only resolves into a gradient if something averages it over
 *           time.
 *   LERP    sample both cascades in the band and mix.  Correct and smooth;
 *           costs a second full PCF exactly in the band.
 *
 * The trap this module exists to close: DITHER is only valid with a temporal
 * resolve.  This engine drops TAA to FXAA without a discrete GPU, and dither
 * with nothing to resolve it is screen-door noise -- strictly worse than the
 * banding it replaced, and it ships looking like a broken shadow filter rather
 * than like a missing feature.
 *
 * The fallback is HARD, not LERP.  LERP is the tempting choice because it is
 * the correct one, but it doubles the filter cost in the band on precisely the
 * hardware that had no temporal resolve because it has no headroom.  A quality
 * fallback that costs more than the tier above it is not a fallback.
 *
 * Layer: Scene (L4) -- PRIVATE.  Pure policy, no bgfx.
 */

#ifndef JCE_SHADOW_BLEND_H
#define JCE_SHADOW_BLEND_H

#include <stdbool.h>

#include "jce/renderer/jce_renderer_caps.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JceCascadeBlendMode {
    JCE_CASCADE_BLEND_HARD   = 0,
    JCE_CASCADE_BLEND_DITHER = 1,
    JCE_CASCADE_BLEND_LERP   = 2
} JceCascadeBlendMode;

/*
 * `has_temporal_resolve` must come from the render pipeline's enable_taa -- the
 * one place that decides it -- and not from the tier or from has_discrete_gpu.
 * Those correlate today and a user can force TAA off tomorrow, at which point a
 * second copy of the rule would keep dithering into a buffer nothing resolves.
 */
JceCascadeBlendMode jce_cascade_blend_mode(JceGpuTier tier,
                                           bool has_temporal_resolve);

/*
 * Fraction of a cascade's depth range used as the blend band.  0 for HARD.
 *
 * The LERP bands are the values this engine already shipped per tier (0.22 /
 * 0.20 / 0.18), NOT the 10% the design document specifies.  That gap is
 * deliberate and it is not resolved here: narrowing the band is a change to
 * what the shadows LOOK like, and the only honest way to pick between 0.22 and
 * 0.10 is to render both and compare.  This module exists to fix the mode
 * selection, which is a correctness question and answerable without a
 * screenshot; the band width is a taste question and is left where it was.
 */
float jce_cascade_blend_band(JceCascadeBlendMode mode, JceGpuTier tier);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SHADOW_BLEND_H */
