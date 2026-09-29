/*
 * test_jce_ocean_spectrum.c
 *
 * Invariants of the JONSWAP / PM / TMA + Donelan-Banner spectra.  These assert
 * PROPERTIES, never golden numbers: a re-tune of the peak-enhancement or the
 * spreading exponents must keep every test green, while a broken guard, a lost
 * Jacobian or an unnormalised lobe must turn one red.
 *
 * The module is #included as a translation unit (it is pure math with no engine
 * dependency), matching test_jce_water_fft.c.
 */
#include "jce_ocean_spectrum.c" /* unit under test */

#include "unity.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define T_PI 3.14159265358979323846

/* NaN/Inf built at run time: writing 0.0f/0.0f as a constant expression lets the
 * compiler fold (or reject) it, and the point here is to feed the real thing. */
static float t_nan(void) { volatile float z = 0.0f; return z / z; }
static float t_inf(void) { volatile float z = 0.0f; return 1.0f / z; }

static int t_finite(float v) { return isfinite((double)v) != 0; }

static JceOceanSpectrumParams t_params(void)
{
    JceOceanSpectrumParams p;
    jce_ocean_spectrum_params_default(&p);
    return p;
}

/* Composite Simpson over [a,b] of the spreading, optionally windowed to
 * |theta| <= win (win < 0 disables the window).  Independent of the quadrature
 * the implementation uses for its own normalisation. */
static double t_integrate_spreading(const JceOceanSpectrumParams *p,
                                    float omega, double a, double b,
                                    double win, int n)
{
    double h = (b - a) / (double)n;
    double acc = 0.0;
    int i;
    for (i = 0; i <= n; ++i) {
        double th = a + (double)i * h;
        double v  = (double)jce_ocean_spectrum_spreading(p, omega, (float)th);
        double w  = (i == 0 || i == n) ? 1.0 : ((i & 1) ? 4.0 : 2.0);
        if (win >= 0.0 && fabs(th) > win) v = 0.0;
        acc += w * v;
    }
    return acc * h / 3.0;
}

/* Zeroth spectral moment m0 = integral S(w) dw; 4*sqrt(m0) is the significant
 * wave height, so m0 is "how much sea there is". */
static double t_m0(const JceOceanSpectrumParams *p)
{
    double acc = 0.0;
    int i;
    for (i = 1; i <= 10000; ++i) {
        double w = (double)i * 0.002;
        acc += (double)jce_ocean_spectrum_energy(p, (float)w) * 0.002;
    }
    return acc;
}

static float t_argmax_omega(const JceOceanSpectrumParams *p)
{
    float best_w = 0.0f, best_s = -1.0f;
    int i;
    for (i = 1; i <= 3000; ++i) {
        float w = (float)i * 0.002f;
        float s = jce_ocean_spectrum_energy(p, w);
        if (s > best_s) { best_s = s; best_w = w; }
    }
    return best_w;
}

/* --------------------------------------------------------------------- */

static void test_energy_finite_and_nonnegative_across_sweep(void)
{
    JceOceanSpectrumParams p = t_params();
    int i;

    /* Decades from far below any representable wave to far above capillary. */
    for (i = -12; i <= 6; ++i) {
        float w = (float)pow(10.0, (double)i);
        float s = jce_ocean_spectrum_energy(&p, w);
        TEST_ASSERT_TRUE(t_finite(s));
        TEST_ASSERT_TRUE(s >= 0.0f);
    }
    /* A dense sweep through the energetic band, where the w^-5 and the quartic
     * exponential fight each other. */
    for (i = 1; i <= 4000; ++i) {
        float s = jce_ocean_spectrum_energy(&p, (float)i * 0.005f);
        TEST_ASSERT_TRUE(t_finite(s));
        TEST_ASSERT_TRUE(s >= 0.0f);
    }
    /* No energy at or below zero frequency, and no NaN from pathological input. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_energy(&p, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_energy(&p, -3.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_energy(&p, t_nan()));
    TEST_ASSERT_TRUE(t_finite(jce_ocean_spectrum_energy(&p, t_inf())));
}

static void test_energy_peaks_at_expected_wp(void)
{
    JceOceanSpectrumParams p = t_params();
    double g = 9.81, u = 10.0, f = 100000.0;
    /* Independent restatement of wp = 22*(g^2/(U*F))^(1/3); if the impl drops
     * the cube root this is off by three orders of magnitude. */
    double wp_expected = 22.0 * pow((g * g) / (u * f), 1.0 / 3.0);
    float  wp_api, wp_arg;

    p.wind_speed = (float)u;
    p.fetch      = (float)f;
    p.gravity    = (float)g;
    wp_api = jce_ocean_spectrum_peak_omega(&p);
    TEST_ASSERT_FLOAT_WITHIN((float)(wp_expected * 0.01), (float)wp_expected, wp_api);

    /* d/dw of w^-5*exp(-1.25*(wp/w)^4) vanishes exactly at w = wp, and the
     * peak-enhancement term is centred there too, so the argmax IS wp. */
    wp_arg = t_argmax_omega(&p);
    TEST_ASSERT_FLOAT_WITHIN((float)(wp_expected * 0.05), (float)wp_expected, wp_arg);

    /* A realistic sea state, not just a self-consistent one: 10 m/s over 100 km
     * must give a significant wave height in the 1..4 m band. */
    {
        double hs = 4.0 * sqrt(t_m0(&p));
        TEST_ASSERT_TRUE(hs > 1.0 && hs < 4.0);
    }
}

