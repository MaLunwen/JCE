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

#include "unity.h"

static int s_a, s_b, s_c, s_x;
static JcePlayerLoopHandle s_hA, s_hX;

void setUp(void)
{
    s_a = s_b = s_c = s_x = 0;
    s_hA.id = 0u;
    s_hX.id = 0u;
}

void tearDown(void)
{
    jce_player_loop_shutdown();   /* clears all phase storage + id counter */
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

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_all_run_once);
    RUN_TEST(test_self_unregister_does_not_skip_successor);
    RUN_TEST(test_register_during_dispatch);
    return UNITY_END();
}
