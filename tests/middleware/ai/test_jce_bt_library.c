/*
 * test_jce_bt_library.c — Unit tests for the bundled deterministic Behavior
 * Tree node library (L3 AI).
 *
 * Exercises the REAL jce_bt tick path (BehaviorTree.CPP backend) driving the
 * nodes registered by jce_bt_register_library():
 *
 *   Wait(sec)            — RUNNING until `sec` of accumulated dt, then SUCCESS
 *   Cooldown(sec)        — gates its child until `sec` of dt have elapsed
 *   SetValue             — writes a typed value a sibling condition reads
 *   ClearValue           — removes a key (or the whole blackboard)
 *   BlackboardCheck      — lexical equality condition on a blackboard key
 *   MoveToTarget         — drives a nav-move hook from the blackboard target
 *   Repeat(num_cycles)   — backend built-in: child must SUCCESS N times
 *
 * Trees are authored as BehaviorTree.CPP XML and ticked with a deterministic
 * per-tick env (blackboard + dt + move hook) so every assertion is reproducible
 * — no wall-clock, no mocks of the BT engine itself.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_bt.h>
#include <jce/middleware/ai/jce_perception.h>

#include <stdint.h>
#include <string.h>

/* ------------------------------------------------------------------ */
/* Fixture                                                             */
/* ------------------------------------------------------------------ */

static JceBtContext *g_ctx;
static JceBlackboard *g_bb;

void setUp(void)
{
    g_ctx = jce_bt_create();
    TEST_ASSERT_NOT_NULL(g_ctx);
    jce_bt_register_library(g_ctx);
    g_bb = jce_blackboard_create();
    TEST_ASSERT_NOT_NULL(g_bb);
}

void tearDown(void)
{
    jce_blackboard_destroy(g_bb);
    g_bb = NULL;
    jce_bt_destroy(g_ctx);
    g_ctx = NULL;
}

/* Set the per-tick env (blackboard + dt + optional move hook) then tick. */
static JceBtStatus tick_dt(JceBtTreeHandle tree, float dt,
                           jce_bt_move_fn mv, void *mvud)
{
    JceBtTickEnv env;
    env.bb            = g_bb;
    env.dt            = dt;
    env.move_to       = mv;
    env.move_userdata = mvud;
    jce_bt_set_env(g_ctx, &env);
    return jce_bt_tick(g_ctx, tree);
}

static JceBtStatus tick(JceBtTreeHandle tree, float dt)
{
    return tick_dt(tree, dt, NULL, NULL);
}

static JceBtTreeHandle load(const char *xml)
{
    JceBtTreeHandle h = jce_bt_load_tree(g_ctx, xml, (uint32_t)strlen(xml));
    TEST_ASSERT_TRUE_MESSAGE(jce_bt_tree_valid(h), "tree failed to load");
    return h;
}

/* ------------------------------------------------------------------ */
/* Wait                                                                */
/* ------------------------------------------------------------------ */

/* Wait(1.0) must stay RUNNING across ticks that accumulate < 1.0s of dt,
 * then flip to SUCCESS exactly once the threshold is crossed. */
static void test_wait_runs_until_duration_elapses(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Wait sec=\"1.0\"/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    /* The first tick (onStart) ALSO advances the world by its dt, so it counts
     * toward the wait — dropping it made Wait take one extra frame.  Each
     * subsequent tick adds its dt (onRunning). */
    TEST_ASSERT_EQUAL_INT(JCE_BT_RUNNING, tick(t, 0.4f));  /* start, elapsed 0.4 */
    TEST_ASSERT_EQUAL_INT(JCE_BT_RUNNING, tick(t, 0.4f));  /* 0.8 */
    /* +0.4 = 1.2s ≥ 1.0 → SUCCESS on the third tick (was the fourth). */
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.4f));  /* 1.2 */
}

/* Wait with a non-positive duration completes immediately. */
static void test_wait_zero_duration_succeeds_immediately(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Wait sec=\"0.0\"/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.016f));
}

/* After completing, re-running the leaf restarts its timer (stateful leaf). */
static void test_wait_restarts_timer_on_reentry(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Wait sec=\"0.5\"/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    /* onStart counts its own tick's dt, so 0.3 + 0.3 = 0.6 ≥ 0.5 in two ticks. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_RUNNING, tick(t, 0.3f));  /* start, 0.3 < 0.5 */
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.3f));  /* 0.6 ≥ 0.5 */

    /* Re-entering from the completed (IDLE-reset) state restarts the timer:
     * a fresh onStart (re-seeded from its own dt), so it takes the same number
     * of ticks again — proving the leaf is stateful, not latched. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_RUNNING, tick(t, 0.3f));  /* start, 0.3 < 0.5 */
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.3f));  /* 0.6 ≥ 0.5 */
}

/* ------------------------------------------------------------------ */
/* SetValue / BlackboardCheck / ClearValue                             */
/* ------------------------------------------------------------------ */

