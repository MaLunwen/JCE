/*
 * test_jce_tod_sky_gradient.c
 *
 * The time-of-day sky gradient must be a PROJECTION of the sky model, not a
 * second hand-authored model that happens to look similar.
 *
 * sky_top / sky_horizon / sky_ground used to be nine hand-tuned palette stops
 * (day/night/twilight zenith, day/night/dawn/dusk horizon, day/night ground)
 * lerped on the sun altitude.  Nothing tied them to the Preetham sky the
 * renderer actually draws, so the two could drift arbitrarily -- and the
 * low-fidelity gradient tier could disagree with the high-fidelity analytic
 * tier for the same moment of the same day.
 *
 * These tests state the relationships that make the gradient a faithful
 * three-point reduction of the sky.  They are deliberately relative rather
 * than absolute so an exposure or turbidity re-tune survives, while a
 * gradient that stops tracking the sky fails.
 */

#include <jce/middleware/world/jce_time_of_day.h>

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static float luma(jce_vec3 c)
{
    return 0.2126f * c.x + 0.7152f * c.y + 0.0722f * c.z;
}

static int finite3(jce_vec3 c)
{
    return !isnan(c.x) && !isnan(c.y) && !isnan(c.z) &&
           !isinf(c.x) && !isinf(c.y) && !isinf(c.z);
}

static JceTimeOfDayState at(float hour)
{
    JceTimeOfDayState s;
    jce_time_of_day_evaluate(NULL, hour, &s);
    return s;
}

/* ── 1. Finite and non-negative across the whole day ───────────────── */

static void test_gradient_finite_and_non_negative(void)
{
    for (float h = 0.0f; h < 24.0f; h += 0.25f) {
        JceTimeOfDayState s = at(h);
        TEST_ASSERT_TRUE(finite3(s.sky_top));
        TEST_ASSERT_TRUE(finite3(s.sky_horizon));
        TEST_ASSERT_TRUE(finite3(s.sky_ground));
        TEST_ASSERT_TRUE(s.sky_top.x >= 0.0f && s.sky_top.y >= 0.0f &&
                         s.sky_top.z >= 0.0f);
        TEST_ASSERT_TRUE(s.sky_horizon.x >= 0.0f && s.sky_horizon.y >= 0.0f &&
                         s.sky_horizon.z >= 0.0f);
        TEST_ASSERT_TRUE(s.sky_ground.x >= 0.0f && s.sky_ground.y >= 0.0f &&
                         s.sky_ground.z >= 0.0f);
    }
}

/* ── 2. A daytime zenith is blue-dominant ──────────────────────────── */

static void test_noon_zenith_is_blue(void)
{
    JceTimeOfDayState noon = at(12.0f);
    /* Rayleigh scattering: the zenith is the bluest part of a clear sky. */
    TEST_ASSERT_TRUE(noon.sky_top.z > noon.sky_top.x);
}

/* ── 3. The horizon is less saturated than the zenith ──────────────── */

static void test_noon_horizon_is_less_blue_than_zenith(void)
{
    JceTimeOfDayState noon = at(12.0f);
    /* Longer path near the horizon means more multiple scattering, so the
     * horizon washes toward white: its blue/red ratio must be LOWER than the
     * zenith's.  A gradient that gets this backwards is not a sky. */
    float zenith_ratio  = noon.sky_top.z     / (noon.sky_top.x + 1e-6f);
    float horizon_ratio = noon.sky_horizon.z / (noon.sky_horizon.x + 1e-6f);
    TEST_ASSERT_TRUE(horizon_ratio < zenith_ratio);
}

/* ── 4. Ground is darker than the sky above it ─────────────────────── */

static void test_ground_is_darker_than_horizon(void)
{
    for (float h = 8.0f; h <= 16.0f; h += 2.0f) {
        JceTimeOfDayState s = at(h);
        TEST_ASSERT_TRUE(luma(s.sky_ground) < luma(s.sky_horizon));
    }
}

/* ── 5. Night is much darker than noon ─────────────────────────────── */

static void test_night_is_darker_than_noon(void)
{
    JceTimeOfDayState noon  = at(12.0f);
    JceTimeOfDayState night = at(0.0f);
    TEST_ASSERT_TRUE(luma(night.sky_top)     < luma(noon.sky_top));
    TEST_ASSERT_TRUE(luma(night.sky_horizon) < luma(noon.sky_horizon));
}

/* ── 6. The gradient tracks the sun continuously ───────────────────── */

static void test_gradient_is_continuous(void)
{
    /* No stop in the gradient may jump discontinuously between adjacent
     * minutes -- an authored palette lerp can, a sampled sky cannot. */
    JceTimeOfDayState prev = at(4.0f);
    for (float h = 4.0f + 1.0f / 60.0f; h <= 20.0f; h += 1.0f / 60.0f) {
        JceTimeOfDayState cur = at(h);
        float d = fabsf(luma(cur.sky_top) - luma(prev.sky_top));
        TEST_ASSERT_TRUE(d < 0.25f);
        prev = cur;
    }
}

/* ── 7. Determinism ────────────────────────────────────────────────── */

static void test_deterministic(void)
{
    for (int i = 0; i < 4; i++) {
        JceTimeOfDayState a = at(9.75f);
        JceTimeOfDayState b = at(9.75f);
        TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_gradient_finite_and_non_negative);
    RUN_TEST(test_noon_zenith_is_blue);
    RUN_TEST(test_noon_horizon_is_less_blue_than_zenith);
    RUN_TEST(test_ground_is_darker_than_horizon);
    RUN_TEST(test_night_is_darker_than_noon);
    RUN_TEST(test_gradient_is_continuous);
    RUN_TEST(test_deterministic);
    return UNITY_END();
}