static void test_fetch_develops_the_sea(void)
{
    JceOceanSpectrumParams shortf = t_params();
    JceOceanSpectrumParams longf  = t_params();
    shortf.fetch = 20000.0f;
    longf.fetch  = 500000.0f;

    /* More fetch => longer waves (lower peak frequency) and more total energy.
     * This is the control Phillips has no way to express. */
    TEST_ASSERT_TRUE(jce_ocean_spectrum_peak_omega(&longf) <
                     jce_ocean_spectrum_peak_omega(&shortf));
    TEST_ASSERT_TRUE(t_argmax_omega(&longf) < t_argmax_omega(&shortf));
    TEST_ASSERT_TRUE(t_m0(&longf) > 2.0 * t_m0(&shortf));

    /* Wind does the same, at fixed fetch. */
    {
        JceOceanSpectrumParams calm = t_params(), gale = t_params();
        calm.wind_speed = 5.0f;
        gale.wind_speed = 20.0f;
        TEST_ASSERT_TRUE(jce_ocean_spectrum_peak_omega(&gale) <
                         jce_ocean_spectrum_peak_omega(&calm));
        TEST_ASSERT_TRUE(t_m0(&gale) > t_m0(&calm));
    }
}

static void test_spectrum_family_ordering(void)
{
    JceOceanSpectrumParams js = t_params();
    JceOceanSpectrumParams pm = t_params();
    JceOceanSpectrumParams tma_shallow = t_params();
    JceOceanSpectrumParams tma_deep    = t_params();
    float wp = jce_ocean_spectrum_peak_omega(&js);
    int i;

    pm.type          = JCE_OCEAN_SPECTRUM_PM;
    tma_shallow.type = JCE_OCEAN_SPECTRUM_TMA;
    tma_deep.type    = JCE_OCEAN_SPECTRUM_TMA;
    tma_shallow.depth = 5.0f;
    tma_deep.depth    = 4000.0f;

    /* gamma = 3.3 only ADDS energy, and only near the peak: PM is the same
     * curve without the enhancement. */
    TEST_ASSERT_TRUE(jce_ocean_spectrum_energy(&pm, wp) <
                     jce_ocean_spectrum_energy(&js, wp));
    {
        float w_far = wp * 4.0f; /* r underflows to 0 here, so gamma^r == 1 */
        float a = jce_ocean_spectrum_energy(&pm, w_far);
        float b = jce_ocean_spectrum_energy(&js, w_far);
        TEST_ASSERT_FLOAT_WITHIN(b * 1e-4f + 1e-30f, b, a);
    }

    /* The enhancement is ASYMMETRIC by construction -- sigma is 0.07 below the
     * peak and 0.09 above it -- so at equal relative offset the high-frequency
     * flank keeps more of the gamma boost than the low-frequency flank.  This
     * skew is what distinguishes a JONSWAP peak from a symmetric bump. */
    {
        float d  = 0.15f;
        double r_lo = (double)jce_ocean_spectrum_energy(&js, wp * (1.0f - d))
                    / (double)jce_ocean_spectrum_energy(&pm, wp * (1.0f - d));
        double r_hi = (double)jce_ocean_spectrum_energy(&js, wp * (1.0f + d))
                    / (double)jce_ocean_spectrum_energy(&pm, wp * (1.0f + d));
        TEST_ASSERT_TRUE(r_lo > 1.0 && r_hi > r_lo * 1.01);
    }

    /* Kitaigorodskii is a factor in [0,1] that grows with depth, so
     * TMA(shallow) <= TMA(deep) <= JONSWAP at every frequency. */
    for (i = 1; i <= 400; ++i) {
        float w  = (float)i * 0.01f;
        float e_js = jce_ocean_spectrum_energy(&js, w);
        float e_sh = jce_ocean_spectrum_energy(&tma_shallow, w);
        float e_dp = jce_ocean_spectrum_energy(&tma_deep, w);
        TEST_ASSERT_TRUE(e_sh <= e_dp * 1.000001f + 1e-30f);
        TEST_ASSERT_TRUE(e_dp <= e_js * 1.000001f + 1e-30f);
    }
    /* Shallow water must actually cost energy, not merely fail to add it. */
    TEST_ASSERT_TRUE(t_m0(&tma_shallow) < t_m0(&js));
}

