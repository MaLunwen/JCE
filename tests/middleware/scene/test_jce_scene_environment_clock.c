/*
 * test_jce_scene_environment_clock.c
 *
 * The scene's environment must advance WITHOUT A RENDERER.
 *
 * That sentence is the whole test.  JceEnvironmentState is scene-owned -- its
 * header says "on the SCENE and not on the renderer, so that physics and
 * gameplay can read the same wind the renderer draws with" -- but until
 * 2026-09-01 the only thing that advanced it was sr_advance_environment_state,
 * inside the scene RENDERER.  jce_environment_advance had exactly one caller in
 * the repository and that was it.
 *
 * A headless build creates no renderer.  The engine says so out loud on boot
 * ("HEADLESS boot: no window, no GPU device, no audio/UI") and that is the
 * shipped dedicated-server mode.  So on a server:
 *
 *   - world_time_seconds never advanced, and the gust envelope is a pure
 *     function of it, so the wind never gusted;
 *   - global_wetness and snow_amount are INTEGRATORS and integrated nothing;
 *   - nothing placed the sun, because advance() moves day_fraction and does not
 *     touch sun_direction_ws, whose only writer was also the renderer -- so
 *     jce_environment_is_daytime() returned TRUE forever, the default sun being
 *     above the horizon.  That is the predicate gameplay asks "is it night?"
 *     with.
 *
 * WHY THIS TEST CAN EXIST AT ALL is itself the point: it constructs a scene and
 * calls jce_scene_environment_advance() with no renderer anywhere in the
 * process.  Before the fix there was no way to write that line -- the advance
 * was a static function reached only through a renderer -- which is exactly why
 * the defect survived: every configuration anybody could test had a renderer in
 * it.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 * The tracked contract is tools/lint/check_environment_authority.py.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/world/jce_environment.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* One in-game hour per real second, so a 1 s step is exactly 1 h and the
 * arithmetic in the assertions is readable rather than derived. */
#define TOD_SPEED_HOURS_PER_SEC 1.0f

static JceScene *make_scene(float start_hour, int weather, float intensity)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceSceneRenderingSettings rs;
    memset(&rs, 0, sizeof rs);
    rs.tod_enabled      = true;
    rs.tod_hour         = start_hour;
    rs.tod_speed        = TOD_SPEED_HOURS_PER_SEC;
    rs.tod_latitude     = 35.0f;
    rs.tod_dawn_hour    = 6.0f;
    rs.tod_dusk_hour    = 18.0f;
    rs.weather_type     = weather;
    rs.weather_intensity = intensity;
    rs.temperature_c    = 15.0f;
    jce_scene_set_rendering_settings(s, &rs);
    return s;
}

/* dt-per-step small enough that the integrators behave, total = `seconds`. */
static void run_seconds(JceScene *s, float seconds)
{
    const float step = 1.0f / 60.0f;
    const int   n    = (int)(seconds / step + 0.5f);
    for (int i = 0; i < n; ++i)
        jce_scene_environment_advance(s, step);
}

