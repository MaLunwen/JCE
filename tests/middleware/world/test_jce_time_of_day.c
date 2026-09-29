/* test_jce_time_of_day.c
 *
 * Pure-CPU unit tests for the analytical day/night driver.
 */

#include <jce/middleware/world/jce_time_of_day.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-3f

static float vec_len(jce_vec3 v)
{
    return sqrtf(v.x * v.x + v.y * v.y + v.z * v.z);
}

static void test_default_config_has_sane_values(void)
{
    JceTimeOfDayConfig c = jce_time_of_day_default_config();
    TEST_ASSERT_FLOAT_WITHIN(EPS,  6.0f, c.dawn_hour);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 18.0f, c.dusk_hour);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 35.0f, c.latitude_degrees);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, vec_len(c.up_axis));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, vec_len(c.north_axis));
}

static void test_evaluate_null_out_is_no_op(void)
{
    jce_time_of_day_evaluate(NULL, 12.0f, NULL);
}

static void test_evaluate_null_cfg_uses_default(void)
{
    JceTimeOfDayState s;
    memset(&s, 0, sizeof(s));
    jce_time_of_day_evaluate(NULL, 12.0f, &s);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, vec_len(s.sun_direction));
    TEST_ASSERT_FALSE(s.is_night);
}

static void test_sun_direction_is_unit_across_full_day(void)
{
    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    for (int i = 0; i < 48; ++i) {
        float h = (float)i * 0.5f;
        JceTimeOfDayState s;
        jce_time_of_day_evaluate(&cfg, h, &s);
        TEST_ASSERT_FLOAT_WITHIN(2e-3f, 1.0f, vec_len(s.sun_direction));
    }
}

static void test_hour_wrapping_matches_within_period(void)
{
    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    JceTimeOfDayState a, b, c;
    jce_time_of_day_evaluate(&cfg,  3.0f,  &a);
    jce_time_of_day_evaluate(&cfg, 27.0f,  &b);
    jce_time_of_day_evaluate(&cfg, -21.0f, &c);
    TEST_ASSERT_FLOAT_WITHIN(EPS, a.sun_direction.x, b.sun_direction.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, a.sun_direction.y, b.sun_direction.y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, a.sun_direction.z, b.sun_direction.z);
    TEST_ASSERT_FLOAT_WITHIN(EPS, a.sun_direction.x, c.sun_direction.x);
}

static void test_is_night_flips_around_dawn_and_dusk(void)
{
    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    JceTimeOfDayState midnight, noon, predawn, postdawn;
    jce_time_of_day_evaluate(&cfg,  0.0f, &midnight);
    jce_time_of_day_evaluate(&cfg, 12.0f, &noon);
    jce_time_of_day_evaluate(&cfg,  4.0f, &predawn);
    jce_time_of_day_evaluate(&cfg, 10.0f, &postdawn);
    TEST_ASSERT_TRUE(midnight.is_night);
    TEST_ASSERT_FALSE(noon.is_night);
    TEST_ASSERT_TRUE(predawn.is_night);
    TEST_ASSERT_FALSE(postdawn.is_night);
}

static void test_night_is_dimmer_and_foggier_than_noon(void)
{
    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    JceTimeOfDayState noon, midnight;
    jce_time_of_day_evaluate(&cfg, 12.0f, &noon);
    jce_time_of_day_evaluate(&cfg,  0.0f, &midnight);
    TEST_ASSERT_TRUE(noon.exposure > midnight.exposure);
    TEST_ASSERT_TRUE(noon.fog_density < midnight.fog_density);
    TEST_ASSERT_TRUE(noon.sun_intensity > midnight.sun_intensity);
}

static void test_sun_intensity_matches_color_length(void)
{
    JceTimeOfDayConfig cfg = jce_time_of_day_default_config();
    for (int i = 0; i < 12; ++i) {
        float h = (float)i * 2.0f;
        JceTimeOfDayState s;
        jce_time_of_day_evaluate(&cfg, h, &s);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, vec_len(s.sun_color), s.sun_intensity);
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_config_has_sane_values);
    RUN_TEST(test_evaluate_null_out_is_no_op);
    RUN_TEST(test_evaluate_null_cfg_uses_default);
    RUN_TEST(test_sun_direction_is_unit_across_full_day);
    RUN_TEST(test_hour_wrapping_matches_within_period);
    RUN_TEST(test_is_night_flips_around_dawn_and_dusk);
    RUN_TEST(test_night_is_dimmer_and_foggier_than_noon);
    RUN_TEST(test_sun_intensity_matches_color_length);
    return UNITY_END();
}
