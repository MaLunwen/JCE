/*
 * test_jce_water_caustics.c
 *
 * Caustic gain from the surface Jacobian.
 *
 * The failure that matters here is a divergence, not a wrong constant:
 * 1/jacobian goes to infinity as the surface approaches a fold, and an
 * unclamped caustic puts a handful of pixels thousands of times brighter than
 * the rest of the scene. That does not read as a bug -- it reads as bloom, and
 * it gets "fixed" by turning bloom down, which removes the symptom from a
 * different feature and leaves the cause in place.
 */

#include "jce_water_caustics.h"

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* ── 1. Compression brightens, expansion dims ──────────────────────────  */

static void test_compression_focuses_light(void)
{
    /* Undisturbed surface: no focusing at all. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_caustic_gain(1.0f, 1.0f));

    /* Compressed to half the area: twice the light. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 2.0f, jce_water_caustic_gain(0.5f, 1.0f));

    /* Stretched to twice the area: half the light.  The dim gaps between the
     * bright bands are as much of the pattern as the bands are; a gain that
     * only ever brightened would wash the floor out uniformly. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.5f, jce_water_caustic_gain(2.0f, 1.0f));

    /* Monotone: more compression is never less light. */
    float prev = 0.0f;
    for (int i = 40; i >= 1; --i) {
        const float g = jce_water_caustic_gain((float)i * 0.05f, 1.0f);
        TEST_ASSERT_TRUE(g >= prev - 1e-6f);
        prev = g;
    }
}

/* ── 2. THE POINT: the divergence is bounded ───────────────────────────  */

static void test_gain_is_clamped_near_a_fold(void)
{
    /* Approach the fold from above.  Unclamped this runs away to 1e6+. */
    const float tiny[5] = { 1e-1f, 1e-2f, 1e-4f, 1e-8f, 1e-30f };
    for (int i = 0; i < 5; ++i) {
        const float g = jce_water_caustic_gain(tiny[i], 1.0f);
        TEST_ASSERT_TRUE(isfinite(g));
        TEST_ASSERT_TRUE_MESSAGE(g <= 4.0f + 1e-4f,
                                 "caustic gain diverges near a fold");
    }

    /* Non-finite input must not propagate: a NaN gain multiplied into the
     * floor colour makes a NaN pixel, which most backends render as black and
     * which looks like a hole in the sea bed. */
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_caustic_gain(nanf(""), 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_caustic_gain(1.0f, nanf("")));
}

/* ── 3. A fold has no caustic ──────────────────────────────────────────
 *
 * A negative Jacobian means the surface has passed through itself; there is no
 * single ray path to the floor. Whitecaps are drawn there from the same
 * quantity, and adding a bright band underneath would double-count one event
 * -- bright foam with a bright patch under it reads as glowing foam. */

static void test_a_fold_produces_no_caustic(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_caustic_gain(-0.5f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_caustic_gain(0.0f, 1.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f, jce_water_caustic_gain(-1e9f, 1.0f));
}

/* ── 4. Strength 0 is EXACTLY off ──────────────────────────────────────
 *
 * The feature ships off, so "off" must be bit-identical to not having it.
 * Scaling the gain instead of blending from 1 agrees at the endpoints and
 * nowhere else -- and the disagreement is largest exactly where the caustic is
 * brightest. */

static void test_strength_zero_is_exactly_neutral(void)
{
    const float js[6] = { 0.01f, 0.25f, 0.5f, 1.0f, 2.0f, 10.0f };
    for (int i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_water_caustic_gain(js[i], 0.0f));
        TEST_ASSERT_EQUAL_FLOAT(1.0f, jce_water_caustic_gain(js[i], -1.0f));
    }

    /* Half strength sits halfway between neutral and full, at every point --
     * that is what makes the slider predictable rather than an exponent. */
    for (int i = 0; i < 6; ++i) {
        const float full = jce_water_caustic_gain(js[i], 1.0f);
        const float half = jce_water_caustic_gain(js[i], 0.5f);
        TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f + (full - 1.0f) * 0.5f, half);
    }
}

/* ── 5. Depth falloff ──────────────────────────────────────────────────  */

static void test_depth_falloff(void)
{
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 1.0f,
        jce_water_caustic_depth_falloff(0.0f, 20.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f,
        jce_water_caustic_depth_falloff(20.0f, 20.0f));
    TEST_ASSERT_FLOAT_WITHIN(1e-5f, 0.0f,
        jce_water_caustic_depth_falloff(1000.0f, 20.0f));

    /* Monotone decreasing, and smooth at BOTH ends: a linear fade kinks where
     * it reaches zero, and on a sloping floor that kink draws a straight line
     * across the sea bed that reads as a terrain seam. */
    float prev = 2.0f;
    for (int i = 0; i <= 100; ++i) {
        const float f = jce_water_caustic_depth_falloff((float)i * 0.2f, 20.0f);
        TEST_ASSERT_TRUE(f <= prev + 1e-6f);
        TEST_ASSERT_TRUE(f >= 0.0f && f <= 1.0f);
        prev = f;
    }
    /* The derivative at the far end is ~0 (that is the smoothstep). */
    const float a = jce_water_caustic_depth_falloff(19.0f, 20.0f);
    const float b = jce_water_caustic_depth_falloff(19.5f, 20.0f);
    const float c = jce_water_caustic_depth_falloff(10.0f, 20.0f);
    const float d = jce_water_caustic_depth_falloff(10.5f, 20.0f);
    TEST_ASSERT_TRUE_MESSAGE((a - b) < (c - d),
        "falloff is linear, not smooth - it will kink where it reaches zero");

    /* A nonsense range disables rather than dividing by it. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_water_caustic_depth_falloff(1.0f, 0.0f));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_compression_focuses_light);
    RUN_TEST(test_gain_is_clamped_near_a_fold);
    RUN_TEST(test_a_fold_produces_no_caustic);
    RUN_TEST(test_strength_zero_is_exactly_neutral);
    RUN_TEST(test_depth_falloff);
    return UNITY_END();
}
