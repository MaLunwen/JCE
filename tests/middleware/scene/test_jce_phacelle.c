/* test_jce_phacelle.c
 *
 * Property tests for the Phacelle stripe kernel and the gully erosion filter.
 *
 * These assert INVARIANTS, not golden samples: a re-tune of the defaults must
 * keep every test green, while a broken kernel must break one.  Verified by
 * mutation: a missing/wrong bell floor, a 3x3 window, absent pivot jitter,
 * full instead of partial normalisation, a missing normalization clamp, a
 * sign-flipped or reciprocated `detail`, a non-C1 smooth_start, cos-for-sin in
 * the gradient, a flipped k', an unmasked gradient, a stale fade target, a
 * non-perpendicular side vector, a scaled ridge, and any parameter that stops
 * reaching the output are each caught by at least one test below.
 *
 * KNOWN NOT COVERED, deliberately.  Two things the header calls load-bearing
 * cannot be pinned by sampling and are held by code review alone:
 *   - a signum returning 0 at exactly s == 0 differs only on a measure-zero
 *     set, so no finite sample grid can observe it;
 *   - steering the next octave with the true slope instead of the sign slope
 *     changes only how much the gullies meander, which is an aesthetic
 *     property with no invariant to bind it to.
 */

#include "jce_phacelle.h"

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1.0e-5f

/* -- 1. NULL / degenerate input ---------------------------------------- */

static void test_null_and_degenerate_input_is_survivable(void)
{
    JcePhacelleParams p;
    JcePhacelleSample s;
    float h = -1.0f, gx = -1.0f, gz = -1.0f, r = -7.0f;

    /* NULL sinks on every entry point must not crash. */
    jce_phacelle_params_default(NULL);
    jce_phacelle_params_sanitize(NULL);
    jce_phacelle_sample(1.0f, 2.0f, 1.0f, 0.0f, 0.5f, 0.25f, 0.5f, 3u, NULL);

    /* NULL params == pass-through, exactly. */
    jce_phacelle_erode(NULL, 3.0f, 4.0f, 5.5f, 0.25f, -0.75f, &h, &gx, &gz, &r);
    TEST_ASSERT_EQUAL_FLOAT(5.5f, h);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, gx);
    TEST_ASSERT_EQUAL_FLOAT(-0.75f, gz);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r);

    /* Every out pointer optional. */
    jce_phacelle_params_default(&p);
    jce_phacelle_erode(&p, 1.0f, 1.0f, 0.0f, 1.0f, 0.0f, NULL, NULL, NULL, NULL);

    /* Zero octaves == identity, and a zero direction must not produce NaN. */
    p.octaves = 0;
    h = gx = gz = r = 123.0f;
    jce_phacelle_erode(&p, 1.0f, 1.0f, 2.0f, 0.5f, 0.5f, &h, &gx, &gz, &r);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, h);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, gx);
    TEST_ASSERT_EQUAL_FLOAT(0.5f, gz);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, r);

    jce_phacelle_sample(0.5f, 0.5f, 0.0f, 0.0f, 0.5f, 0.25f, 0.5f, 3u, &s);
    TEST_ASSERT_TRUE(isfinite(s.c) && isfinite(s.s));
    TEST_ASSERT_TRUE(isfinite(s.side_x) && isfinite(s.side_y));
}

/* -- 2. the load-bearing bell floor ------------------------------------ */

