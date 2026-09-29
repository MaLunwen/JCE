/* test_jce_sky.c
 *
 * Pure-CPU unit tests for the analytic Preetham daylight model.
 * No GPU: all assertions are on jce_sky_evaluate() / jce_sky_radiance().
 *
 * The Preetham model has a free absolute scale, so the tests prefer
 * relational / plausibility checks (brightest sample, channel ordering,
 * monotone trends) over exact magic numbers.
 */

#include <jce/middleware/world/jce_sky.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

/* --- helpers ------------------------------------------------------- */

static void norm3(float v[3])
{
    float l = sqrtf(v[0] * v[0] + v[1] * v[1] + v[2] * v[2]);
    if (l > 1e-6f) { v[0] /= l; v[1] /= l; v[2] /= l; }
}

static float luminance(const float rgb[3])
{
    /* Rec.709 relative luminance. */
    return 0.2126f * rgb[0] + 0.7152f * rgb[1] + 0.0722f * rgb[2];
}

static int all_finite(const float rgb[3])
{
    return isfinite(rgb[0]) && isfinite(rgb[1]) && isfinite(rgb[2]);
}

static int all_nonneg(const float rgb[3])
{
    return rgb[0] >= 0.0f && rgb[1] >= 0.0f && rgb[2] >= 0.0f;
}

/* --- tests --------------------------------------------------------- */

static void test_config_default_is_clear_day(void)
{
    JceSkyConfig c = jce_sky_config_default();
    TEST_ASSERT_TRUE(c.turbidity >= 1.0f && c.turbidity <= 10.0f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, c.exposure);
    TEST_ASSERT_EQUAL_INT(0, c.normalize);
}

/* 1. High sun: every direction finite + non-negative; the sun direction
 *    is (near) the brightest sample vs zenith and the opposite horizon;
 *    zenith is blue-ish (B > R) for a clear sky. */
static void test_high_sun_radiance_is_sane_and_blue_zenith(void)
{
    JceSkyConfig c = jce_sky_config_default();   /* turbidity 2.5 */
    float sun[3] = { 0.0f, 1.0f, 0.0f };         /* straight up (noon) */
    JceSkyState st = jce_sky_evaluate(&c, sun);

    /* Sample a spread of upper-hemisphere directions; all must be sane. */
    const float dirs[][3] = {
        { 0.0f, 1.0f, 0.0f },     /* zenith */
        { 1.0f, 0.2f, 0.0f },     /* horizon +x */
        { -1.0f, 0.2f, 0.0f },    /* horizon -x */
        { 0.0f, 0.2f, 1.0f },     /* horizon +z */
        { 0.0f, 0.2f, -1.0f },    /* horizon -z */
        { 0.5f, 0.5f, 0.5f },     /* mid */
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        float d[3] = { dirs[i][0], dirs[i][1], dirs[i][2] };
        norm3(d);
        float rgb[3];
        jce_sky_radiance(&st, d, rgb);
        TEST_ASSERT_TRUE(all_finite(rgb));
        TEST_ASSERT_TRUE(all_nonneg(rgb));
    }

    /* Zenith should be blue-ish on a clear day: B > R. */
    float up[3] = { 0.0f, 1.0f, 0.0f };
    float zrgb[3];
    jce_sky_radiance(&st, up, zrgb);
    TEST_ASSERT_TRUE(zrgb[2] > zrgb[0]);

    /* Sun direction should be brighter than the zenith and brighter than
     * the horizon opposite the sun.  Use a sun tilted off vertical so the
     * sun and zenith differ. */
    float sun2[3] = { 0.3f, 0.85f, 0.0f };
    norm3(sun2);
    JceSkyState st2 = jce_sky_evaluate(&c, sun2);

    float at_sun[3]; jce_sky_radiance(&st2, sun2, at_sun);
    float at_zen[3]; jce_sky_radiance(&st2, up, at_zen);

    float opp[3] = { -sun2[0], 0.2f, -sun2[2] };
    norm3(opp);
    float at_opp[3]; jce_sky_radiance(&st2, opp, at_opp);

    TEST_ASSERT_TRUE(luminance(at_sun) > luminance(at_zen));
    TEST_ASSERT_TRUE(luminance(at_sun) > luminance(at_opp));
}

