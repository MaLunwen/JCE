/*
 * test_jce_eqs_bt.c — Unit tests for the EQS <-> Behavior-Tree adapter
 * (L3 AI, jce_eqs_bt.h).
 *
 * The adapter is a registerable BT ACTION ("RunEqsQuery") that reads the agent
 * context from a JceBlackboard (self_pos + optional target_pos), runs a
 * caller-configured pure EQS query (generator recentered on self + tests with
 * the target substituted in), and writes the best candidate back to the
 * blackboard.  These tests are fully headless + deterministic: no nav/physics/
 * BT-XML, just the action callback driven directly with hand-computed expected
 * candidates.
 */

#include "unity.h"

#include <jce/middleware/ai/jce_eqs_bt.h>
#include <jce/middleware/ai/jce_eqs.h>
#include <jce/middleware/ai/jce_perception.h>
#include <jce/os/core/jce_math.h>

#include <math.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

static void assert_v3_near(jce_vec3 a, jce_vec3 b, float eps)
{
    TEST_ASSERT_FLOAT_WITHIN(eps, b.x, a.x);
    TEST_ASSERT_FLOAT_WITHIN(eps, b.y, a.y);
    TEST_ASSERT_FLOAT_WITHIN(eps, b.z, a.z);
}

/*
 * Build the canonical query template used by several tests:
 *   GRID 5x5, spacing 1.0 (X,Z offsets {-2,-1,0,1,2} about the center).
 *   test[0] scoring : DISTANCE to target, INVERSE curve, weight 1
 *                     (nearest-to-target wins).  param_vec3 <- target.
 *   test[1] filter  : DISTANCE to target <= filter_max, weight 0
 *                     (within-range gate, no score).  param_vec3 <- target.
 * `tests` (>=2) and `subs` (>=2) are filled by the caller and kept alive for
 * the query's lifetime.
 */
static void build_template(JceEqsQueryDesc *q, JceEqsTestDesc tests[2],
                           JceEqsBtSub subs[2], float filter_max)
{
    JceEqsGeneratorDesc g = { 0 };
    g.kind         = JCE_EQS_GEN_GRID;
    g.center       = jce_v3(0.0f, 0.0f, 0.0f); /* recentered on self at tick */
    g.grid_width   = 5u;
    g.grid_height  = 5u;
    g.grid_spacing = 1.0f;

    tests[0]            = (JceEqsTestDesc){ 0 };
    tests[0].kind       = JCE_EQS_TEST_DISTANCE;
    tests[0].curve      = JCE_EQS_CURVE_INVERSE; /* 1/(1+dist): nearer = higher */
    tests[0].weight     = 1.0f;
    tests[0].param_vec3 = jce_v3(0.0f, 0.0f, 0.0f); /* overwritten by target */

    tests[1]                = (JceEqsTestDesc){ 0 };
    tests[1].kind           = JCE_EQS_TEST_DISTANCE;
    tests[1].curve          = JCE_EQS_CURVE_INVERSE; /* curve irrelevant; w=0 */
    tests[1].weight         = 0.0f;
    tests[1].param_vec3     = jce_v3(0.0f, 0.0f, 0.0f); /* overwritten by target */
    tests[1].has_filter_max = true;
    tests[1].filter_max     = filter_max;

    subs[0] = JCE_EQS_BT_SUB_PARAM_VEC3; /* scoring test follows the target */
    subs[1] = JCE_EQS_BT_SUB_PARAM_VEC3; /* filter test follows the target  */

    *q = (JceEqsQueryDesc){ 0 };
    q->generator  = g;
    q->tests      = tests;
    q->test_count = 2u;
}

/* ------------------------------------------------------------------ */
/* SUCCESS: nearest-to-target in-range grid point is written          */
/* ------------------------------------------------------------------ */

