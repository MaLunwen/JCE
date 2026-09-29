/*
 * test_jce_water_optics.c
 *
 * Beer-Lambert absorption through water.
 *
 * The failure this exists to prevent is not a crash: it is water that dims
 * uniformly instead of losing red first. That renders as grey haze in a pool,
 * which reads as an atmospheric-fog setting somebody chose, and no amount of
 * tuning the surface colour corrects it because the cause is one number where
 * there should be three.
 */

#include "jce_water_optics.h"

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. THE POINT: extinction is per channel, in the right order ────────
 *
 * Red must go first. If the ordering inverted, deep water would turn red --
 * which is not a stylistic variant of water, it is a different substance. */

static void test_red_is_absorbed_fastest_and_blue_slowest(void)
{
    const JceWaterExtinction e = jce_water_extinction_from_clarity(10.0f);
    TEST_ASSERT_TRUE(e.sigma[0] > e.sigma[1]);
    TEST_ASSERT_TRUE(e.sigma[1] > e.sigma[2]);

    float t[3];
    jce_water_transmittance(&e, 5.0f, t);
    /* Ordering of what SURVIVES is the inverse. */
    TEST_ASSERT_TRUE(t[0] < t[1]);
    TEST_ASSERT_TRUE(t[1] < t[2]);

    /* And the gap must be large enough to see. A per-channel split that is
     * technically present but within a few percent is a scalar with extra
     * steps -- it would pass an ordering check and still render grey. */
    TEST_ASSERT_TRUE(t[2] > t[0] * 4.0f);
}

/* ── 2. Clarity means what it says ─────────────────────────────────────  */

static void test_clarity_is_the_green_1_over_e_distance(void)
{
    const float clarity = 12.0f;
    const JceWaterExtinction e = jce_water_extinction_from_clarity(clarity);
    float t[3];
    jce_water_transmittance(&e, clarity, t);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, expf(-1.0f), t[1]);

    /* Murkier water attenuates more at the same depth, in every channel. */
    const JceWaterExtinction murky = jce_water_extinction_from_clarity(3.0f);
    float tm[3];
    jce_water_transmittance(&murky, 5.0f, tm);
    float tc[3];
    jce_water_transmittance(&e, 5.0f, tc);
    for (int i = 0; i < 3; ++i) TEST_ASSERT_TRUE(tm[i] < tc[i]);
}

/* ── 3. Degenerate clarity fails toward CLEAR ──────────────────────────
 *
 * The other direction hides the scene behind a black slab, which looks like a
 * deliberate art choice and gets debugged as a rendering bug somewhere else. */

static void test_bad_clarity_fails_transparent(void)
{
    const JceWaterExtinction z = jce_water_extinction_from_clarity(0.0f);
    const JceWaterExtinction n = jce_water_extinction_from_clarity(-4.0f);
    float t[3];

    jce_water_transmittance(&z, 5.0f, t);
    TEST_ASSERT_TRUE(t[1] > 0.9f);          /* still basically clear */
    jce_water_transmittance(&n, 5.0f, t);
    TEST_ASSERT_TRUE(t[1] > 0.9f);
}

/* ── 4. Zero path is EXACTLY transparent ───────────────────────────────
 *
 * Not approximately. A waterline whose transmittance starts at 0.99 draws a
 * visible band along every shore, and the band moves with the water, so it
 * reads as foam that someone authored. */

static void test_zero_path_is_exactly_one(void)
{
    const JceWaterExtinction e = jce_water_extinction_from_clarity(2.0f);
    float t[3];

    jce_water_transmittance(&e, 0.0f, t);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t[2]);

    /* A negative path is a caller bug (a point above the surface reaching this
     * code). It must not amplify: exp(-sigma * negative) is greater than 1 and
     * would BRIGHTEN the background, turning a sign error into a glow. */
    jce_water_transmittance(&e, -3.0f, t);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t[2]);
}

/* ── 5. Monotonic, and bounded ─────────────────────────────────────────  */

static void test_transmittance_is_monotonic_and_bounded(void)
{
    const JceWaterExtinction e = jce_water_extinction_from_clarity(8.0f);
    float prev[3] = { 2.0f, 2.0f, 2.0f };
    for (int d = 0; d <= 200; ++d) {
        float t[3];
        jce_water_transmittance(&e, (float)d * 0.5f, t);
        for (int i = 0; i < 3; ++i) {
            TEST_ASSERT_TRUE(t[i] <= prev[i] + 1e-6f);   /* never increases */
            TEST_ASSERT_TRUE(t[i] >= 0.0f && t[i] <= 1.0f);
            prev[i] = t[i];
        }
    }
}

/* ── 6. Deep water converges to the water's colour, not to black ───────
 *
 * Extinction alone drives every channel to zero. Water that goes black with
 * depth looks like a hole in the world; every shipping renderer adds the
 * in-scattered term for exactly this reason. */

static void test_deep_water_converges_to_the_tint(void)
{
    const JceWaterExtinction e = jce_water_extinction_from_clarity(4.0f);
    const float bg[3]   = { 0.9f, 0.85f, 0.8f };   /* bright sand */
    const float tint[3] = { 0.02f, 0.15f, 0.28f }; /* deep blue-green */
    float out[3];

    /* At the surface: pure background, no tint at all. */
    jce_water_apply_absorption(&e, 0.0f, bg, tint, out);
    for (int i = 0; i < 3; ++i) TEST_ASSERT_FLOAT_WITHIN(1e-5f, bg[i], out[i]);

    /* Deep: converged to the tint, and specifically NOT to black. */
    jce_water_apply_absorption(&e, 200.0f, bg, tint, out);
    for (int i = 0; i < 3; ++i) TEST_ASSERT_FLOAT_WITHIN(1e-3f, tint[i], out[i]);
    TEST_ASSERT_TRUE(out[2] > 0.1f);          /* blue survives */

    /* Mid-depth: between the two, and still blue-dominant -- the sand's red
     * has gone while its blue has not. */
    jce_water_apply_absorption(&e, 3.0f, bg, tint, out);
    TEST_ASSERT_TRUE(out[0] < bg[0]);
    TEST_ASSERT_TRUE(out[2] > out[0]);
}

/* ── 7. NULL inputs are handled, not dereferenced ──────────────────────  */

static void test_null_inputs(void)
{
    float out[3] = { -1.0f, -1.0f, -1.0f };
    jce_water_transmittance(NULL, 5.0f, out);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, out[0]);    /* no extinction data => clear */

    const JceWaterExtinction e = jce_water_extinction_from_clarity(5.0f);
    jce_water_apply_absorption(&e, 2.0f, NULL, NULL, out);
    for (int i = 0; i < 3; ++i)
        TEST_ASSERT_TRUE(out[i] >= 0.0f && out[i] <= 1.0f);

    jce_water_transmittance(&e, 1.0f, NULL);  /* must not crash */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_red_is_absorbed_fastest_and_blue_slowest);
    RUN_TEST(test_clarity_is_the_green_1_over_e_distance);
    RUN_TEST(test_bad_clarity_fails_transparent);
    RUN_TEST(test_zero_path_is_exactly_one);
    RUN_TEST(test_transmittance_is_monotonic_and_bounded);
    RUN_TEST(test_deep_water_converges_to_the_tint);
    RUN_TEST(test_null_inputs);
    return UNITY_END();
}
