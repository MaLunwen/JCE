/* test_jce_water_fft.c
 *
 * Unit tests for the Tessendorf statistical FFT ocean core (jce_water_fft).
 * Pure / headless — no GPU, no flecs.  The unit under test (jce_water_fft.c) is
 * #included directly so the hand-rolled radix-2 FFT statics (fft_radix2, ifft2d)
 * can be exercised in isolation — the FFT round-trip is THE load-bearing
 * correctness check of the whole module.  The tracked allocator the core uses
 * (JCE_MALLOC) resolves by linking jce_core.
 *
 * Coverage:
 *   1. radix-2 FFT round-trip: ifft(fft(x))/N == x within epsilon (sizes 8/16/64)
 *   2. Phillips/seed determinism: same seed -> identical fields; diff seed differs
 *   3. evolved height is REAL + finite (no spurious imaginary leakage)
 *   4. field statistics: mean ~0; variance grows with amplitude; evolve determinism
 *   5. sample_height bilinear: grid points exact; midpoint = interpolation; wrap
 *   6. robustness: non-power-of-2 -> NULL; NULL-safe destroy/sample/accessors
 */

#include "jce_water_fft.c"   /* unit under test (brings in the statics) */

#include "unity.h"

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. radix-2 FFT round-trip is the identity ─────────────────────────── */
static void roundtrip_for_size(int n)
{
    WfftCpx *x  = (WfftCpx *)JCE_MALLOC((size_t)n * sizeof(WfftCpx));
    WfftCpx *x0 = (WfftCpx *)JCE_MALLOC((size_t)n * sizeof(WfftCpx));
    TEST_ASSERT_NOT_NULL(x);
    TEST_ASSERT_NOT_NULL(x0);

    /* A deterministic, non-trivial complex signal. */
    for (int i = 0; i < n; ++i) {
        x[i].re = sin(0.3 * i) + 0.5 * cos(1.1 * i + 0.2);
        x[i].im = 0.25 * sin(0.7 * i + 1.0) - 0.4 * cos(0.2 * i);
        x0[i] = x[i];
    }

    /* forward (sign=-1) then inverse (sign=+1); inverse divides by n. */
    fft_radix2(x, n, -1);
    fft_radix2(x, n, +1);
    double inv = 1.0 / (double)n;
    for (int i = 0; i < n; ++i) {
        x[i].re *= inv;
        x[i].im *= inv;
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, x0[i].re, x[i].re);
        TEST_ASSERT_DOUBLE_WITHIN(1e-9, x0[i].im, x[i].im);
    }

    JCE_FREE(x);
    JCE_FREE(x0);
}

static void test_fft_roundtrip_identity(void)
{
    roundtrip_for_size(8);
    roundtrip_for_size(16);
    roundtrip_for_size(64);
}

/* A delta in the frequency domain inverse-transforms to a constant — a second,
 * independent check of the inverse transform + normalization. */
static void test_fft_dc_delta(void)
{
    const int n = 16;
    WfftCpx *x = (WfftCpx *)JCE_MALLOC((size_t)n * sizeof(WfftCpx));
    TEST_ASSERT_NOT_NULL(x);
    for (int i = 0; i < n; ++i) { x[i].re = 0.0; x[i].im = 0.0; }
    x[0].re = (double)n;          /* DC only */

    fft_radix2(x, n, +1);         /* inverse */
    for (int i = 0; i < n; ++i) {
        x[i].re /= (double)n;
        x[i].im /= (double)n;
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 1.0, x[i].re);  /* constant 1 */
        TEST_ASSERT_DOUBLE_WITHIN(1e-12, 0.0, x[i].im);
    }
    JCE_FREE(x);
}

