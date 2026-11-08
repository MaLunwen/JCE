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

static JceWaterFftSpectrumDesc spectrum_desc(uint32_t model)
{
    JceWaterFftSpectrumDesc d = {0};
    d.struct_size = sizeof(d);
    d.version = JCE_WATER_FFT_SPECTRUM_VERSION;
    d.model = model;
    d.seed = 17;
    d.wind_speed = 10.0f;
    d.fetch = 100000.0f;
    d.swell = 0.2f;
    d.depth = 5.0f;
    d.directional_blend = 1.0f;
    return d;
}

static void test_versioned_spectrum_controls(void)
{
    JceWaterFftSpectrumDesc d = spectrum_desc(JCE_WATER_FFT_SPECTRUM_JONSWAP);
    JceWaterFft *a = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    JceWaterFft *b = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(jce_water_fft_use_jonswap(a, 10, 100000, 0.2f, 17));
    d.struct_size--;
    TEST_ASSERT_FALSE(jce_water_fft_configure_spectrum(b, &d));
    d.struct_size++;
    d.version++;
    TEST_ASSERT_FALSE(jce_water_fft_configure_spectrum(b, &d));
    d.version--;
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(b, &d));
    jce_water_fft_evolve(a, 2.0f);
    jce_water_fft_evolve(b, 2.0f);
    const double calibration = 32.0 * 32.0 * sqrt(3.14159265358979323846 / 256.0);
    for (int i = 0; i < 32 * 32; ++i) {
        const double expected = jce_water_fft_height_data(a)[i] * calibration;
        TEST_ASSERT_DOUBLE_WITHIN(1e-6 + fabs(expected) * 1e-6,
            expected, jce_water_fft_height_data(b)[i]);
    }
    /* Before first evolve, opting back into the old API must restore its
     * original coefficients, not retain the new calibration state. */
    JceWaterFft *legacy = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    TEST_ASSERT_NOT_NULL(legacy);
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(legacy, &d));
    TEST_ASSERT_TRUE(jce_water_fft_use_jonswap(legacy, 10, 100000, 0.2f, 17));
    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(a));
    TEST_ASSERT_TRUE(jce_water_fft_enable_foam(legacy));
    jce_water_fft_evolve(legacy, 2.0f);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(a),
        jce_water_fft_height_data(legacy), 32 * 32 * sizeof(float));
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_foam_data(a),
        jce_water_fft_foam_data(legacy), 32 * 32 * sizeof(float));
    jce_water_fft_destroy(legacy);
    TEST_ASSERT_FALSE(jce_water_fft_configure_spectrum(b, &d));
    jce_water_fft_destroy(a);
    jce_water_fft_destroy(b);

    d.model = JCE_WATER_FFT_SPECTRUM_PM;
    a = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    b = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(a, &d));
    d.fetch = 1.0f;
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(b, &d));
    jce_water_fft_evolve(a, 2.0f);
    jce_water_fft_evolve(b, 2.0f);
    TEST_ASSERT_EQUAL_MEMORY(jce_water_fft_height_data(a),
                             jce_water_fft_height_data(b), 32 * 32 * sizeof(float));
    jce_water_fft_destroy(b);
    b = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    TEST_ASSERT_NOT_NULL(b);
    d.directional_blend = 0.0f;
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(b, &d));
    jce_water_fft_evolve(b, 2.0f);
    TEST_ASSERT_TRUE(memcmp(jce_water_fft_height_data(a),
                            jce_water_fft_height_data(b), 32 * 32 * sizeof(float)) != 0);
    jce_water_fft_destroy(a);
    jce_water_fft_destroy(b);
}

static void test_tma_fft_uses_finite_depth_phase(void)
{
    JceWaterFftSpectrumDesc d = spectrum_desc(JCE_WATER_FFT_SPECTRUM_TMA);
    d.depth = 2.0f;
    JceWaterFft *f = jce_water_fft_create(32, 128, 10, 1, 0, 0.001f, 17);
    TEST_ASSERT_NOT_NULL(f);
    TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(f, &d));
    const double k = WFFT_TWO_PI / 128.0;
    const double expected = sqrt((double)f->spectrum.gravity * k * tanh(k * d.depth));
    /* Storage is centered: (16,16) is DC, (16,17) is the fundamental. */
    const int fundamental = 16 * 32 + 17;
    TEST_ASSERT_DOUBLE_WITHIN(1e-10, expected, f->omega[fundamental]);
    TEST_ASSERT_TRUE(f->omega[fundamental] < sqrt(WFFT_GRAVITY * k));
    for (int row = 0; row < 32; ++row) for (int col = 0; col < 32; ++col) {
        const double x = (double)(row - 16), z = (double)(col - 16);
        const double wave_number = k * sqrt(x*x + z*z);
        const double reference = sqrt((double)f->spectrum.gravity * wave_number *
            tanh(wave_number * d.depth));
        TEST_ASSERT_DOUBLE_WITHIN(1e-10, reference, f->omega[row * 32 + col]);
    }
    jce_water_fft_evolve(f, 2.0f);
    const float *height = jce_water_fft_height_data(f);
    for (int i = 0; i < 32 * 32; ++i) TEST_ASSERT_TRUE(isfinite(height[i]));
    jce_water_fft_destroy(f);
}

