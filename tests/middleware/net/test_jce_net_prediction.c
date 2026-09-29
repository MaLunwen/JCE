/*
 * test_jce_net_prediction.c — client-side prediction + rollback core.
 *
 * 100% headless + deterministic.  No transport, no flecs, no physics — the
 * prediction buffer is a standalone module driven by a tiny pure sim:
 *
 *     state = { float x }
 *     input = { float vx }
 *     step  : x' = x + vx
 *
 * Cases:
 *   (1) apply a stream of inputs -> current state == hand-computed sum.
 *   (2) reconcile with an authoritative state that MATCHES -> no-correction,
 *       state unchanged.
 *   (3) reconcile with a DIFFERENT auth state at tick K -> corrected, and the
 *       post-replay current state equals a fresh ground-truth resimulation
 *       from auth_state through the same buffered inputs.
 *   (4) reconcile for an evicted/too-old tick -> clear failure, buffer intact.
 *   (5) ring WRAP: apply > capacity inputs -> oldest evicted + current correct.
 *   (6) NULL-safety across the whole API.
 */

#include "unity.h"

#include <jce/middleware/net/jce_net_prediction.h>

#include <stdint.h>
#include <string.h>

#define EPS 1e-4f

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Tiny deterministic sim                                              */
/* ------------------------------------------------------------------ */

typedef struct SimState { float x; } SimState;
typedef struct SimInput { float vx; } SimInput;

static int step_add(const void *prev_state, const void *input,
                    void *out_state, void *user)
{
    const SimState *p = (const SimState *)prev_state;
    const SimInput *in = (const SimInput *)input;
    SimState       *o = (SimState *)out_state;
    (void)user;
    o->x = p->x + in->vx;
    return 0;
}

/* A step that fails — to exercise the error path. */
static int step_fail(const void *prev_state, const void *input,
                     void *out_state, void *user)
{
    (void)prev_state; (void)input; (void)out_state; (void)user;
    return -1;
}

static SimState make_state(float x) { SimState s; s.x = x; return s; }
static SimInput make_input(float v) { SimInput i; i.vx = v; return i; }

/* ================================================================== */
/* (1) apply stream -> current matches the hand-computed sum           */
/* ================================================================== */

static void test_apply_stream_sum(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f);
    SimState cur;
    const float vs[5] = { 1.0f, 2.0f, -0.5f, 4.0f, 0.25f };
    float       expect = 0.0f;
    uint32_t    t;

    TEST_ASSERT_NOT_NULL(buf);
    jce_prediction_set_initial_state(buf, &base);

    for (t = 0u; t < 5u; ++t) {
        SimInput in = make_input(vs[t]);
        TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION,
            jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL));
        expect += vs[t];
    }

    TEST_ASSERT_EQUAL_UINT32(5u, jce_prediction_count(buf));
    TEST_ASSERT_EQUAL_UINT32(5u, jce_prediction_current_tick(buf));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_prediction_oldest_tick(buf));

    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, expect, cur.x);   /* 1+2-0.5+4+0.25 = 6.75 */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 6.75f, cur.x);

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (2) reconcile with a MATCHING auth state -> no correction           */
/* ================================================================== */

static void test_reconcile_match_noop(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f);
    SimState cur_before, cur_after;
    const float vs[4] = { 1.0f, 1.0f, 1.0f, 1.0f };
    uint32_t    t;
    /* predicted state at tick 3 (after inputs 1,2,3) = 0+1+1+1 = 3.0 */
    SimState auth = make_state(3.0f);
    JcePredictionResult r;

    TEST_ASSERT_NOT_NULL(buf);
    jce_prediction_set_initial_state(buf, &base);
    for (t = 0u; t < 4u; ++t) {
        SimInput in = make_input(vs[t]);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }

    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur_before));

    r = jce_prediction_reconcile(buf, 3u, &auth, step_add, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION, r);

    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur_after));
    TEST_ASSERT_FLOAT_WITHIN(EPS, cur_before.x, cur_after.x);   /* unchanged */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 4.0f, cur_after.x);           /* 4 inputs  */

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (3) reconcile with a DIFFERENT auth -> corrected + ground-truth     */
/* ================================================================== */

