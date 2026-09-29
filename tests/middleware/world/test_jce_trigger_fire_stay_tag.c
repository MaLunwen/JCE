/*
 * test_jce_trigger_fire_stay_tag.c — TriggerVolume.fire_stay and .tag.
 *
 * Both were authored, serialised, shown in the Inspector and read by nothing.
 * fire_stay was DOUBLY dead: JceTriggerDesc had no such field, so the trigger
 * world fired its STAY cadence for every trigger alike -- and the runtime
 * dropped the events anyway ("STAY is intentionally not dispatched"), so
 * honouring the flag alone would still have changed nothing observable.
 * `tag` is described by the component as a "user label propagated to event
 * payload" and JceTriggerEvent had no payload field to propagate it to, so a
 * script could not tell one zone from another except by entity id.
 *
 * Asserted here, at the trigger-world layer:
 *   1. a trigger that did NOT ask for STAY gets ENTER and EXIT and no STAY --
 *      the opt-in default, and what every existing zone will now see;
 *   2. one that DID ask gets STAY while the observer stays inside;
 *   3. the authored tag arrives on the event, on ENTER as well as STAY.
 *
 * Both directions of (1)/(2) are asserted, so a build that fired STAY for
 * everything fails as loudly as one that fired it for nothing.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/world/jce_trigger_volume.h>

#include <stdbool.h>
#include <stdio.h>
#include <string.h>

typedef struct {
    int  enter, stay, exit_;
    char last_tag[64];
} Counts;

static Counts g_c;

static void on_event(const JceTriggerEvent *ev, void *user)
{
    (void)user;
    if (!ev) return;
    if (ev->type == JCE_TRIGGER_EVENT_ENTER) g_c.enter++;
    else if (ev->type == JCE_TRIGGER_EVENT_STAY) g_c.stay++;
    else if (ev->type == JCE_TRIGGER_EVENT_EXIT) g_c.exit_++;
    snprintf(g_c.last_tag, sizeof g_c.last_tag, "%s", ev->tag ? ev->tag : "");
}

/* One AABB at the origin, one observer walked in, held, then walked out. */
static void run(bool fire_stay, const char *tag)
{
    memset(&g_c, 0, sizeof g_c);

    JceTriggerWorld *w = jce_trigger_world_create();
    TEST_ASSERT_NOT_NULL(w);
    jce_trigger_world_set_event_fn(w, on_event, NULL);

    JceTriggerDesc d;
    memset(&d, 0, sizeof d);
    d.shape = JCE_TRIGGER_AABB;
    d.center = jce_v3(0.0f, 0.0f, 0.0f);
    d.half_extents = jce_v3(1.0f, 1.0f, 1.0f);
    d.fire_stay = fire_stay;
    snprintf(d.tag, sizeof d.tag, "%s", tag);
    JceTriggerHandle th = jce_trigger_add(w, &d, 1u);
    TEST_ASSERT_TRUE(jce_trigger_valid(th));
    jce_trigger_set_enabled(w, th, true);

    JceObserverHandle oh = jce_observer_add(w, jce_v3(5.0f, 0.0f, 0.0f), 2u);
    TEST_ASSERT_TRUE(jce_observer_valid(oh));

    /* Outside, then 30 ticks inside, then outside again. */
    jce_trigger_world_update(w);
    jce_observer_set_position(w, oh, jce_v3(0.0f, 0.0f, 0.0f));
    for (int i = 0; i < 30; ++i)
        jce_trigger_world_update(w);
    jce_observer_set_position(w, oh, jce_v3(5.0f, 0.0f, 0.0f));
    jce_trigger_world_update(w);

    jce_trigger_world_destroy(w);
}

static void test_stay_is_opt_in(void)
{
    run(false, "");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.enter, "ENTER must still fire");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, g_c.exit_, "EXIT must still fire");
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_c.stay,
        "a trigger that did not ask for STAY must get none -- the world used "
        "to fire its cadence for every trigger alike");
}

static void test_stay_fires_when_asked(void)
{
    run(true, "");
    TEST_ASSERT_EQUAL_INT(1, g_c.enter);
    TEST_ASSERT_EQUAL_INT(1, g_c.exit_);
    TEST_ASSERT_TRUE_MESSAGE(g_c.stay > 0,
        "a trigger WITH fire_stay must get STAY while the observer is inside "
        "-- if this fails, fire_stay never reached the world");
}

static void test_tag_reaches_the_event(void)
{
    run(true, "checkpoint_03");
    TEST_ASSERT_TRUE(g_c.stay > 0);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("checkpoint_03", g_c.last_tag,
        "the authored tag must arrive on the event -- without it a script can "
        "only tell zones apart by entity id");
}

static void test_no_tag_is_empty_not_garbage(void)
{
    run(false, "");
    TEST_ASSERT_EQUAL_STRING("", g_c.last_tag);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_stay_is_opt_in);
    RUN_TEST(test_stay_fires_when_asked);
    RUN_TEST(test_tag_reaches_the_event);
    RUN_TEST(test_no_tag_is_empty_not_garbage);
    return UNITY_END();
}
