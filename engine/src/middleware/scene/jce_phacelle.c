/*
 * jce_phacelle.c -- Phacelle directional stripe noise + Fast Gully Erosion.
 *
 * Technique credit: Rune Skovbo Johansen (Fast & Gorgeous Erosion Filter,
 * Phacelle directional noise); analytic-derivative gradient noise: Inigo
 * Quilez.  Independent clean-room implementation -- see jce_phacelle.h.
 */

#include "jce_phacelle.h"

#include <math.h>

#define JCE_PH_TWO_PI 6.28318530717958647692f

/* Bell floor.  0.011109 == exp(-4.5) == exp(-2 * 1.5^2).
 *
 * LOAD-BEARING, and it is not a fudge factor.  The kernel sums a 4x4 block of
 * cells (i,j in [-1,2]) whose pivots are jittered by at most +-0.5 inside
 * their unit square.  1.5 is therefore the CLOSEST a pivot belonging to a cell
 * OUTSIDE that window can possibly get to the sample point.  Subtracting the
 * bell's value at that distance makes the weight reach exactly zero there, so
 * cells enter and leave the window with zero weight and zero slope-of-weight.
 * That is the entire reason this noise has no grid-line discontinuities.
 *
 * Change the window size, the jitter range, or the -2 coefficient in the
 * exponent and this constant MUST be recomputed as exp(-2 * dmax^2), where
 * dmax = (half the window extent) - (max jitter), per axis in quadrature. */
#define JCE_PH_BELL_FLOOR 0.011109f

/* Beyond ~2^24 a float has no fractional bits left, so floor/frac decomposition
 * stops meaning anything and the noise would silently degenerate into a
 * constant with garbage cell indices.  Clamping the sampled cell coordinate
 * keeps the kernel well defined and the int32 cell arithmetic in range; the
 * documented consequence is that the field is flat outside this radius. */
#define JCE_PH_COORD_LIMIT 1.0e7f

/* -- numeric hygiene ------------------------------------------------------ */

/* NaN scrub.  (v == v) is false only for NaN. */
static float jce_ph_num(float v, float fallback)
{
    return (v == v) ? v : fallback;
}

static float jce_ph_clampf(float v, float lo, float hi)
{
    if (v < lo) { return lo; }
    if (v > hi) { return hi; }
    return v;
}

/* Scrub NaN to zero and clamp Inf/huge into +-limit, in that order. */
static float jce_ph_sanitize(float v, float limit)
{
    return jce_ph_clampf(jce_ph_num(v, 0.0f), -limit, limit);
}

static float jce_ph_sat(float v)
{
    if (!(v > 0.0f)) { return 0.0f; }  /* NaN lands here too */
    if (v > 1.0f)    { return 1.0f; }
    return v;
}

/* Unit vector with a deterministic fallback.  A zero-length input has no
 * direction, and picking a fixed one is what lets the "assumed slope" idea
 * work at all: on perfectly flat input the filter still has an axis to run
 * gullies along instead of producing NaN. */
static void jce_ph_norm2(float x, float y, float *ox, float *oy)
{
    float m2, m;
    /* 1e18^2 == 1e36 stays well inside float range, so the square cannot
     * overflow to Inf before the length test sees it. */
    x = jce_ph_sanitize(x, 1.0e18f);
    y = jce_ph_sanitize(y, 1.0e18f);
    m2 = x * x + y * y;
    if (!(m2 > 1.0e-30f)) {
        *ox = 1.0f;
        *oy = 0.0f;
        return;
    }
    m = sqrtf(m2);
    *ox = x / m;
    *oy = y / m;
}

static int32_t jce_ph_floor_i32(float v)
{
    float f = floorf(jce_ph_sanitize(v, JCE_PH_COORD_LIMIT));
    return (int32_t)f;
}

/* -- integer hash --------------------------------------------------------- */

/* Pure integer avalanche (wang/PCG family finaliser).  Explicitly NOT
 * frac(sin(x)*k): that depends on the platform's transcendental precision and
 * diverges across backends, and this codebase hash-compares runs. */
