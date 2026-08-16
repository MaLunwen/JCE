#include "jce_water_caustics.h"

#include <math.h>

/* The brightest a caustic may get.  Chosen, not derived: the true gain
 * diverges at a fold, so SOME bound is mandatory and the only question is
 * where.  4x is roughly what a real water surface delivers at a metre depth,
 * and it is low enough that the bright bands still sit inside the tonemapper's
 * range instead of resolving to white. */
#define CAUSTIC_MAX_GAIN 4.0f

float jce_water_caustic_gain(float jacobian, float strength)
{
    if (!(strength > 0.0f) || !isfinite(strength)) return 1.0f;
    if (!isfinite(jacobian)) return 1.0f;

    /* A fold: the surface has passed through itself and there is no single
     * ray path to the floor.  Whitecaps are drawn there instead, and claiming
     * a caustic as well would double-count the same event -- bright foam with
     * a bright band under it, which looks like the foam is glowing. */
    if (jacobian <= 0.0f) return 1.0f;

    /* Compression focuses: gain is the reciprocal of the area scale. */
    float gain = 1.0f / jacobian;
    if (gain > CAUSTIC_MAX_GAIN) gain = CAUSTIC_MAX_GAIN;

    /* Blend from 1 by strength, so strength 0 is EXACTLY off.  Scaling the
     * gain directly instead would leave strength 0 dividing the floor by the
     * Jacobian at full force and only then multiplying by zero -- a different
     * expression that happens to agree at the endpoints and nowhere else. */
    return 1.0f + (gain - 1.0f) * strength;
}

float jce_water_caustic_depth_falloff(float depth_m, float max_depth_m)
{
    if (!(max_depth_m > 0.0f) || !isfinite(max_depth_m)) return 0.0f;
    if (!isfinite(depth_m) || depth_m <= 0.0f) return 1.0f;
    if (depth_m >= max_depth_m) return 0.0f;

    /* Smoothstep rather than linear: a linear fade has a visible kink where it
     * reaches zero, and on a sloping floor that kink is a straight line across
     * the sea bed that looks like a terrain seam. */
    const float t = depth_m / max_depth_m;
    const float u = 1.0f - t;
    return u * u * (3.0f - 2.0f * u);
}