/* ── 1. The clock moves, and it moves at the authored rate. ───────────── */
static void test_hour_advances_without_a_renderer(void)
{
    JceScene *s = make_scene(8.0f, 0, 0.0f);

    /* First advance seeds from the authored hour. */
    jce_scene_environment_advance(s, 1.0f / 60.0f);
    const float h0 = jce_scene_environment_hour(s);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, 8.0f, h0,
        "the clock did not seed from the authored hour");

    run_seconds(s, 3.0f);                       /* 3 s = 3 in-game hours */
    const float h1 = jce_scene_environment_hour(s);
    jce_scene_destroy(s);

    /* Before the fix this assertion could not be written: there was no
     * accessor, and nothing advanced anything without a renderer. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, 11.0f, h1,
        "the environment clock did not advance with no renderer present -- "
        "the advance is back in the renderer, so a dedicated server is frozen");
}

/* ── 2. The gameplay predicate follows it. ────────────────────────────── */
static void test_is_daytime_follows_the_clock(void)
{
    /* Start an hour before dawn: night. */
    JceScene *s = make_scene(5.0f, 0, 0.0f);
    jce_scene_environment_advance(s, 1.0f / 60.0f);

    const JceEnvironmentState *env = jce_scene_environment(s);
    TEST_ASSERT_NOT_NULL(env);
    TEST_ASSERT_FALSE_MESSAGE(jce_environment_is_daytime(env),
        "05:00 reads as daytime -- nothing placed the sun, so the state still "
        "holds jce_environment_default()'s, which is above the horizon");

    /* Advance past dawn into the middle of the day. */
    run_seconds(s, 7.0f);                       /* 05:00 -> 12:00 */
    TEST_ASSERT_TRUE_MESSAGE(jce_environment_is_daytime(env),
        "12:00 does not read as daytime");

    /* And on into the night. */
    run_seconds(s, 10.0f);                      /* 12:00 -> 22:00 */
    TEST_ASSERT_FALSE_MESSAGE(jce_environment_is_daytime(env),
        "22:00 does not read as night");

    jce_scene_destroy(s);
}

/* ── 3. The integrators integrate. ───────────────────────────────────── */
static void test_wetness_integrates_under_rain(void)
{
    /* Weather 1 = rain, at full intensity. */
    JceScene *s = make_scene(12.0f, 1, 1.0f);
    const JceEnvironmentState *env = jce_scene_environment(s);
    TEST_ASSERT_NOT_NULL(env);

    const float dry = env->global_wetness;
    run_seconds(s, 30.0f);
    const float wet = env->global_wetness;
    jce_scene_destroy(s);

    /* global_wetness approaches precipitation_rate over time.  Any movement at
     * all is the assertion: before the fix it was pinned at its initial value
     * for the life of a headless process, because nothing called advance. */
    TEST_ASSERT_TRUE_MESSAGE(wet > dry + 0.05f,
        "ground never got wet under 30 s of full rain -- the integrators are "
        "not being stepped");
}

/* ── 4. Setting the hour is honoured, and does not disturb the seed. ─── */
static void test_set_hour_jumps_the_live_clock(void)
{
    JceScene *s = make_scene(8.0f, 0, 0.0f);
    jce_scene_environment_advance(s, 1.0f / 60.0f);

    jce_scene_environment_set_hour(s, 21.5f);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.01f, 21.5f,
        jce_scene_environment_hour(s), "set_hour did not take");

    /* And it keeps running from there rather than snapping back to the seed:
     * the re-seed test compares against the seed we LAST APPLIED, not against
     * the live hour, or a running clock would re-seed itself every frame. */
    run_seconds(s, 1.0f);
    const float h = jce_scene_environment_hour(s);
    jce_scene_destroy(s);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, 22.5f, h,
        "the clock snapped back to the authored seed after a set_hour -- the "
        "re-seed guard is comparing against the live hour");
}

/* ── 5. Wrapping, including the values a text field can produce. ─────── */
static void test_hour_wraps(void)
{
    JceScene *s = make_scene(12.0f, 0, 0.0f);
    jce_scene_environment_advance(s, 1.0f / 60.0f);

    jce_scene_environment_set_hour(s, 26.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 2.0f, jce_scene_environment_hour(s));

    jce_scene_environment_set_hour(s, -3.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.01f, 21.0f, jce_scene_environment_hour(s));

    /* Crossing midnight while running must not stall or go negative. */
    jce_scene_environment_set_hour(s, 23.5f);
    run_seconds(s, 1.0f);
    const float h = jce_scene_environment_hour(s);
    jce_scene_destroy(s);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, 0.5f, h,
        "the clock did not wrap through midnight");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hour_advances_without_a_renderer);
    RUN_TEST(test_is_daytime_follows_the_clock);
    RUN_TEST(test_wetness_integrates_under_rain);
    RUN_TEST(test_set_hour_jumps_the_live_clock);
    RUN_TEST(test_hour_wraps);
    return UNITY_END();
}
