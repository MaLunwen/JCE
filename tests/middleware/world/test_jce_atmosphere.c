/*
 * test_jce_atmosphere.c
 *
 * The sunset must be physics, not a curve.
 *
 * These lock the properties that make transmittance-derived sun colour worth
 * having at all.  They are deliberately stated as physical invariants rather
 * than golden numbers, so the test survives a coefficient re-tune but still
 * fails if the model stops behaving like an atmosphere.
 */

#include <jce/middleware/world/jce_atmosphere.h>

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static const jce_vec3 k_up = { 0.0f, 1.0f, 0.0f };

/* Sun direction at a given elevation above the horizon. */
static jce_vec3 sun_at(float elevation_deg)
{
    float r = elevation_deg * 3.14159265358979f / 180.0f;
    jce_vec3 d;
    d.x = cosf(r);
    d.y = sinf(r);
    d.z = 0.0f;
    return d;
}

/* ── 1. Transmittance is a fraction ────────────────────────────────── */

static void test_transmittance_is_bounded(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    for (float e = -10.0f; e <= 90.0f; e += 5.0f) {
        jce_vec3 t = jce_atmosphere_transmittance(&p, 0.0f, sun_at(e), k_up);
        TEST_ASSERT_TRUE(t.x >= 0.0f && t.x <= 1.0f);
        TEST_ASSERT_TRUE(t.y >= 0.0f && t.y <= 1.0f);
        TEST_ASSERT_TRUE(t.z >= 0.0f && t.z <= 1.0f);
        TEST_ASSERT_FALSE(isnan(t.x) || isnan(t.y) || isnan(t.z));
    }
}

/* ── 2. Overhead sun is bright and near-neutral ────────────────────── */

static void test_zenith_is_bright_and_neutral(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    jce_vec3 t = jce_atmosphere_transmittance(&p, 0.0f, sun_at(90.0f), k_up);

    /* Short path: most light survives on every channel. */
    TEST_ASSERT_TRUE(t.x > 0.80f);
    TEST_ASSERT_TRUE(t.z > 0.55f);

    /* Near-neutral: red and blue within a modest ratio of each other. */
    TEST_ASSERT_TRUE(t.x / t.z < 1.8f);
}

/* ── 3. Light dims monotonically as the sun sets ───────────────────── */

static void test_monotonic_dimming(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    float prev = 2.0f;
    for (float e = 90.0f; e >= 1.0f; e -= 5.0f) {
        jce_vec3 t = jce_atmosphere_transmittance(&p, 0.0f, sun_at(e), k_up);
        float lum = 0.2126f * t.x + 0.7152f * t.y + 0.0722f * t.z;
        TEST_ASSERT_TRUE(lum <= prev + 1e-5f);
        prev = lum;
    }
}

/* ── 4. THE POINT: blue extinguishes faster than red ───────────────── */

static void test_horizon_reddens(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    jce_vec3 noon = jce_atmosphere_transmittance(&p, 0.0f, sun_at(90.0f), k_up);
    jce_vec3 low  = jce_atmosphere_transmittance(&p, 0.0f, sun_at(2.0f),  k_up);

    /* Red/blue ratio must grow sharply toward the horizon -- that ratio IS
     * the sunset colour, and it is what a hand-authored curve was faking. */
    float noon_ratio = noon.x / (noon.z + 1e-6f);
    float low_ratio  = low.x  / (low.z  + 1e-6f);
    TEST_ASSERT_TRUE(low_ratio > noon_ratio * 3.0f);

    /* And the low sun must actually be red-dominant. */
    TEST_ASSERT_TRUE(low.x > low.y);
    TEST_ASSERT_TRUE(low.y > low.z);
}

/* ── 5. Magnitude matches measured daylight ────────────────────────── */

static void test_noon_illuminance_is_physical(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    jce_vec3 e = jce_atmosphere_sun_illuminance(&p, 0.0f, sun_at(90.0f), k_up);
    float lum = 0.2126f * e.x + 0.7152f * e.y + 0.0722f * e.z;

    /* Measured clear-sky perpendicular sun illuminance at ground level is
     * ~100,000-105,000 lx around midday.  Allow a generous band: the point is
     * the ORDER OF MAGNITUDE is physical, not that we match one dataset. */
    TEST_ASSERT_TRUE(lum > 70000.0f);
    TEST_ASSERT_TRUE(lum < 140000.0f);
}

/* ── 6. Below the horizon it goes dark, but not discontinuously ────── */

static void test_below_horizon_falls_off_smoothly(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();

    jce_vec3 a = jce_atmosphere_sun_illuminance(&p, 0.0f, sun_at(0.5f),  k_up);
    jce_vec3 b = jce_atmosphere_sun_illuminance(&p, 0.0f, sun_at(-0.5f), k_up);
    jce_vec3 c = jce_atmosphere_sun_illuminance(&p, 0.0f, sun_at(-8.0f), k_up);

    float la = 0.2126f*a.x + 0.7152f*a.y + 0.0722f*a.z;
    float lb = 0.2126f*b.x + 0.7152f*b.y + 0.0722f*b.z;
    float lc = 0.2126f*c.x + 0.7152f*c.y + 0.0722f*c.z;

    TEST_ASSERT_TRUE(lb <= la);          /* still dimming */
    TEST_ASSERT_TRUE(lc < lb);           /* darker deeper down */
    TEST_ASSERT_TRUE(lc >= 0.0f);        /* never negative */
    TEST_ASSERT_TRUE(lc < la * 0.5f);    /* genuinely night, not a plateau */
}

/* ── 7. Altitude thins the atmosphere above you ────────────────────── */

static void test_higher_altitude_transmits_more(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    jce_vec3 ground = jce_atmosphere_transmittance(&p, 0.0f,  sun_at(20.0f), k_up);
    jce_vec3 high   = jce_atmosphere_transmittance(&p, 10.0f, sun_at(20.0f), k_up);
    TEST_ASSERT_TRUE(high.z > ground.z);
    TEST_ASSERT_TRUE(high.x >= ground.x);
}

/* ── 8. Determinism ────────────────────────────────────────────────── */

static void test_deterministic(void)
{
    JceAtmosphereParams p = jce_atmosphere_default_params();
    for (int i = 0; i < 8; i++) {
        jce_vec3 a = jce_atmosphere_transmittance(&p, 0.0f, sun_at(12.5f), k_up);
        jce_vec3 b = jce_atmosphere_transmittance(&p, 0.0f, sun_at(12.5f), k_up);
        TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof a);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_transmittance_is_bounded);
    RUN_TEST(test_zenith_is_bright_and_neutral);
    RUN_TEST(test_monotonic_dimming);
    RUN_TEST(test_horizon_reddens);
    RUN_TEST(test_noon_illuminance_is_physical);
    RUN_TEST(test_below_horizon_falls_off_smoothly);
    RUN_TEST(test_higher_altitude_transmits_more);
    RUN_TEST(test_deterministic);
    return UNITY_END();
}
