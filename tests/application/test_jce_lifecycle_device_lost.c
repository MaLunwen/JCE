/* JCE_LIFECYCLE_DEVICE_LOST used to be an event nothing could deliver.
 *
 * The whole tracked tree named it four times -- the enum, its case in
 * jce_lifecycle_event_to_string(), its row in contracts/abi-snapshot.txt, and a
 * platform AGENTS.md table saying "(no SDL counterpart yet); renderer may
 * emit manually".  A shipped game's
 *
 *     jce_lifecycle_register(JCE_LIFECYCLE_DEVICE_LOST, on_lost, ctx)
 *
 * returned a valid handle and `on_lost` was dead code.  Nothing in the
 * engine, the editor or the log said otherwise: the authorisation was real
 * and the event never came.
 *
 * WHAT THIS FILE CAN AND CANNOT PIN, stated rather than implied.
 *
 * The producer is bgfx's fatal callback on BGFX_FATAL_DEVICE_LOST, which a
 * unit test cannot raise without a GPU and a genuinely lost device.  So the
 * source-level half -- "somebody emits it at all" -- is a lint
 * (tools/lint/check_lifecycle_events_emitted.py), and this file pins the
 * DELIVERY half: that the registry actually routes this event to a listener,
 * with the right value, and stops when unregistered.
 *
 * Together they cover the defect from both ends.  Neither alone would have:
 * the lint cannot see reachability, and this test would have passed on the
 * broken tree, because the registry was never what was broken.
 */

#include <jce/application/jce_lifecycle.h>
#include <jce/renderer/jce_renderer.h>

#include "unity.h"

#include <string.h>

void setUp(void) { jce_lifecycle_shutdown(); }
void tearDown(void) { jce_lifecycle_shutdown(); }

typedef struct Seen {
    int                count;
    JceLifecycleEvent  last;
} Seen;

static void on_event(JceLifecycleEvent e, void *user)
{
    Seen *s = (Seen *)user;
    s->count++;
    s->last = e;
}

/* THE DELIVERY PATH, which is what a consumer's handler depends on. */
static void test_device_lost_reaches_a_registered_listener(void)
{
    Seen seen = { 0, JCE_LIFECYCLE_EVENT_COUNT };
    JceLifecycleHandle h = jce_lifecycle_register(on_event, 0, &seen);
    TEST_ASSERT_NOT_EQUAL_UINT32_MESSAGE(0u, h.id,
        "registering for a lifecycle event should return a live handle");

    jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_LOST);

    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen.count,
        "the listener should have been called exactly once");
    /* THE VALUE, not just the count.  An emit that delivered the wrong
     * enumerator would satisfy a count-only assertion perfectly, and the two
     * device events sit next to each other in the enum -- which is exactly
     * the pair most likely to be confused. */
    TEST_ASSERT_EQUAL_INT_MESSAGE((int)JCE_LIFECYCLE_DEVICE_LOST, (int)seen.last,
        "the listener should have been told DEVICE_LOST, not its neighbour");

    jce_lifecycle_unregister(h);
    jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_LOST);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, seen.count,
        "an unregistered listener should stop hearing the event");
}

/* The name table is what a log line shows a human, and it is the other place
 * an event can exist while saying nothing useful. */
static void test_both_device_events_have_distinct_names(void)
{
    const char *lost  = jce_lifecycle_event_to_string(JCE_LIFECYCLE_DEVICE_LOST);
    const char *reset = jce_lifecycle_event_to_string(JCE_LIFECYCLE_DEVICE_RESET);
    TEST_ASSERT_NOT_NULL(lost);
    TEST_ASSERT_NOT_NULL(reset);
    TEST_ASSERT_EQUAL_STRING("DEVICE_LOST", lost);
    TEST_ASSERT_EQUAL_STRING("DEVICE_RESET", reset);
}

/* The renderer-side half of the seam, as far as it can be reached without a
 * GPU: the accessor exists, is callable from the application layer WITHOUT
 * pulling in bgfx, and reports no loss on a tree where none happened.
 *
 * FALSE IS THE ONLY HONEST EXPECTATION HERE and it is worth saying why this
 * assertion is weak on purpose: nothing in this process ever initialised
 * bgfx, so a `true` would mean the flag defaulted wrong -- which is the one
 * failure mode this can actually see.  It cannot show that a real device loss
 * sets it; only a lost device can. */
static void test_take_device_lost_is_false_without_a_device(void)
{
    TEST_ASSERT_FALSE_MESSAGE(jce_renderer_take_device_lost(),
        "no bgfx device was ever created in this process, so nothing should "
        "have reported losing one");
    TEST_ASSERT_FALSE_MESSAGE(jce_renderer_take_device_lost(),
        "and it should still be false on a second call");
}

typedef struct MutationWitness {
    int a, b, c, added;
    JceLifecycleHandle self, other, inserted;
} MutationWitness;