/* ── 2. spectrum + evolve determinism ──────────────────────────────────── */
static void test_seed_determinism(void)
{
    JceWaterFft *a = jce_water_fft_create(64, 100.0f, 8.0f, 1.0f, 0.3f, 0.0008f, 1234u);
    JceWaterFft *b = jce_water_fft_create(64, 100.0f, 8.0f, 1.0f, 0.3f, 0.0008f, 1234u);
    JceWaterFft *c = jce_water_fft_create(64, 100.0f, 8.0f, 1.0f, 0.3f, 0.0008f, 9999u);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_NOT_NULL(c);

    int cells = 64 * 64;
    /* same seed -> identical initial spectrum, bit for bit */
    TEST_ASSERT_EQUAL_INT(0, memcmp(a->h0, b->h0, (size_t)cells * sizeof(WfftCpx)));
    TEST_ASSERT_EQUAL_INT(0, memcmp(a->h0_conj, b->h0_conj,
                                    (size_t)cells * sizeof(WfftCpx)));

    /* same seed -> identical evolved height field */
    jce_water_fft_evolve(a, 2.5f);
    jce_water_fft_evolve(b, 2.5f);
    TEST_ASSERT_EQUAL_INT(0, memcmp(jce_water_fft_height_data(a),
                                    jce_water_fft_height_data(b),
                                    (size_t)cells * sizeof(float)));

    /* different seed -> different spectrum (at least one cell differs) */
    TEST_ASSERT_NOT_EQUAL(0, memcmp(a->h0, c->h0, (size_t)cells * sizeof(WfftCpx)));

    /* evolve is a pure function of (state,t): re-evolve at the same t == same */
    JceWaterFft *d = jce_water_fft_create(64, 100.0f, 8.0f, 1.0f, 0.3f, 0.0008f, 1234u);
    jce_water_fft_evolve(d, 2.5f);
    TEST_ASSERT_EQUAL_INT(0, memcmp(jce_water_fft_height_data(a),
                                    jce_water_fft_height_data(d),
                                    (size_t)cells * sizeof(float)));

    jce_water_fft_destroy(a);
    jce_water_fft_destroy(b);
    jce_water_fft_destroy(c);
    jce_water_fft_destroy(d);
}

/* ── 3. evolved height is real (imag ~0) + finite ──────────────────────── */
static void test_evolved_height_real_finite(void)
{
    const int N = 64;
    JceWaterFft *f = jce_water_fft_create(N, 100.0f, 9.0f, 1.0f, 0.2f, 0.0006f, 77u);
    TEST_ASSERT_NOT_NULL(f);
    jce_water_fft_evolve(f, 1.3f);

    /* The IFFT scratch (f->hkt) still holds the complex spatial field; with a
     * Hermitian-symmetric h(k,t) the imaginary part must be ~0 everywhere. */
    int cells = N * N;
    double max_imag = 0.0;
    for (int i = 0; i < cells; ++i) {
        double ai = fabs(f->hkt[i].im);
        if (ai > max_imag) max_imag = ai;
    }
    TEST_ASSERT_DOUBLE_WITHIN(1e-6, 0.0, max_imag);

    /* Output height field is finite everywhere. */
    const float *h = jce_water_fft_height_data(f);
    for (int i = 0; i < cells; ++i)
        TEST_ASSERT_TRUE(isfinite(h[i]));

    jce_water_fft_destroy(f);
}

/* ── 4. field statistics: mean ~0, variance grows with amplitude ───────── */
static double field_variance(const float *h, int cells, double *out_mean)
{
    double mean = 0.0;
    for (int i = 0; i < cells; ++i) mean += h[i];
    mean /= (double)cells;
    double var = 0.0;
    for (int i = 0; i < cells; ++i) {
        double d = h[i] - mean;
        var += d * d;
    }
    var /= (double)cells;
    if (out_mean) *out_mean = mean;
    return var;
}

static void test_field_statistics(void)
{
    const int N = 64;
    const int cells = N * N;

    JceWaterFft *lo = jce_water_fft_create(N, 100.0f, 8.0f, 1.0f, 0.3f, 0.001f, 5u);
    JceWaterFft *hi = jce_water_fft_create(N, 100.0f, 8.0f, 1.0f, 0.3f, 0.004f, 5u);
    TEST_ASSERT_NOT_NULL(lo);
    TEST_ASSERT_NOT_NULL(hi);
    jce_water_fft_evolve(lo, 3.0f);
    jce_water_fft_evolve(hi, 3.0f);

    double mean_lo = 0.0, mean_hi = 0.0;
    double var_lo = field_variance(jce_water_fft_height_data(lo), cells, &mean_lo);
    double var_hi = field_variance(jce_water_fft_height_data(hi), cells, &mean_hi);

    /* Surface oscillates about the still plane: mean ~0. */
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.0, mean_lo);
    TEST_ASSERT_DOUBLE_WITHIN(1e-3, 0.0, mean_hi);

    /* Nonzero energy, and 4x amplitude => larger variance. */
    TEST_ASSERT_TRUE(var_lo > 0.0);
    TEST_ASSERT_TRUE(var_hi > var_lo);

    jce_water_fft_destroy(lo);
    jce_water_fft_destroy(hi);
}

