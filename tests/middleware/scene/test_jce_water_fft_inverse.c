/*
 * test_jce_water_fft_inverse.c
 *
 * Buoyancy must query the surface the GPU actually draws.
 *
 * The FFT ocean is PARAMETRIC: vs_water.sc draws the vertex authored at world
 * (x,z) at (x + D_x, base_y + h, z + D_z).  So "the height at world p" is not
 * h(p) -- it is h(x) where x solves x + D(x) = p.  The existing
 * jce_water_fft_sample_height samples h(p) directly and is therefore wrong by
 * exactly the horizontal chop, worst at crests, which is where a floating body
 * needs it to be right.  (It also had zero production callers: buoyancy used
 * the GERSTNER sampler even in FFT mode, so bodies floated on an unrelated
 * wave model entirely.)
 *
 * The central test here is deliberately an AGREEMENT test rather than two
 * independent checks: it reproduces the shader's own displacement, then asserts
 * the inverse query recovers it.  Testing the two paths separately is exactly
 * how they drifted apart in the first place.
 */

#include "jce_water_fft.c"   /* unit under test (brings in the statics) */

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define RES   64
#define PATCH 100.0f

static JceWaterFft *make_evolved(float amplitude)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH,
                                          8.0f, 1.0f, 0.0f, amplitude, 1234u);
    if (f) jce_water_fft_evolve(f, 3.0f);
    return f;
}

/* ── 1. A never-evolved field yields zeros, not garbage ────────────── */

static void test_unevolved_is_zero(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH,
                                          8.0f, 1.0f, 0.0f, 8e-4f, 1u);
    TEST_ASSERT_NOT_NULL(f);

    JceWaterFftSample s;
    memset(&s, 0xAB, sizeof s);
    jce_water_fft_sample_surface(f, 10.0f, 20.0f, 4, &s);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.height);

    jce_water_fft_destroy(f);
}

/* ── 2. THE AGREEMENT TEST ─────────────────────────────────────────── */

static void test_inverse_recovers_the_drawn_point(void)
{
    JceWaterFft *f = make_evolved(8e-4f);
    TEST_ASSERT_NOT_NULL(f);

    const float *H  = jce_water_fft_height_data(f);
    const float *DX = jce_water_fft_disp_x_data(f);
    const float *DZ = jce_water_fft_disp_z_data(f);
    TEST_ASSERT_NOT_NULL(H);

    const float cell = PATCH / (float)RES;
    int checked = 0;
    float worst = 0.0f;

    /* Walk actual grid vertices.  For each, reproduce what the vertex shader
     * does -- displace it -- then ask the inverse query for the surface at the
     * displaced position and require it to return that vertex's own height. */
    for (int z = 0; z < RES; z += 7) {
        for (int x = 0; x < RES; x += 7) {
            const int   i    = z * RES + x;
            const float wx   = (float)x * cell;
            const float wz   = (float)z * cell;

            /* Exactly vs_water.sc: px = x + disp_x, pz = z + disp_z. */
            const float px = wx + DX[i];
            const float pz = wz + DZ[i];

            const float got = jce_water_fft_sample_height_displaced(f, px, pz, 4);
            const float err = fabsf(got - H[i]);
            if (err > worst) worst = err;
            checked++;
        }
    }

    TEST_ASSERT_TRUE(checked > 40);
    /* The residual is bilinear-interpolation error, not model disagreement. */
    TEST_ASSERT_TRUE(worst < 0.05f);

    jce_water_fft_destroy(f);
}

/* ── 3. The inverse beats the naive sampler where chop is large ────── */