static uint32_t jce_ph_hash_u32(uint32_t x)
{
    x ^= x >> 16;
    x *= 0x7feb352du;
    x ^= x >> 15;
    x *= 0x846ca68bu;
    x ^= x >> 16;
    return x;
}

/* Two independent values in [-1,1) for an integer cell.  The mantissa trick
 * (h >> 8) * 2^-23 - 1 is exact in binary32, so it introduces no rounding of
 * its own. */
static void jce_ph_hash2(int32_t cx, int32_t cy, uint32_t seed,
                         float *ox, float *oy)
{
    uint32_t h = jce_ph_hash_u32(((uint32_t)cx * 0x9e3779b1u) ^
                                 jce_ph_hash_u32(((uint32_t)cy * 0x85ebca6bu) ^ seed));
    uint32_t a = jce_ph_hash_u32(h ^ 0x68bc21ebu);
    uint32_t b = jce_ph_hash_u32(h ^ 0x02e5be93u);
    *ox = (float)(a >> 8) * (1.0f / 8388608.0f) - 1.0f;
    *oy = (float)(b >> 8) * (1.0f / 8388608.0f) - 1.0f;
}

/* Under integer lacunarity floor(p * freq) coincides across octaves, so the
 * SAME cell hash would be re-sampled at nested scales and the octaves would
 * visibly rhyme.  Re-hashing the seed per octave decorrelates them. */
static uint32_t jce_ph_octave_seed(uint32_t seed, int octave)
{
    return jce_ph_hash_u32(seed + 0x9e3779b9u * (uint32_t)(octave + 1));
}

/* -- public scalar helpers ------------------------------------------------ */

float jce_phacelle_clamp_normalization(float normalization)
{
    return jce_ph_clampf(jce_ph_num(normalization, 0.0f), 0.0f, 0.9f);
}

float jce_phacelle_bell(float dist_sq)
{
    float w;
    dist_sq = jce_ph_sanitize(dist_sq, 1.0e18f);
    if (dist_sq < 0.0f) { dist_sq = 0.0f; }
    /* expf underflows to +0 for large arguments, so no Inf can appear here. */
    w = expf(-2.0f * dist_sq) - JCE_PH_BELL_FLOOR;
    return (w > 0.0f) ? w : 0.0f;
}

float jce_phacelle_ease_out(float t)
{
    float u = 1.0f - jce_ph_sat(t);
    return 1.0f - u * u;
}

float jce_phacelle_smooth_start(float t, float s)
{
    /* Inputs are magnitudes; a negative t has no meaning and the quadratic
     * branch would fold it back to positive, so it is floored instead. */
    t = jce_ph_num(t, 0.0f);
    s = jce_ph_num(s, 0.0f);
    if (!(t > 0.0f)) { return 0.0f; }
    if (!(s > 1.0e-6f)) { return t; }   /* no rounding requested: plain ramp */
    if (t >= s) { return t - 0.5f * s; }
    /* Both branches meet at t == s with value s/2 and slope 1, which is the
     * point: the corner of the ramp is rounded C1 so the mask has no kink. */
    return 0.5f * t * t / s;
}

float jce_phacelle_pow_inv(float t, float d)
{
    float base;
    t = jce_ph_sat(t);
    d = jce_ph_num(d, 1.0f);
    /* d <= 0 would make 0^d infinite; d beyond ~64 is numerically pointless. */
    d = jce_ph_clampf(d, 1.0e-2f, 64.0f);
    base = 1.0f - t;
    if (base <= 0.0f) { return 1.0f; }
    return 1.0f - powf(base, d);
}

/* -- the kernel ----------------------------------------------------------- */