static void test_spreading_integrates_to_one(void)
{
    static const float swells[] = { 0.0f, 0.5f, 1.0f };
    static const float blends[] = { 0.0f, 0.35f, 1.0f };
    JceOceanSpectrumParams p = t_params();
    float wp = jce_ocean_spectrum_peak_omega(&p);
    int si, bi, oi;
    static const float mult[] = { 0.4f, 1.0f, 1.7f, 3.0f, 12.0f };

    for (si = 0; si < 3; ++si) {
        for (bi = 0; bi < 3; ++bi) {
            p.swell = swells[si];
            p.directional_blend = blends[bi];
            for (oi = 0; oi < 5; ++oi) {
                double total = t_integrate_spreading(&p, wp * mult[oi],
                                                     -T_PI, T_PI, -1.0, 2000);
                TEST_ASSERT_TRUE(fabs(total - 1.0) < 0.01);
            }
            /* Degenerate frequency still yields a proper density, so the
             * normalisation invariant has no holes. */
            TEST_ASSERT_TRUE(fabs(t_integrate_spreading(&p, 0.0f, -T_PI, T_PI,
                                                        -1.0, 400) - 1.0) < 0.01);
        }
    }
}

static void test_spreading_symmetric_and_periodic(void)
{
    JceOceanSpectrumParams p = t_params();
    float wp = jce_ocean_spectrum_peak_omega(&p);
    int i;
    p.swell = 0.7f;

    for (i = 0; i <= 40; ++i) {
        float th = (float)(-T_PI + (double)i * (2.0 * T_PI / 40.0));
        float a  = jce_ocean_spectrum_spreading(&p, wp, th);
        float b  = jce_ocean_spectrum_spreading(&p, wp, -th);
        float c  = jce_ocean_spectrum_spreading(&p, wp,
                                                (float)((double)th + 2.0 * T_PI));
        TEST_ASSERT_TRUE(t_finite(a) && a >= 0.0f);
        TEST_ASSERT_FLOAT_WITHIN(a * 1e-5f + 1e-9f, a, b);   /* even in theta  */
        TEST_ASSERT_FLOAT_WITHIN(a * 1e-4f + 1e-9f, a, c);   /* 2*PI periodic  */
    }
    /* Downwind must carry more energy than crosswind for the full model. */
    TEST_ASSERT_TRUE(jce_ocean_spectrum_spreading(&p, wp, 0.0f) >
                     jce_ocean_spectrum_spreading(&p, wp, (float)(0.5 * T_PI)));

    /* blend = 0 is exactly isotropic: every direction has density 1/(2*PI). */
    p.directional_blend = 0.0f;
    for (i = 0; i <= 8; ++i) {
        float th = (float)(-T_PI + (double)i * (2.0 * T_PI / 8.0));
        TEST_ASSERT_FLOAT_WITHIN(1e-6f, (float)(1.0 / (2.0 * T_PI)),
                                 jce_ocean_spectrum_spreading(&p, wp, th));
    }
}

static void test_swell_concentrates_the_lobe(void)
{
    JceOceanSpectrumParams none = t_params();
    JceOceanSpectrumParams full = t_params();
    float wp;
    double c_none, c_full;

    none.swell = 0.0f;
    full.swell = 1.0f;
    none.directional_blend = 1.0f;
    full.directional_blend = 1.0f;
    wp = jce_ocean_spectrum_peak_omega(&none);

    /* Fraction of the (unit-total) directional energy inside a narrow cone
     * about the wind: swell means long parallel wave trains, so it must rise. */
    c_none = t_integrate_spreading(&none, wp, -T_PI, T_PI, 0.25, 4000);
    c_full = t_integrate_spreading(&full, wp, -T_PI, T_PI, 0.25, 4000);
    TEST_ASSERT_TRUE(c_full > c_none + 0.02);

    /* The swell exponent scales with tanh(wp/w), so it must NOT narrow the
     * short-wave tail: high frequencies stay as broad as the pure model. */
    {
        float hi = wp * 8.0f;
        double h_none = t_integrate_spreading(&none, hi, -T_PI, T_PI, 0.25, 4000);
        double h_full = t_integrate_spreading(&full, hi, -T_PI, T_PI, 0.25, 4000);
        TEST_ASSERT_TRUE(h_full - h_none < c_full - c_none);
    }
}

