#include "jce_water_shoreline.h"

#include <math.h>

float jce_water_shore_foam(float depth_m, float band_m)
{
    if (!(band_m > 0.0f) || !isfinite(band_m)) return 0.0f;
    if (!isfinite(depth_m)) return 0.0f;
    /* Behind the surface entirely (a caller with an inverted sign, or a floor
     * in front of the water): no foam.  Returning 1 here would paint the
     * brightest possible surf across whatever went wrong. */
    if (depth_m <= 0.0f) return 1.0f;
    if (depth_m >= band_m) return 0.0f;

    const float t = depth_m / band_m;

    /* Squared falloff.  Surf piles at the very edge and thins fast; a linear
     * ramp paints a uniform wedge whose outer edge is a hard line, and on a
     * shallow beach that line sits far offshore and reads as a seam in the
     * water rather than as foam ending. */
    const float u = 1.0f - t;
    return u * u;
}

float jce_water_shore_surge(float phase, float period_s)
{
    if (!(period_s > 0.0f) || !isfinite(period_s)) return 1.0f;
    if (!isfinite(phase)) return 1.0f;

    /* One smooth advance and retreat per period, spanning 0.7x to 1.3x of the
     * authored band.  The multiplier moves the effective WATERLINE, which is
     * what makes the foam run up the sand; modulating opacity instead would
     * make it fade in place like a blinking light. */
    const float w = sinf(phase * (6.28318531f / period_s));
    return 1.0f + 0.3f * w;
}