void jce_phacelle_sample(float px, float py,
                         float dir_x, float dir_y,
                         float freq, float offset,
                         float normalization, uint32_t seed,
                         JcePhacelleSample *out)
{
    float dx, dy, sx, sy, kx, ky, phase0;
    float fx, fy, c, s, mag, mag_floor;
    float acc_c = 0.0f, acc_s = 0.0f, acc_w = 0.0f;
    int32_t ix, iy;
    int i, j;

    if (!out) { return; }
    out->c = 0.0f;
    out->s = 0.0f;
    out->side_x = 1.0f;
    out->side_y = 0.0f;

    px     = jce_ph_sanitize(px, JCE_PH_COORD_LIMIT);
    py     = jce_ph_sanitize(py, JCE_PH_COORD_LIMIT);
    freq   = jce_ph_sanitize(freq, 1.0e4f);
    offset = jce_ph_sanitize(offset, 1.0e4f);
    normalization = jce_phacelle_clamp_normalization(normalization);

    /* Stripes run along d; the phase advances across it.  Folding 2*PI*freq
     * into k is what makes the gradient fall out of the phasor for free. */
    jce_ph_norm2(dir_x, dir_y, &dx, &dy);
    sx = -dy;                       /* perp((x,y)) = (-y,x) */
    sy =  dx;
    kx = sx * freq * JCE_PH_TWO_PI;
    ky = sy * freq * JCE_PH_TWO_PI;
    phase0 = offset * JCE_PH_TWO_PI;

    ix = jce_ph_floor_i32(px);
    iy = jce_ph_floor_i32(py);
    fx = px - (float)ix;
    fy = py - (float)iy;

    /* 4x4 window.  Worst case the nearest pivot is 1.0 away per axis
     * (own-cell pivot pushed to one edge, neighbour's to the other), i.e.
     * dist_sq <= 2.0 < 2.25, so acc_w is provably positive -- but the guard
     * below stays, because "provably" is not "checked". */
    for (j = -1; j <= 2; ++j) {
        for (i = -1; i <= 2; ++i) {
            float jx, jy, vx, vy, w, th;
            jce_ph_hash2(ix + i, iy + j, seed, &jx, &jy);
            vx = fx - (float)i - jx * 0.5f;   /* jitter in [-0.5,0.5]^2 */
            vy = fy - (float)j - jy * 0.5f;
            w = jce_phacelle_bell(vx * vx + vy * vy);
            if (w <= 0.0f) { continue; }
            /* Every cell shares frequency and direction and differs only in
             * phase, so the weighted sum is another sinusoid of the same
             * frequency with reduced amplitude.  That is why neighbouring
             * stripe sets blend instead of beating against each other. */
            th = vx * kx + vy * ky + phase0;
            acc_c += cosf(th) * w;
            acc_s += sinf(th) * w;
            acc_w += w;
        }
    }

    if (acc_w > 1.0e-20f) {
        c = acc_c / acc_w;
        s = acc_s / acc_w;
    } else {
        c = 0.0f;
        s = 0.0f;
    }

    /* PARTIAL normalisation: divide by the phasor length only where that
     * length exceeds (1 - normalization), otherwise by the floor.  Full
     * normalisation would amplify the ill-conditioned direction of a phasor
     * near the origin into loopy swirls.  The floor is >= 0.1 because the
     * exposed parameter is clamped to [0,0.9], so this division is safe by
     * construction, not by luck. */
    mag_floor = 1.0f - normalization;
    mag = sqrtf(c * c + s * s);
    if (mag < mag_floor) { mag = mag_floor; }

    out->c = c / mag;
    out->s = s / mag;
    out->side_x = sx;
    out->side_y = sy;
}

/* -- parameters ----------------------------------------------------------- */

void jce_phacelle_params_default(JcePhacelleParams *p)
{
    if (!p) { return; }
    p->seed          = 0x5eed0001u;
    p->octaves       = 8;
    p->frequency     = 1.0f;
    p->lacunarity    = 2.0f;
    p->strength      = 1.0f;
    p->gain          = 0.5f;
    p->cell_scale    = 0.5f;
    p->phase_offset  = 0.25f;
    p->normalization = 0.5f;   /* gain 2; tuned, see header */
    p->gully_weight  = 1.0f;
    p->detail        = 1.5f;   /* > 1: mask survives longer => more detail */
    p->onset         = 4.0f;
    p->rounding      = 0.5f;
    p->onset_gain    = 1.0f;
    p->rounding_gain = 1.0f;
    p->assumed_slope_magnitude = 1.0f;
    p->assumed_slope_blend     = 0.7f;
}