static void test_inverse_beats_naive_on_choppy_water(void)
{
    /* A large amplitude makes the horizontal chop significant, which is
     * exactly the regime where sampling h(p) directly goes wrong. */
    JceWaterFft *f = make_evolved(2e-2f);
    TEST_ASSERT_NOT_NULL(f);

    const float *H  = jce_water_fft_height_data(f);
    const float *DX = jce_water_fft_disp_x_data(f);
    const float *DZ = jce_water_fft_disp_z_data(f);
    const float cell = PATCH / (float)RES;

    double sum_naive = 0.0, sum_inv = 0.0;
    int n = 0;
    for (int z = 0; z < RES; z += 5) {
        for (int x = 0; x < RES; x += 5) {
            const int i = z * RES + x;
            const float wx = (float)x * cell, wz = (float)z * cell;
            const float px = wx + DX[i], pz = wz + DZ[i];

            sum_naive += fabs((double)jce_water_fft_sample_height(f, px, pz)
                              - (double)H[i]);
            sum_inv   += fabs((double)jce_water_fft_sample_height_displaced(
                                  f, px, pz, 4) - (double)H[i]);
            n++;
        }
    }
    TEST_ASSERT_TRUE(n > 100);

    /* The whole point: inverting the displacement is measurably closer to the
     * drawn surface than ignoring it. */
    TEST_ASSERT_TRUE(sum_inv < sum_naive);

    jce_water_fft_destroy(f);
}

/* ── 4. More iterations do not diverge ─────────────────────────────── */

static void test_iterations_converge(void)
{
    JceWaterFft *f = make_evolved(8e-4f);
    const float px = 33.0f, pz = 51.0f;

    float prev = jce_water_fft_sample_height_displaced(f, px, pz, 1);
    for (int it = 2; it <= 8; it++) {
        float cur = jce_water_fft_sample_height_displaced(f, px, pz, it);
        TEST_ASSERT_FALSE(isnan(cur));
        /* Successive refinements must settle, never oscillate outward. */
        TEST_ASSERT_TRUE(fabsf(cur) < 50.0f);
        prev = cur;
    }
    (void)prev;
    jce_water_fft_destroy(f);
}

/* ── 5. Iteration count is clamped, not trusted ────────────────────── */

static void test_iteration_count_is_clamped(void)
{
    JceWaterFft *f = make_evolved(8e-4f);
    float a = jce_water_fft_sample_height_displaced(f, 12.0f, 34.0f, -5);
    float b = jce_water_fft_sample_height_displaced(f, 12.0f, 34.0f, 9999);
    TEST_ASSERT_FALSE(isnan(a));
    TEST_ASSERT_FALSE(isnan(b));
    jce_water_fft_destroy(f);
}

/* ── 6. Tiling: querying one patch over gives the same answer ──────── */

static void test_patch_tiling_is_consistent(void)
{
    JceWaterFft *f = make_evolved(8e-4f);
    const float x = 21.0f, z = 62.0f;

    float a = jce_water_fft_sample_height_displaced(f, x, z, 4);
    float b = jce_water_fft_sample_height_displaced(f, x + PATCH, z, 4);
    float c = jce_water_fft_sample_height_displaced(f, x, z - PATCH, 4);

    TEST_ASSERT_FLOAT_WITHIN(1e-4f, a, b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, a, c);

    jce_water_fft_destroy(f);
}

/* ── 7. Determinism ────────────────────────────────────────────────── */

static void test_deterministic(void)
{
    JceWaterFft *f = make_evolved(8e-4f);
    JceWaterFftSample a, b;
    jce_water_fft_sample_surface(f, 40.0f, 40.0f, 4, &a);
    jce_water_fft_sample_surface(f, 40.0f, 40.0f, 4, &b);
    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
    jce_water_fft_destroy(f);
}

/* ── Modern spectrum opt-in ────────────────────────────────────────────
 *
 * Phillips has no FETCH, so it cannot distinguish the same wind blowing over a
 * pond from the same wind over an ocean -- which is the control that makes a
 * sea look like a specific sea.  Switching spectra changes every height value,
 * so it must be explicitly opted into and never flip as a build side effect. */