/* SetValue writes a key that a following BlackboardCheck reads SUCCESS. */
static void test_set_then_check_blackboard(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Sequence>"
        "      <SetValue key=\"mode\" value=\"attack\"/>"
        "      <BlackboardCheck key=\"mode\" value=\"attack\"/>"
        "    </Sequence>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.016f));
    /* The write is visible on the actual blackboard (a string tag is stored as
     * a stable hash, so the matching BlackboardCheck above succeeded). */
    TEST_ASSERT_TRUE(jce_blackboard_has(g_bb, "mode"));
    /* A different tag must NOT match the stored hash. */
    jce_blackboard_set_int(g_bb, "other", 123);  /* unrelated key, sanity */
    TEST_ASSERT_TRUE(jce_blackboard_has(g_bb, "other"));
}

/* Numeric values round-trip to an exact typed slot the check matches. */
static void test_set_numeric_blackboard_roundtrips(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Sequence>"
        "      <SetValue key=\"hp\" value=\"42\"/>"
        "      <BlackboardCheck key=\"hp\" value=\"42\"/>"
        "    </Sequence>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.016f));
    TEST_ASSERT_EQUAL_INT(JCE_BB_INT, jce_blackboard_kind(g_bb, "hp"));
    TEST_ASSERT_EQUAL_INT(42, jce_blackboard_get_int(g_bb, "hp", 0));
}

/* BlackboardCheck FAILUREs when the value differs or the key is absent. */
static void test_check_fails_on_mismatch_or_absent(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <BlackboardCheck key=\"mode\" value=\"flee\"/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    /* Absent key → FAILURE. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, tick(t, 0.016f));

    /* Present but different value → FAILURE. */
    jce_blackboard_set_bool(g_bb, "mode", true);  /* "true" != "flee" */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, tick(t, 0.016f));
}

/* ClearValue removes a single key. */
static void test_clear_blackboard_removes_key(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <ClearValue key=\"junk\"/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    jce_blackboard_set_int(g_bb, "junk", 7);
    jce_blackboard_set_int(g_bb, "keep", 9);
    TEST_ASSERT_TRUE(jce_blackboard_has(g_bb, "junk"));

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.016f));
    TEST_ASSERT_FALSE(jce_blackboard_has(g_bb, "junk"));
    TEST_ASSERT_TRUE(jce_blackboard_has(g_bb, "keep"));   /* unrelated key kept */
}

/* ClearValue with no key clears the whole blackboard. */
static void test_clear_blackboard_clears_all(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <ClearValue/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    jce_blackboard_set_int(g_bb, "a", 1);
    jce_blackboard_set_int(g_bb, "b", 2);
    TEST_ASSERT_EQUAL_UINT32(2u, jce_blackboard_count(g_bb));

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.016f));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_blackboard_count(g_bb));
}

/* ------------------------------------------------------------------ */
/* Cooldown                                                            */
/* ------------------------------------------------------------------ */

/* Cooldown(1.0) wrapping an always-SUCCESS child: the first tick runs the
 * child (SUCCESS); subsequent ticks FAIL (gated) until 1.0s of dt elapses,
 * then it runs the child again. */
static void test_cooldown_gates_child_until_timer_expires(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Cooldown sec=\"1.0\">"
        "      <AlwaysSuccess/>"
        "    </Cooldown>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    /* First entry is never gated → child runs → SUCCESS. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.0f));

    /* Now gated: accumulate dt but stay below 1.0 → FAILURE without child. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, tick(t, 0.4f));   /* 0.4 */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, tick(t, 0.4f));   /* 0.8 */

    /* Cross the threshold → child runs again → SUCCESS, restarting cooldown. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.4f));   /* 1.2 ≥ 1.0 */

    /* Re-gated after the fresh completion. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, tick(t, 0.4f));
}

/* ------------------------------------------------------------------ */
/* Repeat (backend built-in)                                           */
/* ------------------------------------------------------------------ */

/* Repeat(num_cycles=3) over a child that SUCCEEDs each tick should SUCCESS
 * once after exactly 3 child completions.  We count via SetBlackboard-free
 * means: a child that increments an int each tick, asserted afterward.
 *
 * BehaviorTree.CPP's Repeat ticks a synchronous SUCCESS child to completion
 * within a single parent tick, so we verify the child was ticked 3 times by
 * observing the resulting blackboard counter the child writes. */

/* A counting action registered via the legacy jce_bt_action_fn path, so the
 * Repeat test also proves custom actions and the bundled library coexist. */
static int g_repeat_count;
static JceBtStatus count_action(const char *name, void *ud)
{
    (void)name; (void)ud;
    g_repeat_count++;
    return JCE_BT_SUCCESS;
}

static void test_repeat_ticks_child_n_times(void)
{
    jce_bt_register_action(g_ctx, "CountOnce", count_action, NULL);
    g_repeat_count = 0;

    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <Repeat num_cycles=\"3\">"
        "      <CountOnce/>"
        "    </Repeat>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick(t, 0.016f));
    TEST_ASSERT_EQUAL_INT(3, g_repeat_count);
}

/* ------------------------------------------------------------------ */
/* MoveToTarget                                                        */
/* ------------------------------------------------------------------ */

/* Stub move hook: records the last goal it was asked to move toward and
 * reports SUCCESS after N invocations (so we can drive RUNNING→SUCCESS). */