void jce_phacelle_params_sanitize(JcePhacelleParams *p)
{
    if (!p) { return; }
    if (p->octaves < 0) { p->octaves = 0; }
    if (p->octaves > JCE_PHACELLE_MAX_OCTAVES) { p->octaves = JCE_PHACELLE_MAX_OCTAVES; }

    p->frequency     = jce_ph_clampf(jce_ph_num(p->frequency,  1.0f), 0.0f, 1.0e6f);
    p->lacunarity    = jce_ph_clampf(jce_ph_num(p->lacunarity, 2.0f), 1.0e-2f, 8.0f);
    p->strength      = jce_ph_sanitize(p->strength, 1.0e6f);
    p->gain          = jce_ph_clampf(jce_ph_num(p->gain, 0.5f), 0.0f, 2.0f);
    p->cell_scale    = jce_ph_clampf(jce_ph_num(p->cell_scale, 0.5f), 0.0f, 64.0f);
    p->phase_offset  = jce_ph_sanitize(p->phase_offset, 1.0e3f);
    p->normalization = jce_phacelle_clamp_normalization(p->normalization);
    p->gully_weight  = jce_ph_clampf(jce_ph_num(p->gully_weight, 1.0f), 0.0f, 8.0f);
    p->detail        = jce_ph_clampf(jce_ph_num(p->detail, 1.0f), 1.0e-2f, 64.0f);
    p->onset         = jce_ph_clampf(jce_ph_num(p->onset, 1.0f), 0.0f, 1.0e4f);
    p->rounding      = jce_ph_clampf(jce_ph_num(p->rounding, 0.0f), 0.0f, 4.0f);
    p->onset_gain    = jce_ph_clampf(jce_ph_num(p->onset_gain, 1.0f), 0.0f, 4.0f);
    p->rounding_gain = jce_ph_clampf(jce_ph_num(p->rounding_gain, 1.0f), 0.0f, 4.0f);
    p->assumed_slope_magnitude =
        jce_ph_clampf(jce_ph_num(p->assumed_slope_magnitude, 1.0f), 0.0f, 1.0e6f);
    p->assumed_slope_blend = jce_ph_sat(p->assumed_slope_blend);
}

/* -- the filter ----------------------------------------------------------- */

