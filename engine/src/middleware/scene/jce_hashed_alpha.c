#include "jce_hashed_alpha.h"

#include <math.h>

/* Hoskins hash: fract-only, deliberately NO sin().
 *
 * sin() of a large argument loses precision differently on GL/GLES than on
 * D3D, so a sin-based hash produces a DIFFERENT stipple pattern per backend
 * far from the origin -- and since this drives an alpha test, that is different
 * geometry, not merely different noise.  Same hazard class the water shader's
 * hash already avoids. */
float jce_hashed_alpha_hash3(float x, float y, float z)
{
    float px = x * 0.1031f, py = y * 0.1030f, pz = z * 0.0973f;
    px -= floorf(px); py -= floorf(py); pz -= floorf(pz);

    const float d = px * (py + 33.33f) + py * (pz + 33.33f) + pz * (px + 33.33f);
    px += d; py += d; pz += d;

    float r = (px + py) * pz;
    r -= floorf(r);
    /* floorf can return the input for huge magnitudes, leaving r outside
     * [0,1); a threshold above 1 would discard a fully opaque fragment. */
    if (!(r >= 0.0f)) r = 0.0f;
    if (r >= 1.0f)    r = 0.999999f;
    return r;
}

float jce_hashed_alpha_threshold(float x, float y, float z,
                                 float pix_deriv, float scale)
{
    /* Without a usable derivative there is no way to hold the noise at a
     * constant screen size, and unscaled object-space noise aliases into
     * per-pixel static.  Fall back to the midpoint: an ordinary 0.5 alpha
     * test, which is a defensible picture rather than a broken one. */
    if (!(pix_deriv > 0.0f) || !isfinite(pix_deriv) ||
        !(scale > 0.0f)     || !isfinite(scale))
        return 0.5f;

    const float pix_scale = 1.0f / (scale * pix_deriv);

    /* Two nearest LOG-DISCRETISED noise scales, and a lerp between them.
     * Snapping to one scale is what makes the stipple visibly change size as
     * the object moves; the paper's whole stability result is this pair. */
    const float lg = log2f(pix_scale);
    const float s_lo = exp2f(floorf(lg));
    const float s_hi = exp2f(ceilf(lg));

    const float a0 = jce_hashed_alpha_hash3(floorf(s_lo * x), floorf(s_lo * y),
                                            floorf(s_lo * z));
    const float a1 = jce_hashed_alpha_hash3(floorf(s_hi * x), floorf(s_hi * y),
                                            floorf(s_hi * z));

    float lerp_t = lg - floorf(lg);
    const float v = (1.0f - lerp_t) * a0 + lerp_t * a1;

    /* The lerp of two uniform variables is TRIANGULAR, not uniform, and a
     * non-uniform threshold biases coverage: the surviving fraction stops
     * equalling alpha, so the foliage systematically thins or thickens with
     * distance.  This CDF maps it back to uniform -- it is the step that makes
     * the technique correct rather than merely dithered. */
    const float a = (lerp_t < 1.0f - lerp_t) ? lerp_t : (1.0f - lerp_t);
    float t;
    if (a <= 0.0f) {
        t = v;                                   /* degenerate: already uniform */
    } else if (v < a) {
        t = v * v / (2.0f * a * (1.0f - a));
    } else if (v < 1.0f - a) {
        t = (v - 0.5f * a) / (1.0f - a);
    } else {
        const float w = 1.0f - v;
        t = 1.0f - w * w / (2.0f * a * (1.0f - a));
    }

    /* Never exactly 0: a zero threshold passes a fully transparent fragment,
     * which draws a stray opaque dot in a leaf's empty margin. */
    if (t < 1.0e-6f) t = 1.0e-6f;
    if (t > 1.0f)    t = 1.0f;
    return t;
}