static void test_bell_reaches_zero_at_the_window_boundary(void)
{
    int i;
    float prev;

    /* 1.5 is the closest an OUTSIDE-the-window pivot can be, given +-0.5
     * jitter in a 4x4 block, so the weight must be exactly zero at 1.5^2 and
     * beyond -- that is what makes cells enter and leave with zero weight. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_bell(2.25f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_bell(3.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_bell(1.0e9f));

    /* It must arrive at zero smoothly, not fall off a cliff. */
    TEST_ASSERT_TRUE(jce_phacelle_bell(2.24f) > 0.0f);
    TEST_ASSERT_TRUE(jce_phacelle_bell(2.24f) < 1.0e-3f);

    /* The worst-case in-window distance is 1.0 per axis => dist_sq 2.0, which
     * must still carry weight or the sum could be empty. */
    TEST_ASSERT_TRUE(jce_phacelle_bell(2.0f) > 0.0f);

    /* Monotone non-increasing, positive at the centre, never negative. */
    TEST_ASSERT_TRUE(jce_phacelle_bell(0.0f) > 0.9f);
    prev = jce_phacelle_bell(0.0f);
    for (i = 1; i <= 300; ++i) {
        float w = jce_phacelle_bell((float)i * 0.01f);
        TEST_ASSERT_TRUE(w >= 0.0f);
        TEST_ASSERT_TRUE(w <= prev + EPS);
        prev = w;
    }
    /* Degenerate arguments stay finite. */
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_bell(-5.0f)));
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_bell(NAN)));
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_bell(INFINITY)));
}

/* -- 3. no grid-line discontinuity ------------------------------------- */

static void test_kernel_is_continuous_across_a_cell_boundary(void)
{
    const int N = 401;                /* step 1e-3 across the seam at 1.0 */
    float worst = 0.0f;
    uint32_t seed;
    int k, n;

    /* Sweep BOTH seams over many offsets and seeds: a window that drops a cell
     * with non-zero weight only misbehaves where that cell's jittered pivot
     * happens to sit near the sample, so a single scan line proves nothing. */
    for (seed = 1u; seed <= 6u; ++seed) {
        for (k = 0; k < 24; ++k) {
            float other = -3.0f + 0.29f * (float)k;
            int axis;
            for (axis = 0; axis < 2; ++axis) {
                float prev_c = 0.0f, prev_s = 0.0f;
                for (n = 0; n < N; ++n) {
                    JcePhacelleSample s;
                    float t = 0.8f + 0.4f * (float)n / (float)(N - 1);
                    float px = (axis == 0) ? t : other;
                    float py = (axis == 0) ? other : t;
                    jce_phacelle_sample(px, py, 0.6f, 0.8f, 0.5f, 0.25f, 0.5f, seed, &s);
                    if (n > 0) {
                        float d = fabsf(s.c - prev_c) + fabsf(s.s - prev_s);
                        if (d > worst) { worst = d; }
                    }
                    prev_c = s.c;
                    prev_s = s.s;
                }
            }
        }
    }
    /* The field's own slope over a 1e-3 step is <0.01; a cell popping in with
     * non-zero weight is a step an order of magnitude larger. */
    TEST_ASSERT_TRUE(worst < 0.05f);

    /* The pivot jitter must actually be applied: without it the kernel is a
     * regular lattice and the field repeats exactly every cell. */
    for (k = 0; k < 50; ++k) {
        JcePhacelleSample a, b;
        float px = 0.13f * (float)k;
        jce_phacelle_sample(px,        0.7f, 1.0f, 0.0f, 0.5f, 0.25f, 0.5f, 5u, &a);
        jce_phacelle_sample(px + 1.0f, 0.7f, 1.0f, 0.0f, 0.5f, 0.25f, 0.5f, 5u, &b);
        TEST_ASSERT_TRUE(fabsf(a.c - b.c) + fabsf(a.s - b.s) > 1.0e-3f);
        jce_phacelle_sample(0.7f, px,        1.0f, 0.0f, 0.5f, 0.25f, 0.5f, 5u, &a);
        jce_phacelle_sample(0.7f, px + 1.0f, 1.0f, 0.0f, 0.5f, 0.25f, 0.5f, 5u, &b);
        TEST_ASSERT_TRUE(fabsf(a.c - b.c) + fabsf(a.s - b.s) > 1.0e-3f);
    }
}

/* -- 4. partial normalisation ------------------------------------------ */

