/*
 * test_jce_weather.c — Unit tests for jce_weather.h (L4 world).
 *
 * The runtime requires bgfx + a pak archive to fully initialize, so we
 * only exercise the pure helpers:
 *   - jce_weather_default()  (preset table)
 *   - NULL-safety of every public entry point
 */

#include "unity.h"

#include <jce/middleware/world/jce_weather.h>

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Defaults                                                             */
/* ------------------------------------------------------------------ */

static void test_default_clear_zeroes_intensity(void)
{
    JceWeatherState s = jce_weather_default(JCE_WEATHER_CLEAR, 0.8f);
    TEST_ASSERT_EQUAL_INT(JCE_WEATHER_CLEAR, (int)s.type);
    /* Default code overrides intensity to 0 for CLEAR. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.intensity);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, s.wetness);
}

static void test_default_rain_sets_wetness(void)
{
    JceWeatherState s = jce_weather_default(JCE_WEATHER_RAIN, 1.0f);
    TEST_ASSERT_EQUAL_INT(JCE_WEATHER_RAIN, (int)s.type);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, s.intensity);
    TEST_ASSERT_TRUE(s.wetness > 0.0f);
    TEST_ASSERT_TRUE(s.wind_strength > 0.0f);
    /* Tint should be slightly blue-ish. */
    TEST_ASSERT_TRUE(s.tint.z >= s.tint.x);
}

static void test_default_snow_dampens_wind(void)
{
    JceWeatherState rain = jce_weather_default(JCE_WEATHER_RAIN, 1.0f);
    JceWeatherState snow = jce_weather_default(JCE_WEATHER_SNOW, 1.0f);
    TEST_ASSERT_EQUAL_INT(JCE_WEATHER_SNOW, (int)snow.type);
    /* Snow wind is half rain's (1.5 vs 3.0 per default impl). */
    TEST_ASSERT_TRUE(snow.wind_strength < rain.wind_strength);
    TEST_ASSERT_EQUAL_FLOAT(0.0f, snow.wetness);
}

/* ------------------------------------------------------------------ */
/* NULL-safety                                                          */
/* ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    /* create rejects null desc or null pak. */
    TEST_ASSERT_NULL(jce_weather_create(NULL));
    JceWeatherDesc empty = { NULL };
    TEST_ASSERT_NULL(jce_weather_create(&empty));

    /* Destroy / update / render / set / get must not crash on NULL. */
    jce_weather_destroy(NULL);
    jce_weather_update(NULL, 0.016f);
    jce_weather_render(NULL, 0);
    jce_weather_set_state(NULL, NULL);

    JceWeatherState s = jce_weather_get_state(NULL);
    /* get_state on NULL must return a CLEAR default. */
    TEST_ASSERT_EQUAL_INT(JCE_WEATHER_CLEAR, (int)s.type);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_clear_zeroes_intensity);
    RUN_TEST(test_default_rain_sets_wetness);
    RUN_TEST(test_default_snow_dampens_wind);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
