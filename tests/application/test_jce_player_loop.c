/* test_jce_player_loop.c
 *
 * Proves jce_player_loop_run_phase dispatches each registered callback
 * EXACTLY ONCE even when a callback registers or unregisters same-phase
 * callbacks mid-dispatch (audit Round-3 rework of F81).
 *
 * The original positional loop iterated by index over a snapshot count; a
 * register/unregister memmoved the dense array, so it could skip a shifted-in
 * sibling or re-run a shifted-up slot.  The fix dispatches by IDENTITY (id
 * snapshot taken at entry), which these tests lock in.
 */

#include <jce/runtime/jce_player_loop.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_allocator.h>
#include <string.h>

#include "unity.h"

static int s_a, s_b, s_c, s_x;
static JcePlayerLoopHandle s_hA, s_hX;

void setUp(void)
{
    jce_trace_set_enabled(false);
    jce_trace_reset();
    s_a = s_b = s_c = s_x = 0;
    s_hA.id = 0u;
    s_hX.id = 0u;
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
    jce_player_loop_shutdown();   /* clears all phase storage; IDs stay monotonic */
}

static void cb_a(float dt, void *u) { (void)dt; (void)u; ++s_a; }
static void cb_b(float dt, void *u) { (void)dt; (void)u; ++s_b; }
static void cb_c(float dt, void *u) { (void)dt; (void)u; ++s_c; }
static void cb_x(float dt, void *u) { (void)dt; (void)u; ++s_x; }

/* Baseline: three callbacks, no mutation → each runs exactly once. */
static void test_all_run_once(void)
{
    jce_player_loop_register(JCE_PHASE_UPDATE, 1, cb_a, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 2, cb_b, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 3, cb_c, NULL);
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_a);
    TEST_ASSERT_EQUAL_INT(1, s_b);
    TEST_ASSERT_EQUAL_INT(1, s_c);
}

/* A callback that unregisters ITSELF must not cause its successor to be
 * skipped.  Old bug: removal memmoved B into A's just-consumed slot, then i++
 * stepped past B → B skipped. */
static void cb_a_unreg_self(float dt, void *u)
{
    (void)dt; (void)u;
    ++s_a;
    jce_player_loop_unregister(s_hA);
}
static void test_self_unregister_does_not_skip_successor(void)
{
    s_hA = jce_player_loop_register(JCE_PHASE_UPDATE, 1, cb_a_unreg_self, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 2, cb_b, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 3, cb_c, NULL);
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_a);
    TEST_ASSERT_EQUAL_INT(1, s_b);   /* must NOT be skipped */
    TEST_ASSERT_EQUAL_INT(1, s_c);
    /* A really is gone now. */
    TEST_ASSERT_EQUAL_UINT32(2u, jce_player_loop_phase_count(JCE_PHASE_UPDATE));
}

/* A callback that registers a LOWER-priority same-phase callback mid-dispatch
 * must (a) not re-run the already-executed callback (old bug: the insert
 * shifted A up, the index-bounded loop re-read and re-ran it) and (b) the new
 * callback must not fire until the next frame (documented contract). */
static void cb_a_register_lower(float dt, void *u)
{
    (void)dt; (void)u;
    ++s_a;
    if (!s_hX.id)
        s_hX = jce_player_loop_register(JCE_PHASE_UPDATE, -100, cb_x, NULL);
}
static void test_register_during_dispatch(void)
{
    jce_player_loop_register(JCE_PHASE_UPDATE, 5, cb_a_register_lower, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 10, cb_b, NULL);

    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_a);   /* exactly once, not double-run */
    TEST_ASSERT_EQUAL_INT(1, s_b);   /* not skipped */
    TEST_ASSERT_EQUAL_INT(0, s_x);   /* registered mid-dispatch → next frame */

    /* Next frame: X (now present in the snapshot) runs exactly once. */
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_x);
    TEST_ASSERT_EQUAL_INT(2, s_a);   /* A ran again, still once per frame */
}

static void cb_trace_mutates_storage(float dt, void *user)
{
    (void)dt; (void)user;
    ++s_a;
    jce_player_loop_unregister(s_hA);
    for (int i = 0; i < 80; ++i) {
        JcePlayerLoopHandle h = jce_player_loop_register(JCE_PHASE_UPDATE, i, cb_x, NULL);
        jce_player_loop_set_debug_name(h.id, "new-slot");
    }
}