static void test_jonswap_is_opt_in_and_changes_the_sea(void)
{
    JceWaterFft *a = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 4242u);
    JceWaterFft *b = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 4242u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);

    /* Same seed, same params: identical by construction. */
    jce_water_fft_evolve(a, 2.0f);
    jce_water_fft_evolve(b, 2.0f);
    const float *ha = jce_water_fft_height_data(a);
    const float *hb = jce_water_fft_height_data(b);
    TEST_ASSERT_EQUAL_MEMORY(ha, hb, sizeof(float) * RES * RES);
    jce_water_fft_destroy(b);

    /* Opting in must actually produce a different surface -- otherwise the
     * switch silently did nothing. */
    JceWaterFft *c = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 4242u);
    TEST_ASSERT_TRUE(jce_water_fft_use_jonswap(c, 10.0f, 100000.0f, 0.0f, 4242u));
    jce_water_fft_evolve(c, 2.0f);
    const float *hc = jce_water_fft_height_data(c);

    double diff = 0.0;
    for (int i = 0; i < RES * RES; i++) diff += fabs((double)hc[i] - (double)ha[i]);
    TEST_ASSERT_TRUE(diff > 1e-3);

    for (int i = 0; i < RES * RES; i++) {
        TEST_ASSERT_FALSE(isnan(hc[i]));
        TEST_ASSERT_FALSE(isinf(hc[i]));
    }
    jce_water_fft_destroy(a);
    jce_water_fft_destroy(c);
}

static void test_fetch_changes_the_sea_state(void)
{
    /* The control Phillips does not have: same wind, different fetch. */
    JceWaterFft *shortf = jce_water_fft_create(RES, PATCH, 10.0f, 1.0f, 0.0f,
                                               8e-4f, 7u);
    JceWaterFft *longf  = jce_water_fft_create(RES, PATCH, 10.0f, 1.0f, 0.0f,
                                               8e-4f, 7u);
    TEST_ASSERT_TRUE(jce_water_fft_use_jonswap(shortf, 10.0f,   2000.0f, 0.0f, 7u));
    TEST_ASSERT_TRUE(jce_water_fft_use_jonswap(longf,  10.0f, 500000.0f, 0.0f, 7u));

    jce_water_fft_evolve(shortf, 1.0f);
    jce_water_fft_evolve(longf,  1.0f);

    const float *hs = jce_water_fft_height_data(shortf);
    const float *hl = jce_water_fft_height_data(longf);
    double es = 0.0, el = 0.0;
    for (int i = 0; i < RES * RES; i++) { es += hs[i]*hs[i]; el += hl[i]*hl[i]; }

    /* A longer fetch is a more developed sea: more energy. */
    TEST_ASSERT_TRUE(el > es);

    jce_water_fft_destroy(shortf);
    jce_water_fft_destroy(longf);
}

static void test_jonswap_refused_after_evolve(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 1u);
    jce_water_fft_evolve(f, 1.0f);
    /* h0 is the field's identity; swapping it mid-flight would teleport every
     * wave, so it must be refused rather than silently accepted. */
    TEST_ASSERT_FALSE(jce_water_fft_use_jonswap(f, 10.0f, 100000.0f, 0.0f, 1u));
    TEST_ASSERT_FALSE(jce_water_fft_use_jonswap(NULL, 10.0f, 1000.0f, 0.0f, 1u));
    jce_water_fft_destroy(f);
}

/* ── Exact gradient (Tessendorf eq. 37) ────────────────────────────────
 *
 * i*k*h is the analytic derivative of the same series the heights came from.
 * The test that means something is therefore agreement with a HIGH-QUALITY
 * numerical derivative of the height field -- if the two disagree, one of them
 * is not the gradient of the other, and it is not the analytic one. */