/* 2. Low sun (sunset): the horizon near the sun is brighter / warmer than
 *    the zenith. */
static void test_low_sun_horizon_is_bright_and_warm(void)
{
    JceSkyConfig c = jce_sky_config_default();
    float sun[3] = { 0.0f, 0.08f, 1.0f };   /* near horizon */
    norm3(sun);
    JceSkyState st = jce_sky_evaluate(&c, sun);

    float near_sun[3] = { sun[0], 0.06f, sun[2] };
    norm3(near_sun);
    float horizon_rgb[3]; jce_sky_radiance(&st, near_sun, horizon_rgb);

    float up[3] = { 0.0f, 1.0f, 0.0f };
    float zen_rgb[3]; jce_sky_radiance(&st, up, zen_rgb);

    /* Brighter near the sun than at the zenith. */
    TEST_ASSERT_TRUE(luminance(horizon_rgb) > luminance(zen_rgb));

    /* Warmer near the sun: R/B ratio higher than at the zenith. */
    float horizon_warm = horizon_rgb[0] / (horizon_rgb[2] + 1e-6f);
    float zen_warm     = zen_rgb[0]     / (zen_rgb[2]     + 1e-6f);
    TEST_ASSERT_TRUE(horizon_warm > zen_warm);
}

/* 3. Turbidity monotonicity: higher turbidity → whiter (less saturated)
 *    zenith, i.e. the zenith B/R ratio decreases as turbidity rises. */
static void test_turbidity_whitens_zenith(void)
{
    float sun[3] = { 0.0f, 1.0f, 0.0f };
    float up[3]  = { 0.0f, 1.0f, 0.0f };

    float prev_ratio = 1e30f;
    const float turbidities[] = { 2.0f, 4.0f, 6.0f, 9.0f };
    for (size_t i = 0; i < sizeof(turbidities) / sizeof(turbidities[0]); ++i) {
        JceSkyConfig c = jce_sky_config_default();
        c.turbidity = turbidities[i];
        JceSkyState st = jce_sky_evaluate(&c, sun);
        float rgb[3]; jce_sky_radiance(&st, up, rgb);
        float ratio = rgb[2] / (rgb[0] + 1e-6f);   /* B/R: high = saturated blue */
        TEST_ASSERT_TRUE(all_finite(rgb));
        TEST_ASSERT_TRUE(all_nonneg(rgb));
        /* Trend: each higher turbidity is less blue-saturated than the last. */
        TEST_ASSERT_TRUE(ratio <= prev_ratio + 1e-3f);
        prev_ratio = ratio;
    }
}

/* 4. Determinism: same inputs → identical output (exact float). */
static void test_determinism(void)
{
    JceSkyConfig c = jce_sky_config_default();
    float sun[3] = { 0.2f, 0.7f, 0.3f };
    norm3(sun);

    JceSkyState a = jce_sky_evaluate(&c, sun);
    JceSkyState b = jce_sky_evaluate(&c, sun);
    TEST_ASSERT_EQUAL_INT(0, memcmp(&a, &b, sizeof(JceSkyState)));

    float view[3] = { 0.1f, 0.6f, -0.4f };
    norm3(view);
    float r1[3], r2[3];
    jce_sky_radiance(&a, view, r1);
    jce_sky_radiance(&b, view, r2);
    TEST_ASSERT_EQUAL_INT(0, memcmp(r1, r2, sizeof(r1)));
}