static void test_partial_normalisation_is_clamped_and_safe(void)
{
    int ix, iy;

    /* The exposed parameter is clamped so the divisor can never reach zero. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_clamp_normalization(-3.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.9f, jce_phacelle_clamp_normalization(1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.9f, jce_phacelle_clamp_normalization(1.0e30f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_clamp_normalization(NAN));
    TEST_ASSERT_EQUAL_FLOAT(0.5f, jce_phacelle_clamp_normalization(0.5f));

    {
        const float dir_x = 0.3f, dir_y = -0.9f;
        float dlen = sqrtf(dir_x * dir_x + dir_y * dir_y);
        float ux = dir_x / dlen, uy = dir_y / dlen;
        float m_raw_max = 0.0f;

        for (iy = 0; iy < 24; ++iy) {
            for (ix = 0; ix < 24; ++ix) {
                JcePhacelleSample raw, part;
                float px = -3.0f + 0.31f * (float)ix;
                float py =  2.0f + 0.29f * (float)iy;
                float m_raw, m_part;

                /* 9.0 is deliberately out of range: it must be clamped to 0.9,
                 * not turned into a divide by zero. */
                jce_phacelle_sample(px, py, dir_x, dir_y, 0.5f, 0.25f, 0.0f, 11u, &raw);
                jce_phacelle_sample(px, py, dir_x, dir_y, 0.5f, 0.25f, 9.0f, 11u, &part);

                TEST_ASSERT_TRUE(isfinite(part.c) && isfinite(part.s));

                m_raw  = sqrtf(raw.c * raw.c + raw.s * raw.s);
                m_part = sqrtf(part.c * part.c + part.s * part.s);
                if (m_raw > m_raw_max) { m_raw_max = m_raw; }

                /* Output phasor is never longer than unit ... */
                TEST_ASSERT_TRUE(m_part <= 1.0f + EPS);
                TEST_ASSERT_TRUE(m_raw  <= 1.0f + EPS);
                /* ... and partial normalisation only ever lengthens it. */
                TEST_ASSERT_TRUE(m_part >= m_raw - EPS);

                /* The stripe direction and the phase direction must be exactly
                 * perpendicular -- k = perp(d) -- or the stripes shear. */
                TEST_ASSERT_FLOAT_WITHIN(1.0e-5f, 0.0f,
                                         part.side_x * ux + part.side_y * uy);
                /* side is a unit vector: the erosion loop scales it by freq. */
                TEST_ASSERT_FLOAT_WITHIN(
                    1.0e-4f, 1.0f,
                    sqrtf(part.side_x * part.side_x + part.side_y * part.side_y));
            }
        }
        /* PARTIAL, not full: with normalization == 0 the phasor must keep its
         * own amplitude, so it cannot be unit length everywhere.  Forcing it to
         * unit is the swirl-artifact failure mode this parameter exists to
         * avoid, and it would otherwise pass every bound above. */
        TEST_ASSERT_TRUE(m_raw_max < 0.99f);
    }
}

/* -- 5. pow_inv direction (the counterintuitive one) ------------------- */

static void test_pow_inv_direction_is_more_detail_for_larger_exponent(void)
{
    int i;
    for (i = 1; i <= 9; ++i) {
        float t = 0.1f * (float)i;
        /* d > 1 => the mask survives LONGER => MORE detail. */
        TEST_ASSERT_TRUE(jce_phacelle_pow_inv(t, 2.0f) > t + 1.0e-4f);
        TEST_ASSERT_TRUE(jce_phacelle_pow_inv(t, 4.0f) >
                         jce_phacelle_pow_inv(t, 2.0f));
        /* d < 1 => the mask decays faster => LESS detail. */
        TEST_ASSERT_TRUE(jce_phacelle_pow_inv(t, 0.5f) < t - 1.0e-4f);
        /* d == 1 is the identity. */
        TEST_ASSERT_FLOAT_WITHIN(1.0e-5f, t, jce_phacelle_pow_inv(t, 1.0f));
    }
    /* Endpoints are fixed for every exponent, and the range is [0,1]. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_pow_inv(0.0f, 3.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_phacelle_pow_inv(1.0f, 3.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_pow_inv(-9.0f, 3.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_phacelle_pow_inv(9.0f, 3.0f));
    /* Degenerate exponents must not produce Inf via 0^(-d). */
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_pow_inv(0.5f, 0.0f)));
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_pow_inv(0.5f, -4.0f)));
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_pow_inv(0.5f, NAN)));
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_pow_inv(NAN, 2.0f)));
}