static void test_amplitude_properties(void)
{
    JceOceanSpectrumParams p = t_params();
    float wp = jce_ocean_spectrum_peak_omega(&p);
    float k_peak = wp * wp / 9.81f;   /* deep-water dispersion inverted */
    float down, cross, tiny_wave;

    /* DC term carries no wave energy and must not divide by k = 0. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_amplitude(&p, 0.0f, 0.0f,
                                                               1.0f, 0.0f));
    down  = jce_ocean_spectrum_amplitude(&p, k_peak, 0.0f, 1.0f, 0.0f);
    cross = jce_ocean_spectrum_amplitude(&p, 0.0f, k_peak, 1.0f, 0.0f);
    TEST_ASSERT_TRUE(down > 0.0f);
    TEST_ASSERT_TRUE(down > cross);

    /* Rotating the wind rotates the lobe with it: the amplitude is a function
     * of the angle between k and the wind, not of either one alone. */
    TEST_ASSERT_FLOAT_WITHIN(down * 1e-4f, down,
        jce_ocean_spectrum_amplitude(&p, 0.0f, k_peak, 0.0f, 1.0f));
    /* Unnormalised wind vectors are accepted. */
    TEST_ASSERT_FLOAT_WITHIN(down * 1e-4f, down,
        jce_ocean_spectrum_amplitude(&p, k_peak, 0.0f, 17.0f, 0.0f));

    /* The wrapper must compose exactly the documented product, INCLUDING the
     * dw/dk = g/(2w) Jacobian that carries the frequency-space density into
     * wavenumber space -- dropping it is the classic error that leaves the
     * short waves far too strong.  Recomputed here from the other two entry
     * points, so a wrong dispersion or a missing factor shows up. */
    {
        double k = (double)k_peak * 1.7;
        double w = sqrt(9.81 * k);
        double expect = sqrt((4.0 * T_PI / k)
                             * (double)jce_ocean_spectrum_energy(&p, (float)w)
                             * (double)jce_ocean_spectrum_spreading(&p, (float)w, 0.0f)
                             * (9.81 / (2.0 * w)));
        TEST_ASSERT_FLOAT_WITHIN((float)expect * 1e-3f, (float)expect,
            jce_ocean_spectrum_amplitude(&p, (float)k, 0.0f, 1.0f, 0.0f));
    }

    /* Far above the peak the spectrum must fall away, not blow up on 1/k. */
    tiny_wave = jce_ocean_spectrum_amplitude(&p, 300.0f, 0.0f, 1.0f, 0.0f);
    TEST_ASSERT_TRUE(t_finite(tiny_wave));
    TEST_ASSERT_TRUE(tiny_wave < down * 1e-3f);

    /* Degenerate wind direction and non-finite wavevectors stay safe. */
    TEST_ASSERT_TRUE(t_finite(jce_ocean_spectrum_amplitude(&p, k_peak, k_peak,
                                                           0.0f, 0.0f)));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_amplitude(&p, t_nan(), 0.0f,
                                                               1.0f, 0.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_amplitude(&p, t_inf(), 0.0f,
                                                               1.0f, 0.0f));
}