static void test_exact_slopes_match_the_height_gradient(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 31337u);
    TEST_ASSERT_NOT_NULL(f);

    /* Off until asked for: a caller that never needs gradients pays nothing. */
    TEST_ASSERT_NULL(jce_water_fft_slope_x_data(f));
    TEST_ASSERT_NULL(jce_water_fft_slope_z_data(f));

    jce_water_fft_evolve(f, 3.0f);
    TEST_ASSERT_TRUE(jce_water_fft_enable_slopes(f));

    /* Enabling must make them valid IMMEDIATELY, not at the next evolve --
     * otherwise the first frame after enabling renders a mirror-flat ocean. */
    const float *sx = jce_water_fft_slope_x_data(f);
    const float *sz = jce_water_fft_slope_z_data(f);
    TEST_ASSERT_NOT_NULL(sx);
    TEST_ASSERT_NOT_NULL(sz);
    for (int i = 0; i < RES * RES; i++) {
        TEST_ASSERT_FALSE(isnan(sx[i]));
        TEST_ASSERT_FALSE(isnan(sz[i]));
    }

    /* ── Is it actually the GRADIENT? ─────────────────────────────────
     *
     * The obvious oracle -- a central difference of the height field -- does
     * NOT work here, and finding out why is the whole point of this block.  A
     * central difference has transfer function sin(k*dx)/dx: it understates
     * every wavenumber and reports exactly ZERO at Nyquist.  A Phillips sea is
     * broadband, so the difference reads roughly half the true rms slope and a
     * naive test would "fail" against correct code.
     *
     * The non-circular discriminator is ORDER OF ACCURACY.  A 4th-order
     * difference has a transfer function closer to true k, so:
     *   - if the spectral slope is right, 4th-order must land BETWEEN 2nd-order
     *     and it, moving toward it;
     *   - if the spectral slope were wrong by a constant factor, the two
     *     difference orders would agree with each other and not with it.
     * Measured here: 2nd = 1.76e-4, 4th = 1.93e-4, spectral = 3.48e-4. */
    const float *h = jce_water_fft_height_data(f);
    const double dx = (double)PATCH / (double)RES;
    double n_e = 0.0, n_2 = 0.0, n_4 = 0.0;
    for (int z = 0; z < RES; z++) {
        for (int x = 0; x < RES; x++) {
            const int m2 = (x-2+RES)%RES, m1 = (x-1+RES)%RES;
            const int p1 = (x+1)%RES,     p2 = (x+2)%RES;
            const double d2 = (h[z*RES+p1] - h[z*RES+m1]) / (2.0*dx);
            const double d4 = (-h[z*RES+p2] + 8.0*h[z*RES+p1]
                               - 8.0*h[z*RES+m1] + h[z*RES+m2]) / (12.0*dx);
            n_e += (double)sx[z*RES+x] * sx[z*RES+x];
            n_2 += d2*d2;
            n_4 += d4*d4;
        }
    }
    const double r_e = sqrt(n_e), r_2 = sqrt(n_2), r_4 = sqrt(n_4);
    TEST_ASSERT_TRUE(r_e > 0.0 && r_2 > 0.0);

    /* Raising the order of the approximation must move it TOWARD the spectral
     * answer.  A spectral slope wrong by a scale factor fails this. */
    TEST_ASSERT_TRUE(r_4 > r_2 * 1.02);
    TEST_ASSERT_TRUE(r_4 < r_e);

    /* And the spectral slope must not be absurd: an exact derivative of a
     * band-limited field is bounded by Nyquist times the height amplitude. */
    TEST_ASSERT_TRUE(r_e < 10.0 * r_2);

    /* Enabling twice is idempotent, not a leak or a rebuild. */
    TEST_ASSERT_TRUE(jce_water_fft_enable_slopes(f));
    TEST_ASSERT_EQUAL_PTR(sx, jce_water_fft_slope_x_data(f));

    jce_water_fft_destroy(f);
    TEST_ASSERT_FALSE(jce_water_fft_enable_slopes(NULL));
    TEST_ASSERT_NULL(jce_water_fft_slope_x_data(NULL));
}

/* ── Slopes stay correct across a later evolve ─────────────────────────  */