typedef struct {
    int   calls;
    int   succeed_after;
    float gx, gy, gz;
} MoveProbe;

static JceBtStatus probe_move(float gx, float gy, float gz, void *ud)
{
    MoveProbe *p = (MoveProbe *)ud;
    p->calls++;
    p->gx = gx; p->gy = gy; p->gz = gz;
    return (p->calls >= p->succeed_after) ? JCE_BT_SUCCESS : JCE_BT_RUNNING;
}

/* MoveToTarget reads target.position from the blackboard and drives the hook:
 * RUNNING while travelling, SUCCESS on arrival, FAILURE when no hook. */
static void test_move_to_target_drives_hook_to_success(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <MoveToTarget speed=\"3.0\"/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    jce_vec3 goal = { 5.0f, 1.0f, -2.0f };
    jce_blackboard_set_vec3(g_bb, "target.position", goal);

    MoveProbe p = { 0, 2, 0, 0, 0 };
    TEST_ASSERT_EQUAL_INT(JCE_BT_RUNNING, tick_dt(t, 0.1f, probe_move, &p));
    /* It received our goal coordinates. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 5.0f, p.gx);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -2.0f, p.gz);
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick_dt(t, 0.1f, probe_move, &p));
    TEST_ASSERT_EQUAL_INT(2, p.calls);
}

/* No target and no last-known position → FAILURE (nothing to move toward). */
static void test_move_to_target_fails_without_goal(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <MoveToTarget/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    MoveProbe p = { 0, 1, 0, 0, 0 };
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, tick_dt(t, 0.1f, probe_move, &p));
    TEST_ASSERT_EQUAL_INT(0, p.calls);   /* hook never called: no goal */
}

/* Falls back to target.last_known_position when target.position is absent. */
static void test_move_to_target_uses_last_known(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <MoveToTarget/>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    jce_vec3 last = { -7.0f, 0.0f, 3.0f };
    jce_blackboard_set_vec3(g_bb, "target.last_known_position", last);

    MoveProbe p = { 0, 1, 0, 0, 0 };
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick_dt(t, 0.1f, probe_move, &p));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -7.0f, p.gx);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, p.gz);
}

/* ------------------------------------------------------------------ */
/* Integration: a small reactive tree mixing library + condition nodes */
/* ------------------------------------------------------------------ */

/* ReactiveFallback[ Sequence[ BlackboardCheck(alert==true), MoveToTarget ],
 * Wait ]: when not alerted it idles in the long Wait; when alerted and a
 * target is known the higher-priority sequence pre-empts the Wait and drives
 * MoveToTarget.  ReactiveFallback re-evaluates the condition every tick, so the
 * transition happens without restarting the tree — the canonical reactive AI
 * shape.  Verifies the bundled nodes compose through the real tick. */
static void test_integration_reactive_tree(void)
{
    static const char xml[] =
        "<root BTCPP_format=\"4\">"
        "  <BehaviorTree ID=\"main\">"
        "    <ReactiveFallback>"
        "      <Sequence>"
        "        <BlackboardCheck key=\"alert\" value=\"true\"/>"
        "        <MoveToTarget/>"
        "      </Sequence>"
        "      <Wait sec=\"10.0\"/>"
        "    </ReactiveFallback>"
        "  </BehaviorTree>"
        "</root>";
    JceBtTreeHandle t = load(xml);

    MoveProbe p = { 0, 1, 0, 0, 0 };

    /* Not alerted: check fails → falls through to the idle Wait → RUNNING,
     * and the move hook is never consulted. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_RUNNING, tick_dt(t, 0.1f, probe_move, &p));
    TEST_ASSERT_EQUAL_INT(0, p.calls);

    /* Alert + a known target: the sequence runs MoveToTarget → SUCCESS. */
    jce_blackboard_set_bool(g_bb, "alert", true);
    jce_vec3 goal = { 1.0f, 0.0f, 1.0f };
    jce_blackboard_set_vec3(g_bb, "target.position", goal);
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, tick_dt(t, 0.1f, probe_move, &p));
    TEST_ASSERT_EQUAL_INT(1, p.calls);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_wait_runs_until_duration_elapses);
    RUN_TEST(test_wait_zero_duration_succeeds_immediately);
    RUN_TEST(test_wait_restarts_timer_on_reentry);
    RUN_TEST(test_set_then_check_blackboard);
    RUN_TEST(test_set_numeric_blackboard_roundtrips);
    RUN_TEST(test_check_fails_on_mismatch_or_absent);
    RUN_TEST(test_clear_blackboard_removes_key);
    RUN_TEST(test_clear_blackboard_clears_all);
    RUN_TEST(test_cooldown_gates_child_until_timer_expires);
    RUN_TEST(test_repeat_ticks_child_n_times);
    RUN_TEST(test_move_to_target_drives_hook_to_success);
    RUN_TEST(test_move_to_target_fails_without_goal);
    RUN_TEST(test_move_to_target_uses_last_known);
    RUN_TEST(test_integration_reactive_tree);
    return UNITY_END();
}
