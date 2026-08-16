#include "jce_water_optics.h"

#include <math.h>

/* Channel ratios for clear natural water, normalised so green == 1.
 *
 * Red is absorbed roughly five times faster than green and green roughly four
 * times faster than blue; that ordering is what makes water blue, and it is
 * fixed here rather than authorable.  A project that could invert it could
 * author "water" that goes red with depth, which is not a style -- it is a
 * different substance. */
#define WATER_SIGMA_R_RATIO  5.0f
#define WATER_SIGMA_G_RATIO  1.0f
#define WATER_SIGMA_B_RATIO  0.25f

JceWaterExtinction jce_water_extinction_from_clarity(float clarity_m)
{
    JceWaterExtinction e;

    /* Fail toward CLEAR.  A misconfigured clarity then looks like water nobody
     * has tuned yet; failing toward opaque would hide the scene behind a black
     * slab that looks deliberate. */
    float c = clarity_m;
    if (!(c > 0.0f)) c = 100.0f;

    /* Green falls to 1/e at `clarity_m`, by definition of the parameter. */
    const float sigma_g = 1.0f / c;
    e.sigma[0] = sigma_g * WATER_SIGMA_R_RATIO;
    e.sigma[1] = sigma_g * WATER_SIGMA_G_RATIO;
    e.sigma[2] = sigma_g * WATER_SIGMA_B_RATIO;
    return e;
}

void jce_water_transmittance(const JceWaterExtinction *e, float path_m,
                             float out_rgb[3])
{
    if (!out_rgb) return;
    if (!e || !(path_m > 0.0f)) {
        /* Exactly 1, not nearly 1: a waterline that starts at 0.99 draws a
         * visible band along every shore. */
        out_rgb[0] = out_rgb[1] = out_rgb[2] = 1.0f;
        return;
    }
    for (int i = 0; i < 3; ++i) {
        const float s = (e->sigma[i] > 0.0f) ? e->sigma[i] : 0.0f;
        out_rgb[i] = expf(-s * path_m);
    }
}

void jce_water_apply_absorption(const JceWaterExtinction *e, float path_m,
                                const float background_rgb[3],
                                const float water_tint_rgb[3],
                                float out_rgb[3])
{
    if (!out_rgb) return;

    float t[3];
    jce_water_transmittance(e, path_m, t);

    for (int i = 0; i < 3; ++i) {
        const float bg   = background_rgb ? background_rgb[i] : 0.0f;
        const float tint = water_tint_rgb ? water_tint_rgb[i] : 0.0f;
        /* Survived background + in-scattered water colour.  The second term is
         * (1 - T), so it grows exactly as fast as the first shrinks: the result
         * converges to the water's own colour with depth rather than to black,
         * and it is continuous at path 0 (pure background, no tint). */
        out_rgb[i] = bg * t[i] + tint * (1.0f - t[i]);
    }
}
