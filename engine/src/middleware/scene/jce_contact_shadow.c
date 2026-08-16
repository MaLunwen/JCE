#include "jce_contact_shadow.h"

uint32_t jce_contact_shadow_steps(JceGpuTier tier)
{
    switch (tier) {
    case JCE_GPU_TIER_ULTRA:
    case JCE_GPU_TIER_HIGH:   return JCE_CONTACT_SHADOW_MAX_STEPS;   /* 16 */
    case JCE_GPU_TIER_MEDIUM: return 8u;
    case JCE_GPU_TIER_LOW:    return 0u;
    default:
        /* Unidentified hardware gets the cheap answer.  Guessing upward here
         * adds depth fetches per pixel on a machine nobody has profiled. */
        return 0u;
    }
}

bool jce_contact_shadow_jitter(bool has_temporal_resolve)
{
    return has_temporal_resolve;
}

float jce_contact_shadow_ray_length(float cascade0_radius_m, uint32_t map_size)
{
    if (!(cascade0_radius_m > 0.0f) || map_size == 0u) return 0.0f;

    /* One cascade-0 texel, in world units... */
    const float texel = 2.0f * cascade0_radius_m / (float)map_size;
    /* ...and the ray spans a small multiple of it.  8 texels is the distance
     * over which a normal-offset bias large enough to kill acne can push a
     * contact point, which is precisely the error being corrected. */
    float len = texel * 8.0f;

    /* Clamped at both ends.  Too short and the march cannot reach past the
     * bias it exists to undo; too long and it starts producing shadows the
     * depth buffer has no information to support -- a screen-space ray only
     * knows about the first depth layer, so a long one invents occlusion
     * behind every silhouette. */
    if (len < 0.02f) len = 0.02f;
    if (len > 0.50f) len = 0.50f;
    return len;
}