/* -- 6. shaping helpers ------------------------------------------------ */

static void test_shaping_helpers_are_bounded_and_c1(void)
{
    const float s = 2.0f;
    const float d = 1.0e-3f;
    float lo, hi, mid, slope_lo, slope_hi;
    int i;
    float prev;

    /* ease_out: [0,1], monotone, and >= t on [0,1] (it eases OUT). */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_ease_out(0.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_phacelle_ease_out(1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_ease_out(-5.0f));
    TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_phacelle_ease_out(5.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_ease_out(NAN));
    prev = 0.0f;
    for (i = 0; i <= 100; ++i) {
        float t = 0.01f * (float)i;
        float e = jce_phacelle_ease_out(t);
        TEST_ASSERT_TRUE(e >= -EPS && e <= 1.0f + EPS);
        TEST_ASSERT_TRUE(e >= t - EPS);
        TEST_ASSERT_TRUE(e >= prev - EPS);
        prev = e;
    }

    /* smooth_start: the two branches must meet C1 at t == s, otherwise the
     * mask has a kink and the erosion shows a visible terrace. */
    lo  = jce_phacelle_smooth_start(s - d, s);
    mid = jce_phacelle_smooth_start(s,     s);
    hi  = jce_phacelle_smooth_start(s + d, s);
    TEST_ASSERT_FLOAT_WITHIN(1.0e-4f, 0.5f * s, mid);   /* C0 */
    slope_lo = (mid - lo) / d;
    slope_hi = (hi - mid) / d;
    TEST_ASSERT_FLOAT_WITHIN(1.0e-2f, slope_lo, slope_hi); /* C1 */
    TEST_ASSERT_FLOAT_WITHIN(1.0e-2f, 1.0f, slope_hi);

    /* Never exceeds the un-rounded ramp, never negative, degenerate-safe. */
    for (i = 0; i <= 100; ++i) {
        float t = 0.05f * (float)i;
        float v = jce_phacelle_smooth_start(t, s);
        TEST_ASSERT_TRUE(v >= 0.0f);
        TEST_ASSERT_TRUE(v <= t + EPS);
    }
    TEST_ASSERT_EQUAL_FLOAT(3.0f, jce_phacelle_smooth_start(3.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_phacelle_smooth_start(-3.0f, 2.0f));
    TEST_ASSERT_TRUE(isfinite(jce_phacelle_smooth_start(NAN, NAN)));
}

/* -- 7. finite over a wide sweep --------------------------------------- */

static void test_erode_is_finite_over_a_wide_parameter_sweep(void)
{
    static const float weird[] = {
        0.0f, -0.0f, 1.0e-9f, 1.0f, -37.5f, 1.0e7f, -1.0e9f, 1.0e20f
    };
    static const float bad[] = { NAN, INFINITY, -INFINITY };
    int oc, ni, gi, di, li, k;

    for (oc = 0; oc <= 14; oc += 7) {
        for (ni = 0; ni < 4; ++ni) {
            for (gi = 0; gi < 3; ++gi) {
                for (di = 0; di < 3; ++di) {
                    for (li = 0; li < 3; ++li) {
                        JcePhacelleParams p;
                        jce_phacelle_params_default(&p);
                        p.octaves       = oc;                     /* incl. > MAX */
                        p.normalization = -0.5f + 0.55f * (float)ni; /* incl. OOR */
                        p.gain          = 0.0f + 1.25f * (float)gi;
                        p.detail        = (di == 0) ? 0.0f : ((di == 1) ? 1.0f : 8.0f);
                        p.lacunarity    = (li == 0) ? 0.0f : ((li == 1) ? 2.0f : 9.0f);
                        p.cell_scale    = 0.5f + 0.75f * (float)gi;

                        for (k = 0; k < (int)(sizeof weird / sizeof weird[0]); ++k) {
                            float h, gx, gz, r;
                            jce_phacelle_erode(&p, weird[k], weird[(k + 3) % 8],
                                               weird[(k + 1) % 8],
                                               weird[(k + 2) % 8] * 0.001f,
                                               weird[(k + 5) % 8] * 0.001f,
                                               &h, &gx, &gz, &r);
                            TEST_ASSERT_TRUE(isfinite(h));
                            TEST_ASSERT_TRUE(isfinite(gx));
                            TEST_ASSERT_TRUE(isfinite(gz));
                            TEST_ASSERT_TRUE(isfinite(r));
                            TEST_ASSERT_TRUE(r >= -1.0f && r <= 1.0f);
                        }
                    }
                }
            }
        }
    }

    /* NaN / Inf in the sample position, height and gradient. */
    for (k = 0; k < 3; ++k) {
        JcePhacelleParams p;
        float h, gx, gz, r;
        jce_phacelle_params_default(&p);
        p.frequency = bad[k];
        p.strength  = bad[(k + 1) % 3];
        p.onset     = bad[(k + 2) % 3];
        jce_phacelle_erode(&p, bad[k], bad[(k + 1) % 3],
                           bad[(k + 2) % 3], bad[k], bad[(k + 1) % 3],
                           &h, &gx, &gz, &r);
        TEST_ASSERT_TRUE(isfinite(h) && isfinite(gx) && isfinite(gz));
        TEST_ASSERT_TRUE(r >= -1.0f && r <= 1.0f);
    }
}

/* -- 8. determinism / seam safety -------------------------------------- */

static void test_determinism_and_call_order_are_bit_identical(void)
{
    enum { N = 12 };
    JcePhacelleParams p;
    float row[N * N * 4];
    float col[N * N * 4];
    int i, j;

    jce_phacelle_params_default(&p);

    for (j = 0; j < N; ++j) {           /* row-major traversal */
        for (i = 0; i < N; ++i) {
            float wx = -2.0f + 0.37f * (float)i;
            float wz =  1.0f + 0.41f * (float)j;
            float *o = &row[(j * N + i) * 4];
            jce_phacelle_erode(&p, wx, wz, 0.5f * wx, 0.1f, -0.2f,
                               &o[0], &o[1], &o[2], &o[3]);
        }
    }
    for (i = N - 1; i >= 0; --i) {      /* column-major, reversed */
        for (j = N - 1; j >= 0; --j) {
            float wx = -2.0f + 0.37f * (float)i;
            float wz =  1.0f + 0.41f * (float)j;
            float *o = &col[(j * N + i) * 4];
            jce_phacelle_erode(&p, wx, wz, 0.5f * wx, 0.1f, -0.2f,
                               &o[0], &o[1], &o[2], &o[3]);
        }
    }
    /* A world point must not depend on which tile asked for it, or chunk
     * borders would not weld.  Bit-identical, not merely close. */
    TEST_ASSERT_EQUAL_INT(0, memcmp(row, col, sizeof row));

    /* Repeating the same call gives the same bits, and interleaving unrelated
     * work in between changes nothing (no hidden state). */
    {
        float a[4], b[4];
        jce_phacelle_erode(&p, 3.25f, -7.5f, 2.0f, 0.3f, 0.4f,
                           &a[0], &a[1], &a[2], &a[3]);
        (void)jce_phacelle_pow_inv(0.3f, 2.0f);
        jce_phacelle_erode(&p, -99.0f, 12.0f, 1.0f, 0.0f, 0.0f, NULL, NULL, NULL, NULL);
        jce_phacelle_erode(&p, 3.25f, -7.5f, 2.0f, 0.3f, 0.4f,
                           &b[0], &b[1], &b[2], &b[3]);
        TEST_ASSERT_EQUAL_INT(0, memcmp(a, b, sizeof a));
    }
}

/* -- 9. flat extremum -------------------------------------------------- */

static void test_flat_extremum_stays_finite_and_bounded(void)
{
    JcePhacelleParams p;
    int i;

    jce_phacelle_params_default(&p);
    /* strength 1, gain 0.5, 8 octaves => sum|strength_i| < 2, and the faded
     * height term is bounded by gully_weight, so |dh| < 2.  The gradient term
     * is bounded by sum(strength_i * freq_i) = 8 for lacunarity 2. */
    for (i = 0; i <= 40; ++i) {
        float mag = (i == 0) ? 0.0f : powf(10.0f, -9.0f + 0.2f * (float)i);
        float h, gx, gz, r;
        jce_phacelle_erode(&p, 12.5f, -4.25f, 3.0f, mag, -mag, &h, &gx, &gz, &r);
        TEST_ASSERT_TRUE(isfinite(h) && isfinite(gx) && isfinite(gz));
        TEST_ASSERT_TRUE(fabsf(h - 3.0f) < 2.05f);
        TEST_ASSERT_TRUE(fabsf(gx - mag) < 9.0f);
        TEST_ASSERT_TRUE(fabsf(gz + mag) < 9.0f);
        TEST_ASSERT_TRUE(r >= -1.0f && r <= 1.0f);
    }

    /* An exactly flat input still resolves to a direction (the assumed slope)
     * rather than swinging with numerical dust: two evaluations of the same
     * flat point agree exactly, and turning the assumed slope off is safe. */
    {
        float h0, h1;
        jce_phacelle_erode(&p, 5.0f, 5.0f, 0.0f, 0.0f, 0.0f, &h0, NULL, NULL, NULL);
        jce_phacelle_erode(&p, 5.0f, 5.0f, 0.0f, 0.0f, 0.0f, &h1, NULL, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(0, memcmp(&h0, &h1, sizeof h0));
        p.assumed_slope_blend = 0.0f;
        p.assumed_slope_magnitude = 0.0f;
        jce_phacelle_erode(&p, 5.0f, 5.0f, 0.0f, 0.0f, 0.0f, &h0, NULL, NULL, NULL);
        TEST_ASSERT_TRUE(isfinite(h0));
    }

    /* A MASKED-OUT OCTAVE MUST CONTRIBUTE NO GRADIENT.  onset == 0 collapses
     * the mask to zero from octave 1 onward, so every later octave fades to
     * (fade_target, 0, 0) -- the height keeps tracking the fade target, but the
     * gradient must be frozen at its octave-0 value, bit for bit.  That zero
     * gradient part is precisely what breaks the feedback loop at a flat
     * extremum, where the slope direction is undefined. */
    {
        JcePhacelleParams q;
        float g1x, g1z, g8x, g8z, hh1, hh8;
        jce_phacelle_params_default(&q);
        q.onset = 0.0f;
        q.octaves = 1;
        jce_phacelle_erode(&q, 3.5f, -2.5f, 1.0f, 0.4f, 0.2f, &hh1, &g1x, &g1z, NULL);
        q.octaves = JCE_PHACELLE_MAX_OCTAVES;
        jce_phacelle_erode(&q, 3.5f, -2.5f, 1.0f, 0.4f, 0.2f, &hh8, &g8x, &g8z, NULL);
        TEST_ASSERT_EQUAL_INT(0, memcmp(&g1x, &g8x, sizeof g1x));
        TEST_ASSERT_EQUAL_INT(0, memcmp(&g1z, &g8z, sizeof g1z));
        /* ...while the height legitimately keeps moving, so the test above is
         * not passing merely because nothing happened. */
        TEST_ASSERT_TRUE(fabsf(hh8 - hh1) > 1.0e-3f);
    }
}

/* -- 10. every knob reaches the output --------------------------------- */

/* Largest height difference the two parameter sets produce over a short walk.
 * Used to prove a knob is actually connected to something. */
static float max_height_difference(const JcePhacelleParams *a,
                                   const JcePhacelleParams *b)
{
    float worst = 0.0f;
    int k;
    for (k = 0; k < 8; ++k) {
        float wx = 2.0f + 0.7f * (float)k;
        float wz = 3.0f - 0.5f * (float)k;
        float ha, hb;
        jce_phacelle_erode(a, wx, wz, 0.0f, 0.31f, -0.22f, &ha, NULL, NULL, NULL);
        jce_phacelle_erode(b, wx, wz, 0.0f, 0.31f, -0.22f, &hb, NULL, NULL, NULL);
        if (fabsf(ha - hb) > worst) { worst = fabsf(ha - hb); }
    }
    return worst;
}

/* Every knob must reach the output.  A parameter that is stored, clamped and
 * then never read looks perfectly healthy in an editor and does nothing. */
#define ASSERT_PARAM_IS_WIRED(field, lo, hi)                     \
    do {                                                         \
        JcePhacelleParams pa, pb;                                \
        jce_phacelle_params_default(&pa);                        \
        jce_phacelle_params_default(&pb);                        \
        pa.field = (lo);                                         \
        pb.field = (hi);                                         \
        TEST_ASSERT_TRUE(max_height_difference(&pa, &pb) > 0.01f); \
    } while (0)

static void test_every_parameter_reaches_the_output(void)
{
    ASSERT_PARAM_IS_WIRED(detail,        0.05f, 32.0f);
    ASSERT_PARAM_IS_WIRED(onset,         0.5f,  40.0f);
    ASSERT_PARAM_IS_WIRED(rounding,      0.0f,   2.0f);
    ASSERT_PARAM_IS_WIRED(gully_weight,  0.25f,  4.0f);
    ASSERT_PARAM_IS_WIRED(phase_offset,  0.0f,   0.5f);
    ASSERT_PARAM_IS_WIRED(cell_scale,    0.25f,  2.0f);
    ASSERT_PARAM_IS_WIRED(onset_gain,    0.5f,   2.0f);
    ASSERT_PARAM_IS_WIRED(rounding_gain, 0.5f,   2.0f);
    ASSERT_PARAM_IS_WIRED(assumed_slope_blend,     0.0f,  1.0f);
    ASSERT_PARAM_IS_WIRED(assumed_slope_magnitude, 0.1f, 10.0f);
    ASSERT_PARAM_IS_WIRED(frequency,     0.5f,   2.0f);
    ASSERT_PARAM_IS_WIRED(lacunarity,    1.5f,   3.0f);
    ASSERT_PARAM_IS_WIRED(gain,          0.25f,  0.9f);
    ASSERT_PARAM_IS_WIRED(strength,      0.5f,   2.0f);
    ASSERT_PARAM_IS_WIRED(normalization, 0.0f,   0.9f);
}

/* -- 11. what the output gradient is, and is not, good for ------------- */

static void test_output_gradient_is_steering_grade_only(void)
{
    JcePhacelleParams p;
    int i, j, agree = 0, total = 0;

    jce_phacelle_params_default(&p);
    p.octaves = 1;   /* least feedback => the approximation is at its best */

    for (j = 0; j < 40; ++j) {
        for (i = 0; i < 40; ++i) {
            /* An off-axis base gradient: with an axis-aligned one the stripe
             * side vector degenerates and one gradient component is ~0. */
            const float bx = 0.31f, bz = -0.22f;
            const float e = 1.0e-4f;
            float wx = -9.0f + 0.37f * (float)i;
            float wz = -9.0f + 0.43f * (float)j;
            float g, hp, hm, cd;

            jce_phacelle_erode(&p, wx, wz, bx * wx + bz * wz, bx, bz,
                               NULL, &g, NULL, NULL);
            /* This central difference is exactly what a shading normal, a
             * physics slope or a collider must use instead of `g`. */
            jce_phacelle_erode(&p, wx + e, wz, bx * (wx + e) + bz * wz, bx, bz,
                               &hp, NULL, NULL, NULL);
            jce_phacelle_erode(&p, wx - e, wz, bx * (wx - e) + bz * wz, bx, bz,
                               &hm, NULL, NULL, NULL);
            cd = (hp - hm) / (2.0f * e);
            if ((g > 0.0f) == (cd > 0.0f)) { ++agree; }
            ++total;
        }
    }
    /* The reported gradient must at least point the same way as the surface
     * more often than a coin would -- that is all "adequate to steer the next
     * octave" means, and it is what pins the sign of k'.  Measured ~79% here
     * and ~61% at the 8-octave default; a sign-flipped k' scores ~21%.  The
     * bound is deliberately loose ABOVE, so sharpening the approximation
     * later cannot fail this test. */
    TEST_ASSERT_TRUE(agree * 100 > total * 60);
    /* And it is emphatically not accurate: see the header. Nothing a player or
     * a simulation observes may be built from it. */
}

/* -- 12. ridge output -------------------------------------------------- */

static void test_ridge_is_in_unit_range_and_the_filter_does_work(void)
{
    JcePhacelleParams p;
    float rmin = 2.0f, rmax = -2.0f;
    float max_dh = 0.0f;
    int i, j, saturated = 0, total = 0;

    jce_phacelle_params_default(&p);

    for (j = 0; j < 20; ++j) {
        for (i = 0; i < 20; ++i) {
            float wx = -10.0f + 1.03f * (float)i;
            float wz = -10.0f + 0.97f * (float)j;
            float base = 0.35f * wx - 0.2f * wz;   /* a plain tilted plane */
            float h, r;
            jce_phacelle_erode(&p, wx, wz, base, 0.35f, -0.2f, &h, NULL, NULL, &r);
            TEST_ASSERT_TRUE(isfinite(r));
            TEST_ASSERT_TRUE(r >= -1.0f && r <= 1.0f);
            if (r < rmin) { rmin = r; }
            if (r > rmax) { rmax = r; }
            if (fabsf(r) > 0.99f) { ++saturated; }
            ++total;
            if (fabsf(h - base) > max_dh) { max_dh = fabsf(h - base); }
        }
    }
    /* The ridge map must actually discriminate; a constant would be useless
     * as a splat/foliage driver. */
    TEST_ASSERT_TRUE(rmax - rmin > 0.1f);
    /* It is a weighted MEAN of values in [-1,1], so it must occupy the range
     * rather than being scaled up and clipped against the clamp -- a clipped
     * mask is a binary mask, which is exactly what this replaces. */
    TEST_ASSERT_TRUE(saturated * 4 < total);
    /* And the filter must actually carve something into the plane. */
    TEST_ASSERT_TRUE(max_dh > 1.0e-3f);

    /* Different seeds must give different fields (the seed is really used). */
    {
        float ha, hb;
        p.seed = 1u;
        jce_phacelle_erode(&p, 2.0f, 3.0f, 0.0f, 0.4f, 0.1f, &ha, NULL, NULL, NULL);
        p.seed = 2u;
        jce_phacelle_erode(&p, 2.0f, 3.0f, 0.0f, 0.4f, 0.1f, &hb, NULL, NULL, NULL);
        TEST_ASSERT_TRUE(fabsf(ha - hb) > 1.0e-4f);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_and_degenerate_input_is_survivable);
    RUN_TEST(test_bell_reaches_zero_at_the_window_boundary);
    RUN_TEST(test_kernel_is_continuous_across_a_cell_boundary);
    RUN_TEST(test_partial_normalisation_is_clamped_and_safe);
    RUN_TEST(test_pow_inv_direction_is_more_detail_for_larger_exponent);
    RUN_TEST(test_shaping_helpers_are_bounded_and_c1);
    RUN_TEST(test_erode_is_finite_over_a_wide_parameter_sweep);
    RUN_TEST(test_determinism_and_call_order_are_bit_identical);
    RUN_TEST(test_flat_extremum_stays_finite_and_bounded);
    RUN_TEST(test_ridge_is_in_unit_range_and_the_filter_does_work);
    RUN_TEST(test_every_parameter_reaches_the_output);
    RUN_TEST(test_output_gradient_is_steering_grade_only);
    return UNITY_END();
}
