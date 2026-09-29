/*
 * test_jce_save_env_roundtrip.c
 *
 * A save remembers the hour it was written at.
 *
 * The scene's environment holds four values a session EARNS and cannot
 * re-derive: the hour of day, the monotonic world_time_seconds behind it, and
 * the global_wetness / snow_amount integrators.  Everything else in the state
 * is recomputed from the authored settings on the next advance, so it is a
 * cache and does not belong in a save file.
 *
 * They are also deliberately NOT in the scene_ecs section, which serializes the
 * scene through its authored JSON schema: a running clock in the authored scene
 * file means saving a level from the editor bakes whatever hour the preview had
 * reached into it.  Hence a separate "scene_env" section.
 *
 * THE CASE THAT WOULD SILENTLY BREAK IT is test 3.  Restoring the hour is not
 * enough on its own: the advance re-seeds the day whenever the authored hour
 * differs from the seed it last applied, so a load that set the hour but left
 * day_seed_hour holding the scene's start-of-level value would look, on the
 * very next frame, exactly like a designer having moved the slider -- and the
 * restored save would snap back to the authored hour.  A round-trip test that
 * asserted immediately after load would pass while the game was still broken.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */

#include <jce/middleware/save/jce_save_providers.h>
#include <jce/middleware/save/jce_snapshot.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/world/jce_environment.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define SAVE_PATH "test_jce_save_env_roundtrip.jsnp"
#define AUTHORED_HOUR 8.0f

static JceScene *make_scene(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceSceneRenderingSettings rs;
    memset(&rs, 0, sizeof rs);
    rs.tod_enabled       = true;
    rs.tod_hour          = AUTHORED_HOUR;
    rs.tod_speed         = 1.0f;          /* 1 in-game hour per real second */
    rs.tod_latitude      = 35.0f;
    rs.tod_dawn_hour     = 6.0f;
    rs.tod_dusk_hour     = 18.0f;
    rs.weather_type      = 1;             /* rain, so wetness integrates */
    rs.weather_intensity = 1.0f;
    rs.temperature_c     = 15.0f;
    jce_scene_set_rendering_settings(s, &rs);
    return s;
}

static void run_seconds(JceScene *s, float seconds)
{
    const float step = 1.0f / 60.0f;
    const int   n    = (int)(seconds / step + 0.5f);
    for (int i = 0; i < n; ++i)
        jce_scene_environment_advance(s, step);
}

static bool save_to(JceScene *s, const char *path)
{
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    if (!reg) return false;
    jce_save_register_env_provider(reg, s);
    const bool ok = jce_snapshot_save_to_file(reg, path);
    jce_snapshot_registry_destroy(reg);
    return ok;
}

static bool load_into(JceScene *s, const char *path)
{
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    if (!reg) return false;
    jce_save_register_env_provider(reg, s);
    const bool ok = jce_snapshot_load_from_file(reg, path);
    jce_snapshot_registry_destroy(reg);
    return ok;
}

/* ── 1. The hour survives the round trip. ─────────────────────────────── */
static void test_hour_round_trips(void)
{
    JceScene *a = make_scene();
    run_seconds(a, 12.0f);                    /* 08:00 -> 20:00 */
    const float saved = jce_scene_environment_hour(a);
    TEST_ASSERT_TRUE(saved > 19.0f && saved < 21.0f);
    TEST_ASSERT_TRUE_MESSAGE(save_to(a, SAVE_PATH), "save failed");
    jce_scene_destroy(a);

    JceScene *b = make_scene();               /* fresh: authored 08:00 */
    TEST_ASSERT_TRUE_MESSAGE(load_into(b, SAVE_PATH), "load failed");
    const float got = jce_scene_environment_hour(b);
    jce_scene_destroy(b);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.05f, saved, got,
        "the saved hour did not come back -- the save resumes at the scene's "
        "authored hour, so a game saved at dusk reloads in the morning");
}

/* ── 2. The integrators survive too. ──────────────────────────────────── */
static void test_wetness_round_trips(void)
{
    JceScene *a = make_scene();
    run_seconds(a, 30.0f);                    /* rain at full intensity */
    const float wet = jce_scene_environment(a)->global_wetness;
    TEST_ASSERT_TRUE_MESSAGE(wet > 0.05f, "the rig did not wet the ground");
    TEST_ASSERT_TRUE(save_to(a, SAVE_PATH));
    jce_scene_destroy(a);

    JceScene *b = make_scene();
    TEST_ASSERT_TRUE(load_into(b, SAVE_PATH));
    const float got = jce_scene_environment(b)->global_wetness;
    jce_scene_destroy(b);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.02f, wet, got,
        "wetness reset on load -- the ground goes dry in the middle of the "
        "storm the player saved in");
}

/* ── 3. The restored clock KEEPS RUNNING from where it was. ──────────── */
static void test_restored_clock_does_not_snap_back(void)
{
    JceScene *a = make_scene();
    run_seconds(a, 12.0f);                    /* -> 20:00 */
    const float saved = jce_scene_environment_hour(a);
    TEST_ASSERT_TRUE(save_to(a, SAVE_PATH));
    jce_scene_destroy(a);

    JceScene *b = make_scene();
    TEST_ASSERT_TRUE(load_into(b, SAVE_PATH));

    /* One more second of play.  This is the assertion the naive round-trip
     * test misses: if the load did not also mark the day as seeded, the very
     * next advance sees the authored 08:00 differ from day_seed_hour and
     * re-seeds, throwing the restored save back to the start of the level. */
    run_seconds(b, 1.0f);
    const float got = jce_scene_environment_hour(b);
    jce_scene_destroy(b);

    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, saved + 1.0f, got,
        "the clock snapped back toward the authored hour on the first frame "
        "after load -- restoring the hour without marking the day seeded");
}

/* ── 4. A save with no scene_env section still loads. ─────────────────── */
static void test_old_save_without_the_section_still_loads(void)
{
    /* Write a snapshot that has some OTHER section and no "scene_env": exactly
     * the shape of every save file written before this provider existed. */
    JceSnapshotRegistry *reg = jce_snapshot_registry_create();
    TEST_ASSERT_NOT_NULL(reg);
    jce_snapshot_register(reg, "unrelated", 1u, NULL, NULL, NULL);
    const bool wrote = jce_snapshot_save_to_file(reg, SAVE_PATH);
    jce_snapshot_registry_destroy(reg);
    TEST_ASSERT_TRUE_MESSAGE(wrote, "could not write the section-less save");

    JceScene *b = make_scene();
    run_seconds(b, 3.0f);                     /* 08:00 -> 11:00 */
    const bool ok = load_into(b, SAVE_PATH);
    const float got = jce_scene_environment_hour(b);
    jce_scene_destroy(b);

    TEST_ASSERT_TRUE_MESSAGE(ok,
        "a save without the scene_env section was refused -- unknown sections "
        "are supposed to be skipped, which is the whole migration story");
    /* Nothing restored it, so the clock is simply where it was. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.1f, 11.0f, got,
        "loading a save with no environment section disturbed the clock");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_hour_round_trips);
    RUN_TEST(test_wetness_round_trips);
    RUN_TEST(test_restored_clock_does_not_snap_back);
    RUN_TEST(test_old_save_without_the_section_still_loads);
    remove(SAVE_PATH);
    return UNITY_END();
}