static void test_extreme_parameters_stay_finite(void)
{
    JceOceanSpectrumParams p;
    int i, j;
    static const float wild[] = { 0.0f, -1.0f, 1e-9f, 1e9f, 1e30f };

    for (i = 0; i < 5; ++i) {
        for (j = 0; j < 7; ++j) {
            float bad = wild[i];
            jce_ocean_spectrum_params_default(&p);
            switch (j) {
                case 0: p.wind_speed = bad; break;
                case 1: p.fetch = bad; break;
                case 2: p.depth = bad; p.type = JCE_OCEAN_SPECTRUM_TMA; break;
                case 3: p.gravity = bad; break;
                case 4: p.swell = bad; break;
                case 5: p.directional_blend = bad; break;
                default: p.type = (JceOceanSpectrumType)(-7); break;
            }
            TEST_ASSERT_TRUE(jce_ocean_spectrum_peak_omega(&p) > 0.0f);
            {
                int s;
                for (s = -6; s <= 4; ++s) {
                    float w = (float)pow(10.0, (double)s);
                    float e = jce_ocean_spectrum_energy(&p, w);
                    float d = jce_ocean_spectrum_spreading(&p, w, 1.3f);
                    float a = jce_ocean_spectrum_amplitude(&p, w, 0.5f * w,
                                                           1.0f, 0.0f);
                    TEST_ASSERT_TRUE(t_finite(e) && e >= 0.0f);
                    TEST_ASSERT_TRUE(t_finite(d) && d >= 0.0f);
                    TEST_ASSERT_TRUE(t_finite(a) && a >= 0.0f);
                }
            }
        }
    }

    /* Serialized NaN/Inf in the params must not escape into the geometry. */
    jce_ocean_spectrum_params_default(&p);
    p.wind_speed = t_nan();
    p.fetch      = t_inf();
    p.gravity    = t_nan();
    p.swell      = t_inf();
    p.depth      = t_nan();
    p.directional_blend = t_nan();
    TEST_ASSERT_TRUE(t_finite(jce_ocean_spectrum_peak_omega(&p)));
    TEST_ASSERT_TRUE(t_finite(jce_ocean_spectrum_energy(&p, 1.0f)));
    TEST_ASSERT_TRUE(t_finite(jce_ocean_spectrum_spreading(&p, 1.0f, t_nan())));
    TEST_ASSERT_TRUE(t_finite(jce_ocean_spectrum_amplitude(&p, 0.1f, 0.1f,
                                                           1.0f, 0.0f)));
}

static void test_determinism_bit_identical(void)
{
    JceOceanSpectrumParams p = t_params();
    float a[192], b[192];
    int pass, i;

    p.swell = 0.63f;            /* exercises the quadrature-normalised branch */
    p.type  = JCE_OCEAN_SPECTRUM_TMA;

    for (pass = 0; pass < 2; ++pass) {
        float *dst = pass ? b : a;
        for (i = 0; i < 64; ++i) {
            float w  = 0.05f + (float)i * 0.07f;
            float th = -3.0f + (float)i * 0.09f;
            dst[i * 3 + 0] = jce_ocean_spectrum_energy(&p, w);
            dst[i * 3 + 1] = jce_ocean_spectrum_spreading(&p, w, th);
            dst[i * 3 + 2] = jce_ocean_spectrum_amplitude(&p, w, th, 0.6f, 0.8f);
        }
    }
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
}

static void test_null_and_default_params(void)
{
    JceOceanSpectrumParams p;

    /* Must not dereference; must not return garbage. */
    jce_ocean_spectrum_params_default(NULL);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_peak_omega(NULL));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_energy(NULL, 1.0f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_spreading(NULL, 1.0f, 0.2f));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_ocean_spectrum_amplitude(NULL, 1.0f, 1.0f,
                                                               1.0f, 0.0f));

    /* Defaults must themselves be a valid, energetic sea state. */
    memset(&p, 0, sizeof p);
    jce_ocean_spectrum_params_default(&p);
    TEST_ASSERT_TRUE(p.wind_speed > 0.0f && p.fetch > 0.0f && p.gravity > 0.0f);
    TEST_ASSERT_TRUE(p.swell >= 0.0f && p.swell <= 1.0f);
    TEST_ASSERT_TRUE(p.directional_blend >= 0.0f && p.directional_blend <= 1.0f);
    TEST_ASSERT_TRUE(jce_ocean_spectrum_energy(&p,
                        jce_ocean_spectrum_peak_omega(&p)) > 0.0f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_energy_finite_and_nonnegative_across_sweep);
    RUN_TEST(test_energy_peaks_at_expected_wp);
    RUN_TEST(test_fetch_develops_the_sea);
    RUN_TEST(test_spectrum_family_ordering);
    RUN_TEST(test_spreading_integrates_to_one);
    RUN_TEST(test_spreading_symmetric_and_periodic);
    RUN_TEST(test_swell_concentrates_the_lobe);
    RUN_TEST(test_amplitude_properties);
    RUN_TEST(test_extreme_parameters_stay_finite);
    RUN_TEST(test_determinism_bit_identical);
    RUN_TEST(test_null_and_default_params);
    return UNITY_END();
}