/* 5a. NULL cfg → defaults, no crash; produces a sane sky. */
static void test_null_cfg_uses_defaults(void)
{
    float sun[3] = { 0.0f, 1.0f, 0.0f };
    JceSkyState st = jce_sky_evaluate(NULL, sun);

    float up[3] = { 0.0f, 1.0f, 0.0f };
    float rgb[3];
    jce_sky_radiance(&st, up, rgb);
    TEST_ASSERT_TRUE(all_finite(rgb));
    TEST_ASSERT_TRUE(all_nonneg(rgb));

    /* Default config used → exposure 1, normalize off. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, st.exposure);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, st.normalize);
}

/* 5b. NULL out / NULL view dir → no crash. */
static void test_null_args_are_safe(void)
{
    JceSkyConfig c = jce_sky_config_default();
    float sun[3] = { 0.0f, 1.0f, 0.0f };
    JceSkyState st = jce_sky_evaluate(&c, sun);

    float view[3] = { 0.0f, 1.0f, 0.0f };
    jce_sky_radiance(&st, view, NULL);   /* NULL out: no-op, no crash */
    jce_sky_radiance(&st, NULL, view);   /* NULL view: writes zeros   */
    /* view here doubles as out for the second call; must be cleared. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, view[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, view[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, view[2]);

    float rgb[3] = { -1.0f, -1.0f, -1.0f };
    jce_sky_radiance(NULL, view, rgb);   /* NULL state: writes zeros  */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, rgb[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, rgb[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, rgb[2]);
}

/* 5c. Sun below the horizon → finite, non-negative (clamped). */
static void test_sun_below_horizon_is_clamped(void)
{
    JceSkyConfig c = jce_sky_config_default();
    float sun[3] = { 0.0f, -0.5f, 0.8f };   /* below horizon */
    norm3(sun);
    JceSkyState st = jce_sky_evaluate(&c, sun);

    /* Zenith and zenith-state values must stay finite. */
    TEST_ASSERT_TRUE(isfinite(st.Yz));
    TEST_ASSERT_TRUE(isfinite(st.xz));
    TEST_ASSERT_TRUE(isfinite(st.yz));

    const float dirs[][3] = {
        { 0.0f, 1.0f, 0.0f },
        { 1.0f, 0.05f, 0.0f },
        { 0.0f, 0.05f, 1.0f },
        { 0.0f, -0.5f, 0.8f },   /* same dir as the (sub-horizon) sun */
    };
    for (size_t i = 0; i < sizeof(dirs) / sizeof(dirs[0]); ++i) {
        float d[3] = { dirs[i][0], dirs[i][1], dirs[i][2] };
        norm3(d);
        float rgb[3];
        jce_sky_radiance(&st, d, rgb);
        TEST_ASSERT_TRUE(all_finite(rgb));
        TEST_ASSERT_TRUE(all_nonneg(rgb));
    }
}

/* Robustness sweep: full sphere of view dirs at several sun heights must
 * never produce a NaN/Inf or a negative channel. */
static void test_full_sweep_never_blows_up(void)
{
    const float sun_heights[] = { 1.0f, 0.5f, 0.1f, 0.0f, -0.3f };
    for (size_t s = 0; s < sizeof(sun_heights) / sizeof(sun_heights[0]); ++s) {
        float sun[3] = { 0.3f, sun_heights[s], 0.6f };
        norm3(sun);
        JceSkyConfig c = jce_sky_config_default();
        c.turbidity = 3.0f;
        JceSkyState st = jce_sky_evaluate(&c, sun);

        for (int it = 0; it <= 16; ++it) {
            for (int ip = 0; ip < 16; ++ip) {
                float theta = (float)it / 16.0f * 3.14159265f;   /* 0..pi */
                float phi   = (float)ip / 16.0f * 6.28318531f;   /* 0..2pi */
                float d[3] = {
                    sinf(theta) * cosf(phi),
                    cosf(theta),
                    sinf(theta) * sinf(phi),
                };
                float rgb[3];
                jce_sky_radiance(&st, d, rgb);
                TEST_ASSERT_TRUE(all_finite(rgb));
                TEST_ASSERT_TRUE(all_nonneg(rgb));
            }
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_config_default_is_clear_day);
    RUN_TEST(test_high_sun_radiance_is_sane_and_blue_zenith);
    RUN_TEST(test_low_sun_horizon_is_bright_and_warm);
    RUN_TEST(test_turbidity_whitens_zenith);
    RUN_TEST(test_determinism);
    RUN_TEST(test_null_cfg_uses_defaults);
    RUN_TEST(test_null_args_are_safe);
    RUN_TEST(test_sun_below_horizon_is_clamped);
    RUN_TEST(test_full_sweep_never_blows_up);
    return UNITY_END();
}
