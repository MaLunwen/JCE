/*
 * test_jce_sky_sh9.c
 *
 * Sky-derived ambient: project the analytic sky onto 9 spherical-harmonic
 * coefficients per channel and reconstruct diffuse irradiance from them.
 *
 * This is what replaces the hardcoded ambient RGB literals in the scene
 * renderer.  The Lambertian cosine kernel is low-frequency enough that three
 * SH bands reproduce diffuse irradiance to ~1% average error (Ramamoorthi &
 * Hanrahan), so 9 vec3s of uniform is enough -- no sampler, no view id, no
 * cubemap, which is what makes it affordable on the WebGL2 / 512 MB floor.
 *
 * The reconstruction machinery is tested independently of the sky model with
 * hand-made coefficients, so a sky re-tune cannot mask an SH bug.
 */

#include <jce/middleware/world/jce_sky.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static void sun_dir(float elev_deg, float out[3])
{
    float r = elev_deg * 3.14159265358979f / 180.0f;
    out[0] = cosf(r); out[1] = sinf(r); out[2] = 0.0f;
}

static float luma(const float c[3])
{
    return 0.2126f * c[0] + 0.7152f * c[1] + 0.0722f * c[2];
}

/* ── 1. DC-only coefficients reconstruct a constant ────────────────── */

static void test_dc_only_is_directionally_flat(void)
{
    float sh[9][3];
    memset(sh, 0, sizeof sh);
    sh[0][0] = sh[0][1] = sh[0][2] = 1.0f;

    const float dirs[6][3] = {
        { 1,0,0 }, { -1,0,0 }, { 0,1,0 }, { 0,-1,0 }, { 0,0,1 }, { 0,0,-1 },
    };
    float first[3];
    jce_sky_irradiance_sh9(sh, dirs[0], first);

    for (int i = 1; i < 6; i++) {
        float c[3];
        jce_sky_irradiance_sh9(sh, dirs[i], c);
        /* A DC-only signal has no directional variation. */
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, first[0], c[0]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, first[1], c[1]);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, first[2], c[2]);
    }
    TEST_ASSERT_TRUE(first[0] > 0.0f);
}

/* ── 2. A +Y linear band brightens up and darkens down ─────────────── */

static void test_linear_band_is_directional(void)
{
    float sh[9][3];
    memset(sh, 0, sizeof sh);
    /* DC plus a positive Y (up) linear term. */
    sh[0][0] = sh[0][1] = sh[0][2] = 1.0f;
    sh[2][0] = sh[2][1] = sh[2][2] = 0.5f;   /* index 2 == the "up" band */

    const float up[3]   = { 0.0f,  1.0f, 0.0f };
    const float down[3] = { 0.0f, -1.0f, 0.0f };
    float cu[3], cd[3];
    jce_sky_irradiance_sh9(sh, up, cu);
    jce_sky_irradiance_sh9(sh, down, cd);

    TEST_ASSERT_TRUE(luma(cu) > luma(cd));
}

/* ── 3. Projecting the real sky yields a usable, finite signal ─────── */

static void test_projection_is_finite_and_positive(void)
{
    JceSkyConfig cfg = jce_sky_config_default();
    float sd[3];
    sun_dir(45.0f, sd);
    JceSkyState st = jce_sky_evaluate(&cfg, sd);

    float sh[9][3];
    jce_sky_project_sh9(&st, sh);

    for (int i = 0; i < 9; i++)
        for (int c = 0; c < 3; c++)
            TEST_ASSERT_FALSE(isnan(sh[i][c]) || isinf(sh[i][c]));

    /* The DC term is the average and must be positive for a lit sky. */
    TEST_ASSERT_TRUE(sh[0][0] > 0.0f);
    TEST_ASSERT_TRUE(sh[0][1] > 0.0f);
    TEST_ASSERT_TRUE(sh[0][2] > 0.0f);
}

