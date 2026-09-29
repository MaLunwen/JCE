/*
 * test_jce_rayleigh.c
 *
 * The constants that make the sky blue.
 *
 * These are three numbers copied into a shader, which is exactly the kind of
 * thing that rots: nobody can run a shader in a unit test, so the values drift
 * or get "tuned" and the only feedback is that the sky looks a bit off. The
 * assertions below are the ones that would have caught the bug this replaces --
 * a sky coloured by transmittance instead of by scattering, which came out warm
 * and read as a time-of-day choice for as long as it shipped.
 */

#include "renderer/jce_rayleigh.h"

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. THE POINT: blue scatters most, red least ───────────────────────
 *
 * If this ordering ever inverts, the sky is warm. That is precisely the defect
 * that shipped, and it is invisible to every other check: the gradient still
 * works, the sun still moves it, the exposure is still sane. */

static void test_blue_scatters_most(void)
{
    float b[3];
    jce_rayleigh_beta_luma_normalised(b);

    TEST_ASSERT_TRUE_MESSAGE(b[2] > b[1], "blue must scatter more than green - the sky is not blue otherwise");
    TEST_ASSERT_TRUE_MESSAGE(b[1] > b[0], "green must scatter more than red");
    TEST_ASSERT_TRUE(b[0] > 0.0f);
}

/* ── 2. The ratio is lambda^-4, independently ──────────────────────────
 *
 * The stored coefficients and the physical law are two separate sources. If a
 * typo crept into the constants, this catches it without anyone having to
 * remember what 33.1e-6 was supposed to be: the blue:red ratio must equal
 * (680/440)^4 to within measurement slop of the standard-atmosphere figures. */

static void test_ratio_matches_inverse_fourth_power(void)
{
    const double predicted = pow(680.0 / 440.0, 4.0);   /* 5.705 */
    const double actual    = (double)JCE_RAYLEIGH_BETA_B / (double)JCE_RAYLEIGH_BETA_R;
    TEST_ASSERT_DOUBLE_WITHIN_MESSAGE(0.05, predicted, actual,
        "beta_R no longer follows lambda^-4 - a constant has been mistyped or 'tuned'");

    /* Green sits where its wavelength says it should, too. */
    const double pred_gr = pow(680.0 / 550.0, 4.0);
    const double act_gr  = (double)JCE_RAYLEIGH_BETA_G / (double)JCE_RAYLEIGH_BETA_R;
    TEST_ASSERT_DOUBLE_WITHIN(0.05, pred_gr, act_gr);
}

/* ── 3. Normalisation preserves LUMINANCE exactly ──────────────────────
 *
 * This is what makes applying the fix safe on existing content. Normalising by
 * max or by sum would have changed every sky's brightness as well as its hue,
 * and a fix that visibly darkens every scene gets reverted before anyone checks
 * whether the colour got better. */

static void test_normalisation_preserves_luminance(void)
{
    float b[3];
    jce_rayleigh_beta_luma_normalised(b);

    const float lum = JCE_LUMA_R * b[0] + JCE_LUMA_G * b[1] + JCE_LUMA_B * b[2];
    /* EQUAL_FLOAT, not WITHIN: "about 1" across the entire sky is a global
     * exposure shift, which is the thing this property exists to exclude. */
    TEST_ASSERT_EQUAL_FLOAT(1.0f, lum);

    /* And it genuinely tints -- a normalisation that returned (1,1,1) would
     * satisfy the luminance property and leave the sky grey, which is the bug. */
    TEST_ASSERT_TRUE_MESSAGE(b[2] - b[0] > 1.0f,
        "coefficients are nearly achromatic - the sky would stay grey");
}

/* ── 4. The shader's hard-coded triple still matches ───────────────────
 *
 * fs_sky.sc cannot include this header, so it carries a literal copy. Two
 * copies of a constant is exactly how one of them goes stale, so the literal
 * is pinned here: if someone edits the header, this fails and points at the
 * shader that also needs editing. */

static void test_shader_literal_is_in_sync(void)
{
    /* The values written in engine/shaders/standard/fs_sky.sc. */
    const float shader_beta[3] = { 0.435585f, 1.017867f, 2.484984f };

    float b[3];
    jce_rayleigh_beta_luma_normalised(b);
    for (int i = 0; i < 3; ++i)
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, shader_beta[i], b[i],
            "fs_sky.sc's beta_R literal has drifted from jce_rayleigh.h");
}

/* ── 5. Degenerate call does not write past, or crash ──────────────────  */

static void test_null_is_safe(void)
{
    jce_rayleigh_beta_luma_normalised(NULL);   /* must simply return */
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_blue_scatters_most);
    RUN_TEST(test_ratio_matches_inverse_fourth_power);
    RUN_TEST(test_normalisation_preserves_luminance);
    RUN_TEST(test_shader_literal_is_in_sync);
    RUN_TEST(test_null_is_safe);
    return UNITY_END();
}