/* ── 5. sample_height bilinear ──────────────────────────────────────────── */
static void test_sample_bilinear(void)
{
    const int N = 64;
    const float L = 100.0f;
    JceWaterFft *f = jce_water_fft_create(N, L, 8.0f, 1.0f, 0.5f, 0.002f, 42u);
    TEST_ASSERT_NOT_NULL(f);
    jce_water_fft_evolve(f, 0.9f);

    const float *h = jce_water_fft_height_data(f);
    const float cell = L / (float)N;

    /* Exact grid points return the grid values. */
    for (int z = 0; z < N; z += 7) {
        for (int x = 0; x < N; x += 9) {
            float wx = (float)x * cell;
            float wz = (float)z * cell;
            float s = jce_water_fft_sample_height(f, wx, wz);
            TEST_ASSERT_FLOAT_WITHIN(1e-3f, h[z * N + x], s);
        }
    }

    /* Midpoint of a cell == bilinear interpolation of the 4 corners. */
    {
        int x0 = 10, z0 = 20;
        float wx = ((float)x0 + 0.5f) * cell;
        float wz = ((float)z0 + 0.5f) * cell;
        float h00 = h[z0 * N + x0];
        float h10 = h[z0 * N + (x0 + 1)];
        float h01 = h[(z0 + 1) * N + x0];
        float h11 = h[(z0 + 1) * N + (x0 + 1)];
        float expect = 0.25f * (h00 + h10 + h01 + h11);
        float s = jce_water_fft_sample_height(f, wx, wz);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, expect, s);
    }

    /* Wrapping: world_x = L + d samples the same as d. */
    {
        float d = 13.7f;
        float a = jce_water_fft_sample_height(f, d, 4.2f);
        float b = jce_water_fft_sample_height(f, L + d, 4.2f);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, a, b);
        /* Negative wraps too. */
        float c = jce_water_fft_sample_height(f, d - L, 4.2f);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, a, c);
    }

    jce_water_fft_destroy(f);
}

/* ── 6. robustness ──────────────────────────────────────────────────────── */
static void test_robustness(void)
{
    /* Non-power-of-two N -> NULL. */
    TEST_ASSERT_NULL(jce_water_fft_create(48, 100.0f, 8.0f, 1.0f, 0.0f, 0.001f, 1u));
    TEST_ASSERT_NULL(jce_water_fft_create(100, 100.0f, 8.0f, 1.0f, 0.0f, 0.001f, 1u));
    TEST_ASSERT_NULL(jce_water_fft_create(0, 100.0f, 8.0f, 1.0f, 0.0f, 0.001f, 1u));
    /* Bad params -> NULL. */
    TEST_ASSERT_NULL(jce_water_fft_create(64, -1.0f, 8.0f, 1.0f, 0.0f, 0.001f, 1u));
    TEST_ASSERT_NULL(jce_water_fft_create(64, 100.0f, 0.0f, 1.0f, 0.0f, 0.001f, 1u));
    TEST_ASSERT_NULL(jce_water_fft_create(64, 100.0f, 8.0f, 1.0f, 0.0f, 0.0f, 1u));

    /* NULL-safe accessors / destroy / sample. */
    TEST_ASSERT_EQUAL_INT(0, jce_water_fft_resolution(NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_fft_patch_size(NULL));
    TEST_ASSERT_NULL(jce_water_fft_height_data(NULL));
    TEST_ASSERT_NULL(jce_water_fft_disp_x_data(NULL));
    TEST_ASSERT_NULL(jce_water_fft_disp_z_data(NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_fft_sample_height(NULL, 1.0f, 2.0f));
    jce_water_fft_evolve(NULL, 1.0f);   /* no crash */
    jce_water_fft_destroy(NULL);        /* no crash */

    /* Sampling before the first evolve returns 0 (well-defined). */
    JceWaterFft *f = jce_water_fft_create(32, 50.0f, 6.0f, 0.0f, 0.0f, 0.001f, 3u);
    TEST_ASSERT_NOT_NULL(f);            /* zero wind dir -> +X fallback, valid */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_fft_sample_height(f, 1.0f, 1.0f));
    TEST_ASSERT_EQUAL_INT(32, jce_water_fft_resolution(f));
    TEST_ASSERT_EQUAL_FLOAT(50.0f, jce_water_fft_patch_size(f));
    jce_water_fft_destroy(f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_fft_roundtrip_identity);
    RUN_TEST(test_fft_dc_delta);
    RUN_TEST(test_seed_determinism);
    RUN_TEST(test_evolved_height_real_finite);
    RUN_TEST(test_field_statistics);
    RUN_TEST(test_sample_bilinear);
    RUN_TEST(test_robustness);
    return UNITY_END();
}