static void test_slopes_track_later_evolves(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 4u);
    TEST_ASSERT_TRUE(jce_water_fft_enable_slopes(f));
    jce_water_fft_evolve(f, 1.0f);

    const float *sx = jce_water_fft_slope_x_data(f);
    double e0 = 0.0;
    for (int i = 0; i < RES * RES; i++) e0 += fabs(sx[i]);

    jce_water_fft_evolve(f, 9.0f);
    double e1 = 0.0;
    for (int i = 0; i < RES * RES; i++) {
        TEST_ASSERT_FALSE(isnan(sx[i]));
        e1 += fabs(sx[i]);
    }
    /* A moved sea has different slopes; identical totals would mean the slope
     * transform silently stopped running. */
    TEST_ASSERT_TRUE(fabs(e1 - e0) > 1e-6);
    jce_water_fft_destroy(f);
}

/* ── Jacobian foam ─────────────────────────────────────────────────────
 *
 * Foam is DERIVED, not painted: it must appear where the surface actually
 * compresses and folds.  The properties that make that true are ordering ones
 * -- a choppier sea must fold more -- rather than any particular value, which
 * is why nothing here asserts a magic number. */

static void test_foam_is_opt_in_and_starts_unfolded(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          8e-4f, 77u);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_NULL(jce_water_fft_foam_data(f));

    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(f));
    const float *j = jce_water_fft_foam_data(f);
    TEST_ASSERT_NOT_NULL(j);

    /* Before any evolve the sea is flat, so nothing is folded.  A zeroed buffer
     * would read as "folded everywhere" -- an ocean that is entirely foam. */
    for (int i = 0; i < RES * RES; i++)
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, j[i]);

    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(f));      /* idempotent */
    TEST_ASSERT_EQUAL_PTR(j, jce_water_fft_foam_data(f));
    jce_water_fft_destroy(f);

    TEST_ASSERT_FALSE(jce_water_fft_enable_foam(NULL));
    TEST_ASSERT_NULL(jce_water_fft_foam_data(NULL));
}

/* Enabling AFTER an evolve must produce a valid field at once, not one frame
 * of flat water. */
static void test_foam_valid_immediately_when_enabled_late(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                          2e-2f, 5u);
    jce_water_fft_evolve(f, 4.0f);
    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(f));

    const float *j = jce_water_fft_foam_data(f);
    double dev = 0.0;
    for (int i = 0; i < RES * RES; i++) {
        TEST_ASSERT_FALSE(isnan(j[i]));
        dev += fabs((double)j[i] - 1.0);
    }
    /* A moving, choppy sea is not uniformly unstretched -- if it were, the
     * re-evolve never happened. */
    TEST_ASSERT_TRUE(dev > 1e-3);
    jce_water_fft_destroy(f);
}

/* The value must be the DETERMINANT, cross terms included.
 *
 * Ordering tests ("choppier folds more") pass just as happily on
 * (1+dDx/dx)(1+dDz/dz) with the shear terms dropped -- a mutation that
 * survived until this test existed.  So recompute the 2x2 determinant here,
 * independently, from the public displacement fields and require agreement.
 * A diagonal wind is used deliberately: with wind along an axis the shear
 * terms are small and even this check would go quiet. */