static void count_b(JceLifecycleEvent e, void *user)
{
    (void)e;
    ++((MutationWitness *)user)->b;
}
static void count_c(JceLifecycleEvent e, void *user)
{
    (void)e;
    ++((MutationWitness *)user)->c;
}
static void count_added(JceLifecycleEvent e, void *user)
{
    (void)e;
    ++((MutationWitness *)user)->added;
}
static void unregister_self(JceLifecycleEvent e, void *user)
{
    (void)e;
    MutationWitness *w = user;
    ++w->a;
    jce_lifecycle_unregister(w->self);
}
static void unregister_other(JceLifecycleEvent e, void *user)
{
    (void)e;
    MutationWitness *w = user;
    ++w->a;
    jce_lifecycle_unregister(w->other);
}
static void insert_before(JceLifecycleEvent e, void *user)
{
    (void)e;
    MutationWitness *w = user;
    ++w->a;
    if (!w->inserted.id)
        w->inserted = jce_lifecycle_register(count_added, -1, w);
}
static void insert_between(JceLifecycleEvent e, void *user)
{
    (void)e;
    MutationWitness *w = user;
    ++w->a;
    if (!w->inserted.id)
        w->inserted = jce_lifecycle_register(count_added, 1, w);
}
static void insert_after(JceLifecycleEvent e, void *user)
{
    (void)e;
    MutationWitness *w = user;
    ++w->a;
    if (!w->inserted.id)
        w->inserted = jce_lifecycle_register(count_added, 3, w);
}
static void test_insert_after_waits_for_next_emit(void)
{
    MutationWitness w = {0};
    jce_lifecycle_register(insert_after, 0, &w);
    jce_lifecycle_register(count_b, 2, &w);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, w.a);
    TEST_ASSERT_EQUAL_INT(1, w.b);
    TEST_ASSERT_EQUAL_INT(0, w.added);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(2, w.a);
    TEST_ASSERT_EQUAL_INT(2, w.b);
    TEST_ASSERT_EQUAL_INT(1, w.added);
}
static void test_shutdown_resets_paired_state_before_any_allocation(void)
{
    jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST);
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    jce_lifecycle_shutdown();
    TEST_ASSERT_TRUE(jce_lifecycle_is_focused());
    TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
}
static void test_self_removal_preserves_each_successor_once(void)
{
    MutationWitness w = {0};
    w.self = jce_lifecycle_register(unregister_self, 0, &w);
    jce_lifecycle_register(count_b, 1, &w);
    jce_lifecycle_register(count_c, 2, &w);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, w.a);
    TEST_ASSERT_EQUAL_INT(1, w.b);
    TEST_ASSERT_EQUAL_INT(1, w.c);
}
static void test_removing_later_listener_never_calls_its_user(void)
{
    MutationWitness w = {0};
    jce_lifecycle_register(unregister_other, 0, &w);
    jce_lifecycle_register(count_b, 1, &w);
    w.other = jce_lifecycle_register(count_c, 2, &w);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, w.a);
    TEST_ASSERT_EQUAL_INT(1, w.b);
    TEST_ASSERT_EQUAL_INT(0, w.c);
}
static void test_insertions_do_not_run_new_or_repeat_existing_listener(void)
{
    MutationWitness before = {0};
    jce_lifecycle_register(insert_before, 0, &before);
    jce_lifecycle_register(count_b, 2, &before);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, before.a);
    TEST_ASSERT_EQUAL_INT(1, before.b);
    TEST_ASSERT_EQUAL_INT(0, before.added);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(2, before.a);
    TEST_ASSERT_EQUAL_INT(2, before.b);
    TEST_ASSERT_EQUAL_INT(1, before.added);
    jce_lifecycle_shutdown();
    MutationWitness between = {0};
    jce_lifecycle_register(insert_between, 0, &between);
    jce_lifecycle_register(count_b, 2, &between);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, between.a);
    TEST_ASSERT_EQUAL_INT(1, between.b);
    TEST_ASSERT_EQUAL_INT(0, between.added);
}

static void shutdown_and_replace(JceLifecycleEvent e, void *user)
{
    (void)e;
    MutationWitness *w = user;
    ++w->a;
    jce_lifecycle_shutdown();
    w->inserted = jce_lifecycle_register(count_added, 0, w);
    jce_lifecycle_register(count_c, 1, w);
}
static void test_shutdown_cancels_dispatch_and_stale_handles(void)
{
    MutationWitness w = {0};
    w.self = jce_lifecycle_register(shutdown_and_replace, 0, &w);
    w.other = jce_lifecycle_register(count_b, 1, &w);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, w.a);
    TEST_ASSERT_EQUAL_INT(0, w.b);
    TEST_ASSERT_EQUAL_INT(0, w.c);
    TEST_ASSERT_EQUAL_INT(0, w.added);
    TEST_ASSERT_NOT_EQUAL_UINT32(w.self.id, w.inserted.id);
    jce_lifecycle_unregister(w.self);
    jce_lifecycle_unregister(w.other);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    TEST_ASSERT_EQUAL_INT(1, w.c);
    TEST_ASSERT_EQUAL_INT(1, w.added);
}

