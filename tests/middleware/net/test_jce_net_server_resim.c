/*
 * test_jce_net_server_resim.c — SERVER-side re-tick on a late input.
 *
 * 100% headless + deterministic, on the same tiny pure sim as the client
 * prediction test:
 *     state = { float x };  input = { float vx };  step : x' = x + vx
 *
 * Exercises jce_prediction_resim_from_input — the server analogue of reconcile,
 * where the correction source is a LATE INPUT (the server owns state, the client
 * owns input).  Unlike the latest-wins store that DROPS late inputs, a late
 * input within the buffered window is re-applied at its tick and the timeline is
 * replayed forward.
 *
 * Cases:
 *   (1) a late input mid-window corrects + replays; the post-replay current
 *       state equals a fresh ground-truth sim with the late input in place.
 *   (2) an identical late input -> no correction, buffer untouched.
 *   (3) a tick outside the window -> NOT_FOUND, buffer untouched.
 *   (4) a late input at the OLDEST tick AFTER ring wrap uses the advanced base
 *       floor as its predecessor (validates the eviction-floor fix).
 *   (5) a failing step mid-replay -> STEP_FAILED.
 *   (6) NULL / no-baseline / empty-ring safety.
 *   (7) end-to-end server scenario: an out-of-order arrival reproduces the
 *       exact authoritative timeline it would have had in order.
 */

#include "unity.h"

#include <jce/middleware/net/jce_net_prediction.h>

#include <stdint.h>
#include <string.h>

#define EPS 1e-4f

void setUp(void)    {}
void tearDown(void) {}

typedef struct SimState { float x; } SimState;
typedef struct SimInput { float vx; } SimInput;

static int step_add(const void *prev_state, const void *input,
                    void *out_state, void *user)
{
    const SimState *p  = (const SimState *)prev_state;
    const SimInput *in = (const SimInput *)input;
    SimState       *o  = (SimState *)out_state;
    (void)user;
    o->x = p->x + in->vx;
    return 0;
}

static int step_fail(const void *prev_state, const void *input,
                     void *out_state, void *user)
{
    (void)prev_state; (void)input; (void)out_state; (void)user;
    return -1;
}

static SimState make_state(float x) { SimState s; s.x = x; return s; }
static SimInput make_input(float v) { SimInput i; i.vx = v; return i; }

/* Ground truth: sum base + every input (the sim is x += vx). */
static float ground_truth(float base, const float *vs, int n)
{
    float x = base;
    for (int i = 0; i < n; ++i) x += vs[i];
    return x;
}

/* ── (1) late input mid-window corrects + replays ──────────────────────── */
static void test_late_input_corrects_and_replays(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f), cur;
    const float vs[5] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f };
    TEST_ASSERT_NOT_NULL(buf);
    jce_prediction_set_initial_state(buf, &base);

    for (uint32_t t = 0; t < 5; ++t) {
        SimInput in = make_input(vs[t]);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }
    /* newest = 1+2+3+4+5 = 15 */
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 15.0f, cur.x);

    /* Late input for tick 3: vx 3 -> 10. */
    SimInput late = make_input(10.0f);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_CORRECTED,
        jce_prediction_resim_from_input(buf, 3u, &late, step_add, NULL));

    const float corrected[5] = { 1.0f, 2.0f, 10.0f, 4.0f, 5.0f };
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, ground_truth(0.0f, corrected, 5), cur.x); /* 22 */

    jce_prediction_buffer_destroy(buf);
}

/* ── (2) identical late input -> no correction ─────────────────────────── */
static void test_identical_late_input_no_correction(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f), cur;
    const float vs[4] = { 1.0f, 2.0f, 3.0f, 4.0f };
    jce_prediction_set_initial_state(buf, &base);
    for (uint32_t t = 0; t < 4; ++t) {
        SimInput in = make_input(vs[t]);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }

    SimInput same = make_input(3.0f);   /* tick 3 already had vx 3 */
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION,
        jce_prediction_resim_from_input(buf, 3u, &same, step_add, NULL));
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, cur.x);   /* unchanged 1+2+3+4 */

    jce_prediction_buffer_destroy(buf);
}

/* ── (3) out-of-window tick -> NOT_FOUND, buffer intact ────────────────── */
static void test_out_of_window_not_found(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f), cur;
    jce_prediction_set_initial_state(buf, &base);
    for (uint32_t t = 0; t < 3; ++t) {
        SimInput in = make_input(1.0f);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }
    SimInput late = make_input(9.0f);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NOT_FOUND,
        jce_prediction_resim_from_input(buf, 99u, &late, step_add, NULL));
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, cur.x);   /* untouched */

    jce_prediction_buffer_destroy(buf);
}

