/*
 * test_jce_trail_time.c — JceTrailRendererComponent.time expires points.
 *
 * `time` -- "seconds points persist" -- had nothing to measure against: the
 * component stored positions and no timestamps, so a trail was a fixed
 * 64-point ring that dropped its oldest sample only when FULL.  A trail on a
 * slow object lasted forever; one on a fast object lasted a fraction of a
 * second.  And .autodestruct, which Unity fires once the trail has faded out,
 * could not fire because nothing ever faded.
 *
 * This drives the REAL runtime step over a real scene, so it covers what a
 * unit test of the component cannot: capture -> age -> expire.
 *
 * Both directions are asserted.  time <= 0 is the parser's default and MUST
 * keep the ring behaviour, or every existing trail changes length.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include <stdio.h>
#include <stdbool.h>
#include <string.h>

/* Build a scene with one moving trail emitter and return its point count
 * after `steps` fixed steps of `dt`, having stopped emitting at `stop_at`. */
static int trail_points(float time_s, int steps, int stop_at)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "Emitter");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);

    JceTrailRendererComponent tr;
    memset(&tr, 0, sizeof tr);
    tr.time = time_s;
    tr.min_vertex_distance = 0.05f;   /* well under the per-step motion */
    tr.width_start = tr.width_end = 0.2f;
    tr.emitting = true;
    jce_scene_set_trail_renderer(s, e, &tr);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;
    JceRuntime *rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(rt);

    const float dt = 1.0f / 60.0f;
    for (int i = 0; i < steps; ++i) {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (t) t->position.x += 1.0f;          /* always past min distance */
        if (i == stop_at) {
            JceTrailRendererComponent *c = jce_scene_get_trail_renderer(s, e);
            if (c) c->emitting = false;
        }
        jce_runtime_step(rt, dt);
    }

    JceTrailRendererComponent *out = jce_scene_get_trail_renderer(s, e);
    int n = out ? out->point_count : -1;
    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    return n;
}

static void test_unset_time_keeps_the_ring(void)
{
    /* 120 steps, never stops emitting.  With no expiry the ring fills and
     * holds at its capacity. */
    int n = trail_points(0.0f, 120, 999);
    TEST_ASSERT_EQUAL_INT_MESSAGE(JCE_TRAIL_MAX_POINTS, n,
        "time <= 0 is the parser's default and must keep the 64-point ring -- "
        "otherwise every trail authored before this changes length");
}

static void test_time_bounds_the_trail(void)
{
    /* 0.25 s at 60 Hz is about 15 points, well under the 64 the ring would
     * hold after 120 steps. */
    int n = trail_points(0.25f, 120, 999);
    TEST_ASSERT_TRUE_MESSAGE(n > 0,
        "a trail that is still emitting must keep its recent points");
    TEST_ASSERT_TRUE_MESSAGE(n < JCE_TRAIL_MAX_POINTS,
        "an authored lifetime must bound the trail BELOW the ring capacity -- "
        "if it does not, `time` still expires nothing");
    TEST_ASSERT_TRUE_MESSAGE(n <= 20,
        "0.25 s at 60 Hz is ~15 points; a much longer trail means the ageing "
        "is not keeping up with the authored lifetime");
}

static void test_trail_drains_after_emitting_stops(void)
{
    /* Stop at step 10, then keep stepping well past the 0.25 s lifetime. */
    int n = trail_points(0.25f, 120, 10);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, n,
        "once emitting stops, every point must age out -- this is the state "
        "autodestruct waits for, and nothing ever reached it before");
}

/* .autodestruct: Unity destroys the object once the trail has faded out.  It
 * needs `time` to fade at all, which is why the two fields were dead
 * together -- a trail that never faded could never reach the state this
 * waits for. */
static bool autodestruct_removes_entity(bool autodestruct)
{
    JceScene *s = jce_scene_create();
    JceEntity e = jce_scene_create_entity(s, "Emitter");
    JceTransform tf;
    memset(&tf, 0, sizeof tf);
    tf.rotation.w = 1.0f;
    tf.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &tf);

    JceTrailRendererComponent tr;
    memset(&tr, 0, sizeof tr);
    tr.time = 0.25f;
    tr.min_vertex_distance = 0.05f;
    tr.width_start = tr.width_end = 0.2f;
    tr.emitting = true;
    tr.autodestruct = autodestruct;
    jce_scene_set_trail_renderer(s, e, &tr);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene = s;
    JceRuntime *rt = jce_runtime_create(&desc);

    for (int i = 0; i < 120; ++i) {
        JceTransform *t = jce_scene_get_transform(s, e);
        if (t) t->position.x += 1.0f;
        if (i == 10) {
            JceTrailRendererComponent *c = jce_scene_get_trail_renderer(s, e);
            if (c) c->emitting = false;
        }
        jce_runtime_step(rt, 1.0f / 60.0f);
    }

    bool alive = jce_scene_entity_alive(s, e);
    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    return !alive;
}

static void test_autodestruct_removes_a_faded_trail(void)
{
    TEST_ASSERT_TRUE_MESSAGE(autodestruct_removes_entity(true),
        "autodestruct must destroy the entity once its trail has faded out");
}

static void test_without_autodestruct_the_entity_stays(void)
{
    TEST_ASSERT_FALSE_MESSAGE(autodestruct_removes_entity(false),
        "without autodestruct a faded trail must leave its entity alone -- "
        "otherwise the flag is not what decides, and every trail self-deletes");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_unset_time_keeps_the_ring);
    RUN_TEST(test_time_bounds_the_trail);
    RUN_TEST(test_trail_drains_after_emitting_stops);
    RUN_TEST(test_autodestruct_removes_a_faded_trail);
    RUN_TEST(test_without_autodestruct_the_entity_stays);
    return UNITY_END();
}
