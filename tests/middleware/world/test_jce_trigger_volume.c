/*
 * test_jce_trigger_volume.c — Unit tests for jce_trigger_volume.h (L3 world).
 *
 * Volumes fire ENTER/STAY/EXIT for tracked observer points.  We use a
 * single observer that we teleport between updates and verify the
 * callback sees the expected event sequence.
 */

#include "unity.h"

#include <jce/middleware/world/jce_trigger_volume.h>
#include <jce/os/core/jce_math.h>

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Capture sink                                                        */
/* ------------------------------------------------------------------ */

typedef struct {
    JceTriggerEventType last;
    uint32_t enters, exits, stays;
    uint64_t last_trig_user;
    uint64_t last_obs_user;
} Capture;

static void on_event(const JceTriggerEvent *ev, void *user)
{
    Capture *c = (Capture *)user;
    c->last = ev->type;
    c->last_trig_user = ev->trigger_user;
    c->last_obs_user  = ev->observer_user;
    switch (ev->type) {
    case JCE_TRIGGER_EVENT_ENTER: ++c->enters; break;
    case JCE_TRIGGER_EVENT_EXIT:  ++c->exits;  break;
    case JCE_TRIGGER_EVENT_STAY:  ++c->stays;  break;
    }
}

/* ------------------------------------------------------------------ */
/* Helpers                                                              */
/* ------------------------------------------------------------------ */

static JceTriggerDesc aabb_desc(jce_vec3 center, jce_vec3 half)
{
    JceTriggerDesc d;
    memset(&d, 0, sizeof(d));
    d.shape        = JCE_TRIGGER_AABB;
    d.center       = center;
    d.half_extents = half;
    return d;
}

static JceTriggerDesc sphere_desc(jce_vec3 center, float r)
{
    JceTriggerDesc d;
    memset(&d, 0, sizeof(d));
    d.shape          = JCE_TRIGGER_SPHERE;
    d.center         = center;
    d.half_extents.x = r;
    return d;
}

/* ------------------------------------------------------------------ */
/* Tests                                                                */
/* ------------------------------------------------------------------ */

static void test_create_destroy(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    TEST_ASSERT_NOT_NULL(w);
    jce_trigger_world_destroy(w);
}

static void test_destroy_null_safe(void)
{
    jce_trigger_world_destroy(NULL);
    TEST_PASS();
}

static void test_enter_fires_when_observer_steps_in(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    Capture cap = {0};
    jce_trigger_world_set_event_fn(w, on_event, &cap);

    JceTriggerDesc d = aabb_desc(jce_v3(0, 0, 0), jce_v3(1, 1, 1));
    JceTriggerHandle  th = jce_trigger_add(w, &d, 42);
    JceObserverHandle oh = jce_observer_add(w, jce_v3(10, 0, 0), 7);
    TEST_ASSERT_TRUE(jce_trigger_valid(th));
    TEST_ASSERT_TRUE(jce_observer_valid(oh));

    jce_trigger_world_update(w);          /* outside → no enter yet */
    TEST_ASSERT_EQUAL_UINT32(0u, cap.enters);

    jce_observer_set_position(w, oh, jce_v3(0, 0, 0));
    jce_trigger_world_update(w);          /* moved in → enter fires */
    TEST_ASSERT_EQUAL_UINT32(1u, cap.enters);
    TEST_ASSERT_EQUAL_UINT64(42ull, cap.last_trig_user);
    TEST_ASSERT_EQUAL_UINT64(7ull,  cap.last_obs_user);
    jce_trigger_world_destroy(w);
}

static void test_exit_fires_when_observer_leaves(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    Capture cap = {0};
    jce_trigger_world_set_event_fn(w, on_event, &cap);

    JceTriggerDesc d = aabb_desc(jce_v3(0, 0, 0), jce_v3(1, 1, 1));
    (void)jce_trigger_add(w, &d, 0);
    JceObserverHandle oh = jce_observer_add(w, jce_v3(0, 0, 0), 0);

    jce_trigger_world_update(w);          /* inside from frame 1 */
    TEST_ASSERT_EQUAL_UINT32(1u, cap.enters);

    jce_observer_set_position(w, oh, jce_v3(100, 0, 0));
    jce_trigger_world_update(w);          /* moved out → exit fires */
    TEST_ASSERT_EQUAL_UINT32(1u, cap.exits);
    jce_trigger_world_destroy(w);
}