typedef struct NestedWitness {
    int outer_a, inner_a, outer_b, inner_b, added;
    bool entered;
} NestedWitness;
static void nested_added(JceLifecycleEvent e, void *user)
{
    TEST_ASSERT_EQUAL_INT(JCE_LIFECYCLE_RESUME, e);
    ++((NestedWitness *)user)->added;
}
static void nested_a(JceLifecycleEvent e, void *user)
{
    NestedWitness *w = user;
    if (e == JCE_LIFECYCLE_PAUSE) {
        ++w->outer_a;
        TEST_ASSERT_TRUE(jce_lifecycle_is_paused());
        if (!w->entered) {
            w->entered = true;
            jce_lifecycle_register(nested_added, 2, w);
            jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
        }
    } else {
        ++w->inner_a;
        TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
    }
}
static void nested_b(JceLifecycleEvent e, void *user)
{
    NestedWitness *w = user;
    if (e == JCE_LIFECYCLE_PAUSE) ++w->outer_b;
    else ++w->inner_b;
    TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
}
static void test_nested_emit_has_independent_snapshot_and_latest_state(void)
{
    NestedWitness w = {0};
    jce_lifecycle_register(nested_a, 0, &w);
    jce_lifecycle_register(nested_b, 1, &w);
    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    TEST_ASSERT_EQUAL_INT(1, w.outer_a);
    TEST_ASSERT_EQUAL_INT(1, w.inner_a);
    TEST_ASSERT_EQUAL_INT(1, w.outer_b);
    TEST_ASSERT_EQUAL_INT(1, w.inner_b);
    TEST_ASSERT_EQUAL_INT(1, w.added);
    TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
}

static void count_one(JceLifecycleEvent e, void *user)
{
    (void)e;
    ++*(int *)user;
}
static void test_more_than_stack_capacity_delivers_every_listener(void)
{
    int seen[80] = {0};
    for (int i = 0; i < 80; ++i) {
        JceLifecycleHandle h = jce_lifecycle_register(count_one, i, &seen[i]);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, h.id);
    }
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    for (int i = 0; i < 80; ++i) TEST_ASSERT_EQUAL_INT(1, seen[i]);
}

typedef struct OrderedWitness {
    int values[4];
    int count;
} OrderedWitness;
typedef struct OrderedListener {
    OrderedWitness *witness;
    int value;
} OrderedListener;
static void ordered_callback(JceLifecycleEvent e, void *user)
{
    (void)e;
    OrderedListener *listener = user;
    listener->witness->values[listener->witness->count++] = listener->value;
}
static void test_stable_priority_and_duplicate_callbacks_keep_distinct_identity(void)
{
    OrderedWitness w = {0};
    OrderedListener listeners[4] = {{&w, 1}, {&w, 2}, {&w, 3}, {&w, 4}};
    JceLifecycleHandle first = jce_lifecycle_register(ordered_callback, 5, &listeners[0]);
    JceLifecycleHandle duplicate = jce_lifecycle_register(ordered_callback, 5, &listeners[0]);
    jce_lifecycle_register(ordered_callback, -1, &listeners[1]);
    jce_lifecycle_register(ordered_callback, 5, &listeners[2]);
    TEST_ASSERT_NOT_EQUAL_UINT32(first.id, duplicate.id);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    const int expected[4] = {2, 1, 1, 3};
    TEST_ASSERT_EQUAL_INT_ARRAY(expected, w.values, 4);
    jce_lifecycle_unregister(first);
    w.count = 0;
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    const int remaining[3] = {2, 1, 3};
    TEST_ASSERT_EQUAL_INT_ARRAY(remaining, w.values, 3);
    TEST_ASSERT_EQUAL_INT(3, w.count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_shutdown_resets_paired_state_before_any_allocation);
    RUN_TEST(test_device_lost_reaches_a_registered_listener);
    RUN_TEST(test_both_device_events_have_distinct_names);
    RUN_TEST(test_take_device_lost_is_false_without_a_device);
    RUN_TEST(test_self_removal_preserves_each_successor_once);
    RUN_TEST(test_removing_later_listener_never_calls_its_user);
    RUN_TEST(test_insertions_do_not_run_new_or_repeat_existing_listener);
    RUN_TEST(test_shutdown_cancels_dispatch_and_stale_handles);
    RUN_TEST(test_nested_emit_has_independent_snapshot_and_latest_state);
    RUN_TEST(test_more_than_stack_capacity_delivers_every_listener);
    RUN_TEST(test_insert_after_waits_for_next_emit);
    RUN_TEST(test_stable_priority_and_duplicate_callbacks_keep_distinct_identity);
    return UNITY_END();
}
