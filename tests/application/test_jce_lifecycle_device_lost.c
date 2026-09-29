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

void setUp(void) {}
void tearDown(void) {}

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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_device_lost_reaches_a_registered_listener);
    RUN_TEST(test_both_device_events_have_distinct_names);
    RUN_TEST(test_take_device_lost_is_false_without_a_device);
    return UNITY_END();
}