static void test_success_writes_nearest_to_target(void)
{
    /* self=(10,0,10) -> grid X,Z in {8,9,10,11,12}, Y carried = 0.
     * target=(12,0,10) coincides with the grid corner (12,10), so the nearest
     * candidate is exactly the target: dist 0 -> INVERSE score 1.0, and it is
     * within the 2.0 range filter.  Expected best = (12,0,10). */
    JceBlackboard *bb  = jce_blackboard_create();
    JceBtContext  *ctx = jce_bt_create();
    JceEqs        *eqs = jce_eqs_create(64u);
    TEST_ASSERT_NOT_NULL(bb);
    TEST_ASSERT_NOT_NULL(ctx);
    TEST_ASSERT_NOT_NULL(eqs);

    jce_blackboard_set_vec3(bb, JCE_EQS_BT_KEY_SELF_POS,   jce_v3(10.0f, 0.0f, 10.0f));
    jce_blackboard_set_vec3(bb, JCE_EQS_BT_KEY_TARGET_POS, jce_v3(12.0f, 0.0f, 10.0f));

    JceEqsTestDesc tests[2];
    JceEqsBtSub    subs[2];
    JceEqsQueryDesc q;
    build_template(&q, tests, subs, 2.0f);

    JceEqsBtAction act = { 0 };
    act.eqs              = eqs;
    act.bb               = bb;
    act.query            = q;
    act.subs             = subs;
    act.recenter_on_self = true;

    /* Register + drive through the BT action signature directly. */
    jce_eqs_bt_register(ctx, "RunEqsQuery", &act);
    JceBtStatus st = jce_eqs_bt_action("RunEqsQuery", &act);
    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, st);

    /* The winner was published to the result key. */
    TEST_ASSERT_EQUAL_INT(JCE_BB_VEC3, jce_blackboard_kind(bb, JCE_EQS_BT_KEY_RESULT));
    jce_vec3 res = jce_blackboard_get_vec3(bb, JCE_EQS_BT_KEY_RESULT,
                                           jce_v3(0.0f, 0.0f, 0.0f));
    assert_v3_near(res, jce_v3(12.0f, 0.0f, 10.0f), EPS);

    /* Side channel agrees: score = 1/(1+0) = 1.0 (filter weight 0). */
    TEST_ASSERT_TRUE(act.last_ok);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, act.last_score);
    assert_v3_near(act.last_result, jce_v3(12.0f, 0.0f, 10.0f), EPS);

    jce_eqs_destroy(eqs);
    jce_bt_destroy(ctx);
    jce_blackboard_destroy(bb);
}

/* Custom keys + a target that does NOT land on a grid point still picks the
 * closest grid candidate (and proves key overrides are honored). */
static void test_success_custom_keys_offgrid_target(void)
{
    /* self=(0,0,0) -> X,Z in {-2,-1,0,1,2}.  target=(1.4,0,0): nearest grid
     * point is (1,0) (dist 0.4) vs (2,0) (dist 0.6) -> (1,0,0) wins.  Filter
     * range 3.0 keeps everything. */
    JceBlackboard *bb  = jce_blackboard_create();
    JceEqs        *eqs = jce_eqs_create(64u);

    jce_blackboard_set_vec3(bb, "agent.pos",  jce_v3(0.0f, 0.0f, 0.0f));
    jce_blackboard_set_vec3(bb, "enemy.pos",  jce_v3(1.4f, 0.0f, 0.0f));

    JceEqsTestDesc tests[2];
    JceEqsBtSub    subs[2];
    JceEqsQueryDesc q;
    build_template(&q, tests, subs, 3.0f);

    JceEqsBtAction act = { 0 };
    act.eqs              = eqs;
    act.bb               = bb;
    act.query            = q;
    act.subs             = subs;
    act.recenter_on_self = true;
    act.self_pos_key     = "agent.pos";
    act.target_pos_key   = "enemy.pos";
    act.result_key       = "cover.spot";

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, jce_eqs_bt_action(NULL, &act));

    jce_vec3 res = jce_blackboard_get_vec3(bb, "cover.spot",
                                           jce_v3(9.0f, 9.0f, 9.0f));
    assert_v3_near(res, jce_v3(1.0f, 0.0f, 0.0f), EPS);
    /* Default result key was NOT written (override honored). */
    TEST_ASSERT_FALSE(jce_blackboard_has(bb, JCE_EQS_BT_KEY_RESULT));

    jce_eqs_destroy(eqs);
    jce_blackboard_destroy(bb);
}

/* ------------------------------------------------------------------ */
/* FAILURE: every candidate filtered out                              */
/* ------------------------------------------------------------------ */