/* PM S(omega) is a one-sided height variance density. Independently
 * integrate its isotropic k-space density over this finite FFT grid. */
static double pm_grid_variance(int n, double length, double wind)
{
    const double pi = 3.14159265358979323846;
    const double dk = 2.0 * pi / length, g = 9.81;
    const double wp = (0.877 / 1.026) * g / wind;
    double sum = 0.0;
    for (int row = 0; row < n; ++row) for (int col = 0; col < n; ++col) {
        const double x = (double)(row - n/2), z = (double)(col - n/2);
        const double k = dk * sqrt(x*x + z*z);
        if (k == 0.0) continue;
        const double omega = sqrt(g * k);
        const double r = wp / omega;
        const double energy = 0.0081 * g*g / pow(omega, 5.0) * exp(-1.25*r*r*r*r);
        sum += energy * (0.5 * sqrt(g/k)) / (2.0*pi*k) * dk*dk;
    }
    return sum;
}

static void test_versioned_pm_preserves_physical_energy_across_resolution(void)
{
    const int sizes[] = {32, 64, 128};
    double variances[3], references[3];
    for (int r = 0; r < 3; ++r) {
        const int n = sizes[r];
        double measured = 0.0;
        JceWaterFftSpectrumDesc d = spectrum_desc(JCE_WATER_FFT_SPECTRUM_PM);
        d.directional_blend = 0.0f;
        d.swell = 0.0f;
        for (unsigned int seed = 1; seed <= 16; ++seed) {
            d.seed = seed;
            JceWaterFft *f = jce_water_fft_create(n, 128, 10, 1, 0, 0.001f, seed);
            TEST_ASSERT_NOT_NULL(f);
            TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(f, &d));
            for (int t = 0; t < 4; ++t) {
                double mean;
                jce_water_fft_evolve(f, 3.7f * (float)t);
                measured += field_variance(jce_water_fft_height_data(f), n*n, &mean);
            }
            jce_water_fft_destroy(f);
        }
        measured /= 64.0;
        const double expected = pm_grid_variance(n, 128.0, 10.0);
        printf("PM energy N=%d variance=%g expected=%g Hs=%g\n",
            n, measured, expected, 4.0 * sqrt(measured));
        variances[r] = measured;
        references[r] = expected;
    }
    for (int r = 0; r < 3; ++r)
        TEST_ASSERT_DOUBLE_WITHIN(references[r] * 0.25, references[r], variances[r]);
}

/* The horizontal map transforms a world-space cell area. Recover its
 * signed area from the two secants, without assuming a unit FFT grid. */
static void test_versioned_jacobian_measures_world_area(void)
{
    const int sizes[] = {32, 64};
    const float lengths[] = {128.0f, 32.0f};
    for (int r = 0; r < 2; ++r) {
        const int n = sizes[r];
        const double step = lengths[r] / n;
        JceWaterFftSpectrumDesc d = spectrum_desc(JCE_WATER_FFT_SPECTRUM_PM);
        JceWaterFft *f = jce_water_fft_create(n, lengths[r], 10, 1, 0, 0.001f, 17);
        TEST_ASSERT_NOT_NULL(f);
        TEST_ASSERT_TRUE(jce_water_fft_configure_spectrum(f, &d));
        TEST_ASSERT_TRUE(jce_water_fft_enable_foam(f));
        jce_water_fft_evolve(f, 2.0f);
        const float *dx = jce_water_fft_disp_x_data(f);
        const float *dz = jce_water_fft_disp_z_data(f);
        const float *jac = jce_water_fft_foam_data(f);
        double worst = 0.0;
        for (int z = 0; z < n; ++z) for (int x = 0; x < n; ++x) {
            const int left = z*n + (x+n-1)%n, right = z*n + (x+1)%n;
            const int down = ((z+n-1)%n)*n+x, up = ((z+1)%n)*n+x;
            const double ax = 2.0*step + dx[right]-dx[left];
            const double az = dz[right]-dz[left];
            const double bx = dx[up]-dx[down];
            const double bz = 2.0*step + dz[up]-dz[down];
            const double area_ratio = (ax*bz-az*bx)/(4.0*step*step);
            const double error = fabs(jac[z*n+x]-area_ratio);
            if (error > worst) worst = error;
        }
        printf("Jacobian world area N=%d spacing=%g max_error=%g\n", n, step, worst);
        jce_water_fft_destroy(f);
        TEST_ASSERT_DOUBLE_WITHIN(2e-6, 0.0, worst);
    }
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
    RUN_TEST(test_versioned_spectrum_controls);
    RUN_TEST(test_tma_fft_uses_finite_depth_phase);
    RUN_TEST(test_versioned_pm_preserves_physical_energy_across_resolution);
    RUN_TEST(test_versioned_jacobian_measures_world_area);
    return UNITY_END();
}