/* ── (4) late input at the OLDEST tick after wrap uses the base floor ───── */
static void test_oldest_after_wrap_uses_floor(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 4u);
    SimState base = make_state(0.0f), cur;
    jce_prediction_set_initial_state(buf, &base);
    /* 6 ticks of vx=1 through a capacity-4 ring -> window {3,4,5,6}, base floor
     * advanced to the state before tick 3 (== 2). */
    for (uint32_t t = 0; t < 6; ++t) {
        SimInput in = make_input(1.0f);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }
    TEST_ASSERT_EQUAL_UINT32(3u, jce_prediction_oldest_tick(buf));
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 6.0f, cur.x);   /* 1..6 summed */

    /* Late input at oldest tick 3: vx 1 -> 5.  Predecessor is the base floor (2),
     * so window becomes 2+5=7, then +1,+1,+1 -> 10. */
    SimInput late = make_input(5.0f);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_CORRECTED,
        jce_prediction_resim_from_input(buf, 3u, &late, step_add, NULL));
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, cur.x);

    jce_prediction_buffer_destroy(buf);
}

/* ── (5) failing step mid-replay -> STEP_FAILED ────────────────────────── */
static void test_step_failure(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 8u);
    SimState base = make_state(0.0f);
    jce_prediction_set_initial_state(buf, &base);
    for (uint32_t t = 0; t < 3; ++t) {
        SimInput in = make_input(1.0f);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }
    SimInput late = make_input(9.0f);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_STEP_FAILED,
        jce_prediction_resim_from_input(buf, 2u, &late, step_fail, NULL));

    jce_prediction_buffer_destroy(buf);
}

/* ── (6) NULL / no-baseline / empty-ring safety ────────────────────────── */
static void test_null_and_empty_safety(void)
{
    SimInput in = make_input(1.0f);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_resim_from_input(NULL, 1u, &in, step_add, NULL));

    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 4u);
    TEST_ASSERT_NOT_NULL(buf);

    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_resim_from_input(buf, 1u, NULL, step_add, NULL));
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NULL,
        jce_prediction_resim_from_input(buf, 1u, &in, NULL, NULL));

    /* No base seeded yet. */
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NO_BASELINE,
        jce_prediction_resim_from_input(buf, 1u, &in, step_add, NULL));

    /* Base seeded but ring empty. */
    SimState base = make_state(0.0f);
    jce_prediction_set_initial_state(buf, &base);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_ERR_NOT_FOUND,
        jce_prediction_resim_from_input(buf, 1u, &in, step_add, NULL));

    jce_prediction_buffer_destroy(buf);
}

/* ── (7) end-to-end: out-of-order arrival == in-order timeline ─────────── */
static void test_server_scenario_equivalence(void)
{
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(SimInput), sizeof(SimState), 16u);
    SimState base = make_state(0.0f), cur;
    jce_prediction_set_initial_state(buf, &base);

    /* The server received tick 5's input as a STALE value first (network
     * extrapolated 0), built ticks 1..8, then the real tick-5 input arrives. */
    const float applied[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 0.0f, 6.0f, 7.0f, 8.0f };
    for (uint32_t t = 0; t < 8; ++t) {
        SimInput in = make_input(applied[t]);
        jce_prediction_apply_input(buf, t + 1u, &in, step_add, NULL);
    }

    /* The authoritative tick-5 input finally arrives (vx 5). */
    SimInput real5 = make_input(5.0f);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_CORRECTED,
        jce_prediction_resim_from_input(buf, 5u, &real5, step_add, NULL));

    const float in_order[8] = { 1.0f, 2.0f, 3.0f, 4.0f, 5.0f, 6.0f, 7.0f, 8.0f };
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, ground_truth(0.0f, in_order, 8), cur.x); /* 36 */

    jce_prediction_buffer_destroy(buf);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_late_input_corrects_and_replays);
    RUN_TEST(test_identical_late_input_no_correction);
    RUN_TEST(test_out_of_window_not_found);
    RUN_TEST(test_oldest_after_wrap_uses_floor);
    RUN_TEST(test_step_failure);
    RUN_TEST(test_null_and_empty_safety);
    RUN_TEST(test_server_scenario_equivalence);
    return UNITY_END();
}