static void test_stay_disabled_fires_no_stay_events(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    Capture cap = {0};
    jce_trigger_world_set_event_fn(w, on_event, &cap);
    jce_trigger_world_set_stay_events(w, false);

    JceTriggerDesc d = aabb_desc(jce_v3(0, 0, 0), jce_v3(1, 1, 1));
    (void)jce_trigger_add(w, &d, 0);
    (void)jce_observer_add(w, jce_v3(0, 0, 0), 0);

    jce_trigger_world_update(w);
    jce_trigger_world_update(w);
    jce_trigger_world_update(w);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.stays);
    TEST_ASSERT_EQUAL_UINT32(1u, cap.enters);
    jce_trigger_world_destroy(w);
}

static void test_sphere_uses_radius_in_half_extents_x(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    Capture cap = {0};
    jce_trigger_world_set_event_fn(w, on_event, &cap);

    JceTriggerDesc d = sphere_desc(jce_v3(0, 0, 0), 2.0f);
    (void)jce_trigger_add(w, &d, 0);
    JceObserverHandle oh = jce_observer_add(w, jce_v3(5, 0, 0), 0);

    jce_trigger_world_update(w);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.enters);     /* 5 > radius 2 → out */

    jce_observer_set_position(w, oh, jce_v3(1, 0, 0));
    jce_trigger_world_update(w);
    TEST_ASSERT_EQUAL_UINT32(1u, cap.enters);     /* 1 < radius 2 → in */
    jce_trigger_world_destroy(w);
}

static void test_disabled_trigger_skips_overlap(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    Capture cap = {0};
    jce_trigger_world_set_event_fn(w, on_event, &cap);

    JceTriggerDesc d = aabb_desc(jce_v3(0, 0, 0), jce_v3(1, 1, 1));
    JceTriggerHandle th = jce_trigger_add(w, &d, 0);
    (void)jce_observer_add(w, jce_v3(0, 0, 0), 0);

    jce_trigger_set_enabled(w, th, false);
    jce_trigger_world_update(w);
    TEST_ASSERT_EQUAL_UINT32(0u, cap.enters);

    jce_trigger_set_enabled(w, th, true);
    jce_trigger_world_update(w);
    TEST_ASSERT_EQUAL_UINT32(1u, cap.enters);
    jce_trigger_world_destroy(w);
}

static void test_stats_reports_counts(void)
{
    JceTriggerWorld *w = jce_trigger_world_create();
    Capture cap = {0};
    jce_trigger_world_set_event_fn(w, on_event, &cap);

    JceTriggerDesc d = aabb_desc(jce_v3(0, 0, 0), jce_v3(1, 1, 1));
    (void)jce_trigger_add(w, &d, 0);
    (void)jce_observer_add(w, jce_v3(0, 0, 0), 0);
    jce_trigger_world_update(w);

    JceTriggerStats s = jce_trigger_world_stats(w);
    TEST_ASSERT_EQUAL_UINT32(1u, s.triggers);
    TEST_ASSERT_EQUAL_UINT32(1u, s.observers);
    TEST_ASSERT_EQUAL_UINT32(1u, s.pairs_overlapping);
    TEST_ASSERT_EQUAL_UINT32(1u, s.enter_events_last_update);
    jce_trigger_world_destroy(w);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_create_destroy);
    RUN_TEST(test_destroy_null_safe);
    RUN_TEST(test_enter_fires_when_observer_steps_in);
    RUN_TEST(test_exit_fires_when_observer_leaves);
    RUN_TEST(test_stay_disabled_fires_no_stay_events);
    RUN_TEST(test_sphere_uses_radius_in_half_extents_x);
    RUN_TEST(test_disabled_trigger_skips_overlap);
    RUN_TEST(test_stats_reports_counts);
    return UNITY_END();
}