static void test_reconcile_diverge_rollback_replay(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f);
    SimState cur;
    const float vs[6] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f };
    uint32_t    t;
    JcePredictionResult r;

    /* The authority disagrees about tick 3.  Our prediction at tick 3 was
     * 0+1+2+3 = 6.0; the server says it was actually 100.0 (e.g. a hit /
     * teleport we couldn't predict). */
    const uint32_t K        = 3u;
    SimState       auth      = make_state(100.0f);

    /* Ground truth: resimulate from auth_state(=100) through the inputs
     * AFTER tick K, i.e. ticks 4,5,6 with vx 4,5,6.
     *   100 + 4 + 5 + 6 = 115.0 */
    float expect_after = 100.0f + 4.0f + 5.0f + 6.0f;

    TEST_ASSERT_NOT_NULL(buf);
    jce_prediction_set_initial_state(buf, &base);
    for (t = 0u; t < 6u; ++t) {
        SimInput in = make_input(vs[t]);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }

    /* Pre-condition: predicted current = sum(1..6) = 21.0 */
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 21.0f, cur.x);

    r = jce_prediction_reconcile(buf, K, &auth, step_add, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_CORRECTED, r);

    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, expect_after, cur.x);   /* 115.0 */

    /* The corrected slot at K must now equal the authority exactly. */
    /* (re-reconcile with the SAME auth -> now matches -> no correction) */
    r = jce_prediction_reconcile(buf, K, &auth, step_add, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION, r);

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (4) reconcile for an evicted / future tick -> clear failure         */
/* ================================================================== */

static void test_reconcile_not_found(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 4u);
    SimState base = make_state(0.0f);
    SimState before, after;
    uint32_t t;
    SimState auth = make_state(123.0f);
    JcePredictionResult r;

    TEST_ASSERT_NOT_NULL(buf);
    jce_prediction_set_initial_state(buf, &base);

    /* Apply 6 inputs into a capacity-4 ring: ticks 1,2 are evicted, the
     * ring now holds ticks 3,4,5,6. */
    for (t = 0u; t < 6u; ++t) {
        SimInput in = make_input(1.0f);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(4u, jce_prediction_count(buf));
    TEST_ASSERT_EQUAL_UINT32(3u, jce_prediction_oldest_tick(buf));

    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &before));

    /* Too-old (evicted) tick 1. */
    r = jce_prediction_reconcile(buf, 1u, &auth, step_add, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NOT_FOUND, r);

    /* Future tick 99 never predicted. */
    r = jce_prediction_reconcile(buf, 99u, &auth, step_add, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NOT_FOUND, r);

    /* Buffer intact: count + current unchanged. */
    TEST_ASSERT_EQUAL_UINT32(4u, jce_prediction_count(buf));
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &after));
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.x, after.x);

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (5) ring WRAP -> oldest evicted + current correct                   */
/* ================================================================== */

