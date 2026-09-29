/*
 * test_jce_tod_publish.c
 *
 * The time-of-day snapshot must actually reach consumers.
 *
 * fog_color, fog_density and exposure were computed for every frame of every
 * day/night cycle and read by NOTHING -- a repo-wide search for reads of
 * tod_state.fog_color / .fog_density / .exposure returned zero.  The visible
 * consequence: a scene running a full day/night cycle kept its daytime fog
 * colour, daytime fog thickness and daytime exposure at midnight.
 *
 * These lock the publish contract: an inactive cycle is a pass-through (so
 * scenes without a cycle are bit-identical), and an active cycle moves the
 * authored value in the physically expected direction.
 */

#include <jce/middleware/world/jce_time_of_day.h>

#include <math.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

static JceTimeOfDayState at(float hour)
{
    JceTimeOfDayState s;
    jce_time_of_day_evaluate(NULL, hour, &s);
    return s;
}

/* ── 1. No cycle means no change ───────────────────────────────────── */

static void test_inactive_cycle_is_pass_through(void)
{
    const float authored[3] = { 0.4f, 0.5f, 0.6f };
    float out[3] = { -1.0f, -1.0f, -1.0f };
    float density = -1.0f;

    jce_time_of_day_resolve_fog(NULL, authored, 0.0125f, out, &density);

    TEST_ASSERT_EQUAL_FLOAT(authored[0], out[0]);
    TEST_ASSERT_EQUAL_FLOAT(authored[1], out[1]);
    TEST_ASSERT_EQUAL_FLOAT(authored[2], out[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.0125f, density);

    TEST_ASSERT_EQUAL_FLOAT(2.5f,
                            jce_time_of_day_resolve_exposure(NULL, 2.5f));
}

/* ── 2. An active cycle actually changes the fog ───────────────────── */

static void test_active_cycle_drives_fog_colour(void)
{
    JceTimeOfDayState noon  = at(12.0f);
    JceTimeOfDayState night = at(0.0f);

    const float authored[3] = { 0.4f, 0.5f, 0.6f };
    float cn[3], cm[3], dn, dm;

    jce_time_of_day_resolve_fog(&noon,  authored, 0.0025f, cn, &dn);
    jce_time_of_day_resolve_fog(&night, authored, 0.0025f, cm, &dm);

    /* The two moments must not produce the same fog -- that was the bug. */
    float diff = fabsf(cn[0] - cm[0]) + fabsf(cn[1] - cm[1]) +
                 fabsf(cn[2] - cm[2]);
    TEST_ASSERT_TRUE(diff > 1e-3f);
}

/* ── 3. Night is hazier than noon ──────────────────────────────────── */

static void test_night_fog_is_thicker(void)
{
    JceTimeOfDayState noon  = at(12.0f);
    JceTimeOfDayState night = at(0.0f);

    float c[3], d_noon, d_night;
    const float authored[3] = { 0.5f, 0.5f, 0.5f };
    jce_time_of_day_resolve_fog(&noon,  authored, 0.0025f, c, &d_noon);
    jce_time_of_day_resolve_fog(&night, authored, 0.0025f, c, &d_night);

    TEST_ASSERT_TRUE(d_night > d_noon);
    TEST_ASSERT_TRUE(d_noon >= 0.0f);
}

/* ── 4. Authored thickness is scaled, not discarded ────────────────── */

static void test_authored_density_is_respected(void)
{
    JceTimeOfDayState noon = at(12.0f);
    const float authored[3] = { 0.5f, 0.5f, 0.5f };
    float c[3], thin, thick;

    jce_time_of_day_resolve_fog(&noon, authored, 0.001f, c, &thin);
    jce_time_of_day_resolve_fog(&noon, authored, 0.010f, c, &thick);

    /* An artist who authored ten times the fog must still get more fog. */
    TEST_ASSERT_TRUE(thick > thin);
}

/* ── 5. Night is dimmer than noon ──────────────────────────────────── */

static void test_night_exposure_is_lower(void)
{
    JceTimeOfDayState noon  = at(12.0f);
    JceTimeOfDayState night = at(0.0f);

    float e_noon  = jce_time_of_day_resolve_exposure(&noon,  1.0f);
    float e_night = jce_time_of_day_resolve_exposure(&night, 1.0f);

    TEST_ASSERT_TRUE(e_night < e_noon);
    TEST_ASSERT_TRUE(e_night > 0.0f);
}

/* ── 6. Authored exposure is scaled, not discarded ─────────────────── */

static void test_authored_exposure_is_respected(void)
{
    JceTimeOfDayState noon = at(12.0f);
    float a = jce_time_of_day_resolve_exposure(&noon, 1.0f);
    float b = jce_time_of_day_resolve_exposure(&noon, 2.0f);
    TEST_ASSERT_TRUE(b > a);
}

/* ── 7. Null outputs are tolerated ─────────────────────────────────── */

static void test_null_outputs_are_safe(void)
{
    JceTimeOfDayState noon = at(12.0f);
    const float authored[3] = { 0.5f, 0.5f, 0.5f };
    float only_density = -1.0f;
    float only_color[3] = { -1, -1, -1 };

    jce_time_of_day_resolve_fog(&noon, authored, 0.0025f, NULL, &only_density);
    TEST_ASSERT_TRUE(only_density >= 0.0f);

    jce_time_of_day_resolve_fog(&noon, authored, 0.0025f, only_color, NULL);
    TEST_ASSERT_TRUE(only_color[0] >= 0.0f);

    jce_time_of_day_resolve_fog(&noon, NULL, 0.0025f, only_color, NULL);
    TEST_ASSERT_TRUE(only_color[0] >= 0.0f);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_inactive_cycle_is_pass_through);
    RUN_TEST(test_active_cycle_drives_fog_colour);
    RUN_TEST(test_night_fog_is_thicker);
    RUN_TEST(test_authored_density_is_respected);
    RUN_TEST(test_night_exposure_is_lower);
    RUN_TEST(test_authored_exposure_is_respected);
    RUN_TEST(test_null_outputs_are_safe);
    return UNITY_END();
}