void jce_phacelle_erode(const JcePhacelleParams *p,
                        float x, float z,
                        float base_h, float base_grad_x, float base_grad_z,
                        float *out_height, float *out_grad_x,
                        float *out_grad_z, float *out_ridge)
{
    JcePhacelleParams pr;
    float h, gx, gz;
    float gully_sx, gully_sy;      /* steering accumulator -- NOT the output */
    float fade_target = 0.0f;
    float mask = 1.0f;             /* octave 0 is unmasked: the mask is built
                                    * from the PREVIOUS octave's slope       */
    float ridge_acc = 0.0f, ridge_w = 0.0f;
    float strength, freq, onset, rounding;
    int oct;

    x      = jce_ph_sanitize(x, 1.0e12f);
    z      = jce_ph_sanitize(z, 1.0e12f);
    h      = jce_ph_sanitize(base_h, 1.0e12f);
    gx     = jce_ph_sanitize(base_grad_x, 1.0e12f);
    gz     = jce_ph_sanitize(base_grad_z, 1.0e12f);

    if (!p) {
        /* No parameters is not an error -- pass the input field through. */
        if (out_height) { *out_height = h; }
        if (out_grad_x) { *out_grad_x = gx; }
        if (out_grad_z) { *out_grad_z = gz; }
        if (out_ridge)  { *out_ridge  = 0.0f; }
        return;
    }
    pr = *p;
    jce_phacelle_params_sanitize(&pr);

    /* Assumed slope: blend the true gradient toward a unit-length one of fixed
     * magnitude, i.e. pretend the input has a consistent steepness.  Without
     * it, near-flat input gives the first octave no direction to run along and
     * the stripes swing with whatever numerical dust is in the gradient. */
    {
        float ux, uy, b, m;
        b = pr.assumed_slope_blend;
        m = pr.assumed_slope_magnitude;
        jce_ph_norm2(gx, gz, &ux, &uy);
        gully_sx = gx + (ux * m - gx) * b;
        gully_sy = gz + (uy * m - gz) * b;
    }

    strength = pr.strength;
    freq     = pr.frequency;
    onset    = pr.onset;
    rounding = pr.rounding;

    for (oct = 0; oct < pr.octaves; ++oct) {
        JcePhacelleSample ph;
        float dx, dy, kpx, kpy, sloping, sgn;
        float g_h, g_gx, g_gz, f_h, f_gx, f_gz, w_oct, new_mask;

        jce_ph_norm2(gully_sx, gully_sy, &dx, &dy);
        jce_phacelle_sample(x * freq, z * freq, dx, dy,
                            pr.cell_scale, pr.phase_offset,
                            pr.normalization, jce_ph_octave_seed(pr.seed, oct),
                            &ph);

        /* grad(cos(dot(p,k))) = -sin(...) * k; the 2*PI is dropped here and
         * absorbed into `strength`, so k' is just the unit side times -freq. */
        kpx = ph.side_x * (-freq);
        kpy = ph.side_y * (-freq);
        sloping = fabsf(ph.s);

        /* Explicitly NOT a signum that returns 0 at zero.  A zero here would
         * silently drop this octave's steering on a measure-zero set, and
         * different implementations disagree about sign(0) -- this codebase
         * hash-compares runs, so that divergence is a real bug. */
        sgn = (ph.s >= 0.0f) ? 1.0f : -1.0f;

        /* TWO SEPARATE SLOPE ACCUMULATORS, and the split is the technique.
         * This one is UNMASKED and SIGN-based (a triangle wave), and it only
         * ever steers the NEXT octave's stripe direction.  Steering with the
         * true slope makes the gullies meander instead of holding a line. */
        gully_sx += sgn * kpx * strength * pr.gully_weight;
        gully_sy += sgn * kpy * strength * pr.gully_weight;

        g_h  = ph.c;
        g_gx = ph.s * kpx;
        g_gz = ph.s * kpy;

        /* Flat-extremum guard.  Where the mask goes to zero the octave fades
         * to (fade_target, 0, 0): note the gradient part of the fade target is
         * exactly (0,0), so a masked-out region contributes NO gradient at
         * all.  That is what breaks the feedback loop at a peak/pit/saddle,
         * where grad h -> 0 and its DIRECTION is undefined. */
        f_h  = fade_target + (g_h * pr.gully_weight - fade_target) * mask;
        f_gx = g_gx * pr.gully_weight * mask;
        f_gz = g_gz * pr.gully_weight * mask;

        /* ...and this accumulator is the OUTPUT: masked, faded, true gradient.
         * Feeding it the sign-based increment instead would facet the surface. */
        h  += f_h  * strength;
        gx += f_gx * strength;
        gz += f_gz * strength;
        fade_target = f_h;

        /* Ridge map: one extra scalar pair (sum, weight).  cos is +1 on a
         * stripe crest and -1 in its trough, so the mask-weighted mean of the
         * octaves' real parts reads as +1 ridge / -1 crease.  |ph.c| <= 1 and
         * the weights are non-negative, so the quotient is in [-1,1]. */
        w_oct = fabsf(strength) * mask;
        ridge_acc += ph.c * w_oct;
        ridge_w   += w_oct;

        new_mask = jce_phacelle_ease_out(
                       jce_phacelle_smooth_start(sloping * onset, rounding * onset));
        mask = jce_phacelle_pow_inv(mask, pr.detail) * new_mask;

        strength = jce_ph_sanitize(strength * pr.gain, 1.0e12f);
        freq     = jce_ph_sanitize(freq * pr.lacunarity, 1.0e12f);
        onset    = jce_ph_sanitize(onset * pr.onset_gain, 1.0e12f);
        rounding = jce_ph_sanitize(rounding * pr.rounding_gain, 1.0e12f);
    }

    if (out_height) { *out_height = jce_ph_sanitize(h, 1.0e18f); }
    if (out_grad_x) { *out_grad_x = jce_ph_sanitize(gx, 1.0e18f); }
    if (out_grad_z) { *out_grad_z = jce_ph_sanitize(gz, 1.0e18f); }
    if (out_ridge) {
        float r = (ridge_w > 1.0e-20f) ? (ridge_acc / ridge_w) : 0.0f;
        *out_ridge = jce_ph_clampf(jce_ph_num(r, 0.0f), -1.0f, 1.0f);
    }
}