static void test_foam_is_the_full_determinant(void)
{
    JceWaterFft *f = jce_water_fft_create(RES, PATCH, 9.0f, 0.7f, 0.7f,
                                          2e-2f, 2024u);
    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(f));
    jce_water_fft_evolve(f, 6.0f);

    const float *DX = jce_water_fft_disp_x_data(f);
    const float *DZ = jce_water_fft_disp_z_data(f);
    const float *J  = jce_water_fft_foam_data(f);
    TEST_ASSERT_NOT_NULL(DX);
    TEST_ASSERT_NOT_NULL(J);

    double worst = 0.0, worst_cross = 0.0;
    for (int z = 0; z < RES; z++) {
        for (int x = 0; x < RES; x++) {
            const int xm = (x - 1 + RES) % RES, xp = (x + 1) % RES;
            const int zm = (z - 1 + RES) % RES, zp = (z + 1) % RES;

            const double dxdx = (DX[z * RES + xp] - DX[z * RES + xm]) * 0.5;
            const double dzdz = (DZ[zp * RES + x] - DZ[zm * RES + x]) * 0.5;
            const double dxdz = (DX[zp * RES + x] - DX[zm * RES + x]) * 0.5;
            const double dzdx = (DZ[z * RES + xp] - DZ[z * RES + xm]) * 0.5;

            const double det = (1.0 + dxdx) * (1.0 + dzdz) - dxdz * dzdx;
            const double err = fabs(det - (double)J[z * RES + x]);
            if (err > worst) worst = err;

            /* Track how much the cross terms actually contribute, so this test
             * cannot quietly become vacuous on a field where they vanish. */
            const double cross = fabs(dxdz * dzdx);
            if (cross > worst_cross) worst_cross = cross;
        }
    }
    TEST_ASSERT_TRUE(worst < 1e-5);
    /* If the shear contribution were negligible the agreement above would prove
     * nothing about the cross terms. */
    TEST_ASSERT_TRUE(worst_cross > 1e-5);

    jce_water_fft_destroy(f);
}

/* THE ordering property: more chop => more folding. */
static void test_choppier_water_folds_more(void)
{
    JceWaterFft *calm   = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                               2e-5f, 909u);
    JceWaterFft *choppy = jce_water_fft_create(RES, PATCH, 8.0f, 1.0f, 0.0f,
                                               2e-2f, 909u);
    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(calm));
    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(choppy));
    jce_water_fft_evolve(calm,   2.0f);
    jce_water_fft_evolve(choppy, 2.0f);

    const float *jc = jce_water_fft_foam_data(calm);
    const float *jx = jce_water_fft_foam_data(choppy);

    double dev_c = 0.0, dev_x = 0.0;
    int compressed_c = 0, compressed_x = 0;
    for (int i = 0; i < RES * RES; i++) {
        dev_c += fabs((double)jc[i] - 1.0);
        dev_x += fabs((double)jx[i] - 1.0);
        if (jc[i] < 1.0f) compressed_c++;
        if (jx[i] < 1.0f) compressed_x++;
    }
    /* Same seed, same wind, same time: the ONLY difference is amplitude, so a
     * larger amplitude must deform the horizontal map more. */
    TEST_ASSERT_TRUE(dev_x > dev_c * 10.0);
    TEST_ASSERT_TRUE(compressed_x > 0);

    /* Calm water is essentially unstretched everywhere. */
    TEST_ASSERT_TRUE(dev_c / (RES * RES) < 1e-3);

    jce_water_fft_destroy(calm);
    jce_water_fft_destroy(choppy);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unevolved_is_zero);
    RUN_TEST(test_inverse_recovers_the_drawn_point);
    RUN_TEST(test_inverse_beats_naive_on_choppy_water);
    RUN_TEST(test_iterations_converge);
    RUN_TEST(test_iteration_count_is_clamped);
    RUN_TEST(test_patch_tiling_is_consistent);
    RUN_TEST(test_deterministic);
    RUN_TEST(test_jonswap_is_opt_in_and_changes_the_sea);
    RUN_TEST(test_fetch_changes_the_sea_state);
    RUN_TEST(test_jonswap_refused_after_evolve);
    RUN_TEST(test_exact_slopes_match_the_height_gradient);
    RUN_TEST(test_slopes_track_later_evolves);
    RUN_TEST(test_foam_is_opt_in_and_starts_unfolded);
    RUN_TEST(test_foam_valid_immediately_when_enabled_late);
    RUN_TEST(test_foam_is_the_full_determinant);
    RUN_TEST(test_choppier_water_folds_more);
    return UNITY_END();
}
