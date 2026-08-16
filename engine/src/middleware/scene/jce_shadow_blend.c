#include "jce_shadow_blend.h"

JceCascadeBlendMode jce_cascade_blend_mode(JceGpuTier tier,
                                           bool has_temporal_resolve)
{
    switch (tier) {
    case JCE_GPU_TIER_HIGH:
    case JCE_GPU_TIER_ULTRA:
        return JCE_CASCADE_BLEND_LERP;

    case JCE_GPU_TIER_MEDIUM:
        /* Dither is the point of this tier -- one PCF, no seam.  But it is a
         * stochastic technique, and without a temporal resolve there is nothing
         * to turn the noise back into a gradient.  Fall to HARD: see the header
         * for why not LERP. */
        return has_temporal_resolve ? JCE_CASCADE_BLEND_DITHER
                                    : JCE_CASCADE_BLEND_HARD;

    case JCE_GPU_TIER_LOW:
        return JCE_CASCADE_BLEND_HARD;

    default:
        /* An unrecognised tier is a bug elsewhere, and this is the wrong place
         * to gamble on it: HARD is the only mode that is affordable everywhere
         * and correct-looking nowhere-in-particular.  Guessing UPWARD here
         * would put a second PCF on hardware we failed to identify. */
        return JCE_CASCADE_BLEND_HARD;
    }
}

float jce_cascade_blend_band(JceCascadeBlendMode mode, JceGpuTier tier)
{
    switch (mode) {
    case JCE_CASCADE_BLEND_DITHER:
        /* Narrow by construction: dither trades a seam for noise, and the wider
         * the band the more of the screen is noisy. */
        return 0.05f;
    case JCE_CASCADE_BLEND_LERP:
        /* The shipped per-tier values, preserved.  See the header. */
        switch (tier) {
        case JCE_GPU_TIER_HIGH:
        case JCE_GPU_TIER_ULTRA:  return 0.22f;
        case JCE_GPU_TIER_MEDIUM: return 0.20f;
        default:                  return 0.18f;
        }
    case JCE_CASCADE_BLEND_HARD:
    default:
        return 0.0f;
    }
}