static void test_ring_wrap(void)
{
    const uint32_t CAP = 4u;
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), CAP);
    SimState base = make_state(0.0f);
    SimState cur;
    uint32_t t;
    float    running = 0.0f;

    TEST_ASSERT_NOT_NULL(buf);
    TEST_ASSERT_EQUAL_UINT32(CAP, jce_prediction_capacity(buf));
    jce_prediction_set_initial_state(buf, &base);

    /* Apply 10 inputs (vx = tick) into a cap-4 ring. */
    for (t = 1u; t <= 10u; ++t) {
        SimInput in = make_input((float)t);
        TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION,
            jce_prediction_apply_input(buf, t, &in, step_add, NULL));
        running += (float)t;
    }

    /* Ring holds only the last 4 ticks (7,8,9,10) but the running STATE is
     * cumulative because each apply chains off the prior newest state. */
    TEST_ASSERT_EQUAL_UINT32(CAP, jce_prediction_count(buf));
    TEST_ASSERT_EQUAL_UINT32(10u, jce_prediction_current_tick(buf));
    TEST_ASSERT_EQUAL_UINT32(7u, jce_prediction_oldest_tick(buf));

    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    /* sum(1..10) = 55.0 */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 55.0f, cur.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, running, cur.x);

    /* Reconcile a still-buffered tick (8): predicted state after ticks
     * 1..8 = 36.0.  Feed a DIFFERENT auth (1000) and verify replay of
     * ticks 9,10 lands at 1000 + 9 + 10 = 1019. */
    {
        SimState auth = make_state(1000.0f);
        JcePredictionResult r =
            jce_prediction_reconcile(buf, 8u, &auth, step_add, NULL, NULL);
        TEST_ASSERT_EQUAL_INT(JCE_PREDICT_CORRECTED, r);
        TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
        TEST_ASSERT_FLOAT_WITHIN(EPS, 1019.0f, cur.x);
    }

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (6) NULL-safety + error paths                                       */
/* ================================================================== */

static void test_null_safety(void)
{
    JcePredictionBuffer *buf;
    SimState state = make_state(1.0f);
    SimInput in    = make_input(1.0f);
    SimState out;

    /* create rejects zero sizes / capacity. */
    TEST_ASSERT_NULL(jce_prediction_buffer_create(0u, 4u, 4u));
    TEST_ASSERT_NULL(jce_prediction_buffer_create(4u, 0u, 4u));
    TEST_ASSERT_NULL(jce_prediction_buffer_create(4u, 4u, 0u));

    /* All queries NULL-safe. */
    TEST_ASSERT_FALSE(jce_prediction_get_current_state(NULL, &out));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_prediction_current_tick(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_prediction_count(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_prediction_capacity(NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_prediction_oldest_tick(NULL));
    jce_prediction_buffer_destroy(NULL);             /* no crash */
    jce_prediction_set_initial_state(NULL, &state);  /* no crash */

    /* apply / reconcile NULL args. */
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_apply_input(NULL, 1u, &in, step_add, NULL));

    buf = jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 8u);
    TEST_ASSERT_NOT_NULL(buf);

    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_apply_input(buf, 1u, NULL, step_add, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_apply_input(buf, 1u, &in, NULL, NULL));

    /* apply before seeding the base -> NO_BASELINE. */
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NO_BASELINE,
        jce_prediction_apply_input(buf, 1u, &in, step_add, NULL));

    /* current state with no base + empty ring -> false. */
    TEST_ASSERT_FALSE(jce_prediction_get_current_state(buf, &out));

    jce_prediction_set_initial_state(buf, &state);

    /* set_initial_state NULL state -> no-op (base stays seeded at 1.0). */
    jce_prediction_set_initial_state(buf, NULL);
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &out));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out.x);

    /* reconcile NULL args + empty-ring NOT_FOUND. */
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_reconcile(NULL, 1u, &state, step_add, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_reconcile(buf, 1u, NULL, step_add, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_reconcile(buf, 1u, &state, NULL, NULL, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NOT_FOUND,
        jce_prediction_reconcile(buf, 1u, &state, step_add, NULL, NULL));

    /* A failing step on apply leaves the buffer unchanged. */
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_STEP_FAILED,
        jce_prediction_apply_input(buf, 1u, &in, step_fail, NULL));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_prediction_count(buf));

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* runner                                                             */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_apply_stream_sum);
    RUN_TEST(test_reconcile_match_noop);
    RUN_TEST(test_reconcile_diverge_rollback_replay);
    RUN_TEST(test_reconcile_not_found);
    RUN_TEST(test_ring_wrap);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