static void test_trace_names_survive_callback_reallocation_and_preserve_parent(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[9];
    JceTraceStats stats;
    s_hA = jce_player_loop_register(JCE_PHASE_UPDATE, 0, cb_trace_mutates_storage, NULL);
    jce_player_loop_set_debug_name(s_hA.id, "mutating-original");
    JcePlayerLoopHandle next = jce_player_loop_register(JCE_PHASE_UPDATE, 1, cb_b, NULL);
    jce_player_loop_set_debug_name(next.id, "surviving-original");
    jce_trace_set_enabled(true);
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_a);
    TEST_ASSERT_EQUAL_INT(1, s_b);
    TEST_ASSERT_EQUAL_INT(0, s_x);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(9, jce_trace_read(&cursor, events, 9));
    TEST_ASSERT_EQUAL_STRING("update", events[0].name);
    TEST_ASSERT_EQUAL_STRING("mutating-original", events[2].name);
    TEST_ASSERT_EQUAL_STRING("mutating-original", events[4].name);
    TEST_ASSERT_EQUAL_STRING("surviving-original", events[5].name);
    TEST_ASSERT_EQUAL_STRING("surviving-original", events[7].name);
    TEST_ASSERT_EQUAL_UINT64(events[0].id, events[2].parent_id);
    TEST_ASSERT_EQUAL_UINT64(events[0].id, events[5].parent_id);
    TEST_ASSERT_EQUAL_UINT64(events[1].id, events[8].id);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(3, stats.work_items_completed);
}

static void test_trace_records_empty_phase_dispatch_only_when_enabled(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[3];
    jce_trace_set_enabled(true);
    jce_player_loop_run_phase(JCE_PHASE_INITIALIZATION, 0.0f);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(3, jce_trace_read(&cursor, events, 3));
    TEST_ASSERT_EQUAL_STRING("initialization", events[0].name);
    jce_trace_set_enabled(false);
    jce_player_loop_run_phase(JCE_PHASE_END_OF_FRAME, 0.0f);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, events, 3));
}

static void test_large_phase_dispatch_allocates_no_snapshot(void)
{
    uint64_t allocations = 0, bytes = 0;
    for (int i = 0; i < 80; ++i)
        jce_player_loop_register(JCE_PHASE_UPDATE, i, cb_b, NULL);
    jce_alloc_frame_delta(&allocations, &bytes);
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    jce_alloc_frame_delta(&allocations, &bytes);
    TEST_ASSERT_EQUAL_INT(80, s_b);
    TEST_ASSERT_EQUAL_UINT64(0, allocations);
    TEST_ASSERT_EQUAL_UINT64(0, bytes);
}

static void cb_shutdown_and_register(float dt, void *user)
{
    (void)dt; (void)user;
    ++s_a;
    jce_player_loop_shutdown();
    jce_player_loop_register(JCE_PHASE_UPDATE, 0, cb_x, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 1, cb_x, NULL);
}
static void test_shutdown_cancels_active_identity_snapshot(void)
{
    jce_player_loop_register(JCE_PHASE_UPDATE, 0, cb_shutdown_and_register, NULL);
    jce_player_loop_register(JCE_PHASE_UPDATE, 1, cb_b, NULL);
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_a);
    TEST_ASSERT_EQUAL_INT(0, s_b);
    TEST_ASSERT_EQUAL_INT(0, s_x);
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(2, s_x);
}
static void test_stale_handle_cannot_remove_post_shutdown_callback(void)
{
    JcePlayerLoopHandle old = jce_player_loop_register(JCE_PHASE_UPDATE, 0, cb_a, NULL);
    jce_player_loop_shutdown();
    JcePlayerLoopHandle current = jce_player_loop_register(JCE_PHASE_UPDATE, 0, cb_b, NULL);
    TEST_ASSERT_NOT_EQUAL(old.id, current.id);
    jce_player_loop_unregister(old);
    TEST_ASSERT_EQUAL_UINT32(1, jce_player_loop_phase_count(JCE_PHASE_UPDATE));
    jce_player_loop_run_phase(JCE_PHASE_UPDATE, 0.016f);
    TEST_ASSERT_EQUAL_INT(1, s_b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_run_once);
    RUN_TEST(test_large_phase_dispatch_allocates_no_snapshot);
    RUN_TEST(test_shutdown_cancels_active_identity_snapshot);
    RUN_TEST(test_stale_handle_cannot_remove_post_shutdown_callback);
    RUN_TEST(test_trace_names_survive_callback_reallocation_and_preserve_parent);
    RUN_TEST(test_trace_records_empty_phase_dispatch_only_when_enabled);
    RUN_TEST(test_self_unregister_does_not_skip_successor);
    RUN_TEST(test_register_during_dispatch);
    return UNITY_END();
}