/* ── 4. THE POINT: sky light comes from above ──────────────────────── */

static void test_up_is_brighter_than_down(void)
{
    JceSkyConfig cfg = jce_sky_config_default();
    float sd[3];
    sun_dir(60.0f, sd);
    JceSkyState st = jce_sky_evaluate(&cfg, sd);

    float sh[9][3];
    jce_sky_project_sh9(&st, sh);

    const float up[3]   = { 0.0f,  1.0f, 0.0f };
    const float down[3] = { 0.0f, -1.0f, 0.0f };
    float cu[3], cd[3];
    jce_sky_irradiance_sh9(sh, up, cu);
    jce_sky_irradiance_sh9(sh, down, cd);

    /* Only the upper hemisphere is projected, so a downward-facing normal
     * receives far less.  This is what stops SH ambient lighting the
     * underside of everything like open sky. */
    TEST_ASSERT_TRUE(luma(cu) > luma(cd) * 2.0f);
}

/* ── 5. Irradiance is never negative ───────────────────────────────── */

static void test_irradiance_non_negative(void)
{
    JceSkyConfig cfg = jce_sky_config_default();
    for (float elev = -5.0f; elev <= 85.0f; elev += 10.0f) {
        float sd[3];
        sun_dir(elev, sd);
        JceSkyState st = jce_sky_evaluate(&cfg, sd);
        float sh[9][3];
        jce_sky_project_sh9(&st, sh);

        for (int i = 0; i < 32; i++) {
            float a = (float)i * 0.19634954f;   /* 2pi/32 */
            float n[3] = { cosf(a), sinf(a * 0.5f) - 0.3f, sinf(a) };
            float len = sqrtf(n[0]*n[0] + n[1]*n[1] + n[2]*n[2]);
            n[0] /= len; n[1] /= len; n[2] /= len;

            float c[3];
            jce_sky_irradiance_sh9(sh, n, c);
            TEST_ASSERT_TRUE(c[0] >= 0.0f);
            TEST_ASSERT_TRUE(c[1] >= 0.0f);
            TEST_ASSERT_TRUE(c[2] >= 0.0f);
        }
    }
}

/* ── 6. A daytime sky is blue-dominant in ambient ──────────────────── */

static void test_daytime_ambient_is_blue_dominant(void)
{
    JceSkyConfig cfg = jce_sky_config_default();
    float sd[3];
    sun_dir(60.0f, sd);
    JceSkyState st = jce_sky_evaluate(&cfg, sd);

    float sh[9][3];
    jce_sky_project_sh9(&st, sh);

    const float up[3] = { 0.0f, 1.0f, 0.0f };
    float c[3];
    jce_sky_irradiance_sh9(sh, up, c);

    /* Rayleigh scattering makes the daytime sky blue, so sky-derived ambient
     * must be blue-dominant.  The old hardcoded literal (0.25,0.45,0.80) was
     * hand-picked to fake exactly this. */
    TEST_ASSERT_TRUE(c[2] > c[0]);
}

/* ── 7. Determinism ────────────────────────────────────────────────── */

static void test_deterministic(void)
{
    JceSkyConfig cfg = jce_sky_config_default();
    float sd[3];
    sun_dir(30.0f, sd);
    JceSkyState st = jce_sky_evaluate(&cfg, sd);

    float a[9][3], b[9][3];
    jce_sky_project_sh9(&st, a);
    jce_sky_project_sh9(&st, b);
    TEST_ASSERT_EQUAL_MEMORY(a, b, sizeof a);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_dc_only_is_directionally_flat);
    RUN_TEST(test_linear_band_is_directional);
    RUN_TEST(test_projection_is_finite_and_positive);
    RUN_TEST(test_up_is_brighter_than_down);
    RUN_TEST(test_irradiance_non_negative);
    RUN_TEST(test_daytime_ambient_is_blue_dominant);
    RUN_TEST(test_deterministic);
    return UNITY_END();
}