static void test_failure_all_filtered(void)
{
    /* target=(12.5,0,10) off-grid; range filter 0.3.  Nearest grid point to
     * target is (12,10) at dist 0.5 > 0.3 -> NO candidate survives -> FAILURE,
     * and the result key is never written. */
    JceBlackboard *bb  = jce_blackboard_create();
    JceEqs        *eqs = jce_eqs_create(64u);

    jce_blackboard_set_vec3(bb, JCE_EQS_BT_KEY_SELF_POS,   jce_v3(10.0f, 0.0f, 10.0f));
    jce_blackboard_set_vec3(bb, JCE_EQS_BT_KEY_TARGET_POS, jce_v3(12.5f, 0.0f, 10.0f));

    JceEqsTestDesc tests[2];
    JceEqsBtSub    subs[2];
    JceEqsQueryDesc q;
    build_template(&q, tests, subs, 0.3f);

    JceEqsBtAction act = { 0 };
    act.eqs              = eqs;
    act.bb               = bb;
    act.query            = q;
    act.subs             = subs;
    act.recenter_on_self = true;

    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, jce_eqs_bt_action(NULL, &act));
    TEST_ASSERT_FALSE(jce_blackboard_has(bb, JCE_EQS_BT_KEY_RESULT));
    TEST_ASSERT_FALSE(act.last_ok);

    jce_eqs_destroy(eqs);
    jce_blackboard_destroy(bb);
}

/* ------------------------------------------------------------------ */
/* No target present: the query still runs (substitution skipped).    */
/* ------------------------------------------------------------------ */

static void test_no_target_runs_self_centered(void)
{
    /* No target_pos on the blackboard.  The template's tests keep their (zero)
     * param_vec3.  With INVERSE distance-to-(0,0,0) scoring and self=(0,0,0),
     * the generator centers on self so the candidate AT the center (0,0,0)
     * scores 1/(1+0)=1 and wins.  Filter (to zero, range 5) keeps it. */
    JceBlackboard *bb  = jce_blackboard_create();
    JceEqs        *eqs = jce_eqs_create(64u);

    jce_blackboard_set_vec3(bb, JCE_EQS_BT_KEY_SELF_POS, jce_v3(0.0f, 0.0f, 0.0f));
    /* deliberately NO target_pos */

    JceEqsTestDesc tests[2];
    JceEqsBtSub    subs[2];
    JceEqsQueryDesc q;
    build_template(&q, tests, subs, 5.0f);

    JceEqsBtAction act = { 0 };
    act.eqs              = eqs;
    act.bb               = bb;
    act.query            = q;
    act.subs             = subs;
    act.recenter_on_self = true;

    TEST_ASSERT_EQUAL_INT(JCE_BT_SUCCESS, jce_eqs_bt_action(NULL, &act));
    jce_vec3 res = jce_blackboard_get_vec3(bb, JCE_EQS_BT_KEY_RESULT,
                                           jce_v3(9.0f, 9.0f, 9.0f));
    assert_v3_near(res, jce_v3(0.0f, 0.0f, 0.0f), EPS);

    jce_eqs_destroy(eqs);
    jce_blackboard_destroy(bb);
}

/* ------------------------------------------------------------------ */
/* NULL / malformed-context safety                                    */
/* ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    /* NULL userdata. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, jce_eqs_bt_action("RunEqsQuery", NULL));

    /* Action missing eqs / bb. */
    JceEqsBtAction empty = { 0 };
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, jce_eqs_bt_action(NULL, &empty));

    JceBlackboard *bb  = jce_blackboard_create();
    JceEqs        *eqs = jce_eqs_create(8u);

    JceEqsTestDesc tests[2];
    JceEqsBtSub    subs[2];
    JceEqsQueryDesc q;
    build_template(&q, tests, subs, 2.0f);

    JceEqsBtAction act = { 0 };
    act.eqs              = eqs;
    act.bb               = bb;
    act.query            = q;
    act.subs             = subs;
    act.recenter_on_self = true;

    /* self_pos absent -> FAILURE, nothing written. */
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, jce_eqs_bt_action(NULL, &act));
    TEST_ASSERT_FALSE(jce_blackboard_has(bb, JCE_EQS_BT_KEY_RESULT));

    /* self_pos present but WRONG kind (int) -> FAILURE. */
    jce_blackboard_set_int(bb, JCE_EQS_BT_KEY_SELF_POS, 3);
    TEST_ASSERT_EQUAL_INT(JCE_BT_FAILURE, jce_eqs_bt_action(NULL, &act));

    /* jce_eqs_bt_register tolerates NULL ctx / NULL action (no crash). */
    jce_eqs_bt_register(NULL, "RunEqsQuery", &act);
    jce_eqs_bt_register(NULL, NULL, NULL);

    jce_eqs_destroy(eqs);
    jce_blackboard_destroy(bb);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_success_writes_nearest_to_target);
    RUN_TEST(test_success_custom_keys_offgrid_target);
    RUN_TEST(test_failure_all_filtered);
    RUN_TEST(test_no_target_runs_self_centered);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
