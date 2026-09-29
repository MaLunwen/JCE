/*
 * test_jce_prediction_wiring.c — proves the CLIENT-PREDICTION composition:
 * the deterministic locomotion step (jce_predict_locomotion) driven through
 * the generic predict ring + reconcile core (jce_net_prediction).
 *
 * This is the headless, deterministic proof that the runtime's
 * apply_input -> get_current_state -> reconcile(rollback/replay) pipeline is
 * correct.  It exercises the exact step fn + compare fn the runtime composes
 * (jce_predict_locomotion_step / jce_predict_loco_compare), without needing an
 * ECS / physics / runtime / transport — both the locomotion module and the
 * prediction core are pure.  Real over-the-wire client-server prediction is F12.
 *
 * Cases:
 *   (1) walk +x over ticks 1..5 -> predicted pos advances by move_speed*dt
 *       each tick (known deterministic values).
 *   (2) reconcile with an authoritative state == predicted at tick 3 (tolerant
 *       compare) -> NO correction, current state unchanged.
 *   (3) reconcile with a divergent auth at tick 3 -> rollback to auth + replay
 *       inputs 4,5 -> corrected current == ground truth re-simulated in-test.
 *   (4) the step fn is PURE: same inputs => byte-identical states.
 *   (5) gravity + ground clamp: an idle entity above ground falls to ground_y
 *       and stops (vel.y == 0), proving the vertical path is deterministic too.
 */

#include "unity.h"

#include <jce/middleware/net/jce_net_prediction.h>
#include <jce/middleware/net/jce_predict_locomotion.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

#define EPS 1e-4f

void setUp(void)    {}
void tearDown(void) {}

/* ------------------------------------------------------------------ */
/* Shared params: flat ground, no gravity surprises for the XZ cases  */
/* ------------------------------------------------------------------ */

static JcePredictLocoParams make_params(void)
{
    JcePredictLocoParams p;
    jce_predict_loco_params_default(&p);
    p.dt         = 1.0f / 60.0f;
    p.move_speed = 6.0f;
    p.ground_y   = 0.0f;
    return p;
}

static JcePredictInput walk_x_input(float vx)
{
    JcePredictInput in;
    memset(&in, 0, sizeof in);
    in.walk_x     = vx;     /* +x direction */
    in.walk_z     = 0.0f;
    in.speed_mult = 1.0f;
    in.jump       = 0u;
    in.sprint     = 0u;
    return in;
}

/* ================================================================== */
/* (1) walk +x stream -> deterministic advance by move_speed*dt        */
/* ================================================================== */

static void test_walk_advances_deterministically(void)
{
    JcePredictLocoParams params = make_params();
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(JcePredictInput),
                                     sizeof(JcePredictState), 32u);
    TEST_ASSERT_NOT_NULL(buf);

    JcePredictState base;
    memset(&base, 0, sizeof base);     /* origin, on ground */
    jce_prediction_set_initial_state(buf, &base);

    const float step_dx = params.move_speed * params.dt;   /* 6 * 1/60 = 0.1 */

    for (uint32_t t = 1u; t <= 5u; ++t) {
        JcePredictInput in = walk_x_input(1.0f);
        TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION,
            jce_prediction_apply_input(buf, t, &in,
                                       jce_predict_locomotion_step, &params));

        JcePredictState cur;
        TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
        TEST_ASSERT_FLOAT_WITHIN(EPS, step_dx * (float)t, cur.pos[0]);
        TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cur.pos[2]);
        /* Ground-clamped: y stays at ground_y (gravity each tick, clamped). */
        TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cur.pos[1]);
    }

    TEST_ASSERT_EQUAL_UINT32(5u, jce_prediction_count(buf));
    TEST_ASSERT_EQUAL_UINT32(5u, jce_prediction_current_tick(buf));

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (2) reconcile with matching auth -> no correction                   */
/* ================================================================== */

static void test_reconcile_match_noop(void)
{
    JcePredictLocoParams params = make_params();
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(JcePredictInput),
                                     sizeof(JcePredictState), 32u);
    TEST_ASSERT_NOT_NULL(buf);

    JcePredictState base;
    memset(&base, 0, sizeof base);
    jce_prediction_set_initial_state(buf, &base);

    for (uint32_t t = 1u; t <= 5u; ++t) {
        JcePredictInput in = walk_x_input(1.0f);
        jce_prediction_apply_input(buf, t, &in,
                                   jce_predict_locomotion_step, &params);
    }

    JcePredictState before;
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &before));

    /* Authority agrees with prediction at tick 3 (pos.x = 0.3) — within the
     * tolerant compare epsilon, so reconcile must NOT correct. */
    const float step_dx = params.move_speed * params.dt;
    JcePredictState auth;
    memset(&auth, 0, sizeof auth);
    auth.pos[0] = step_dx * 3.0f;      /* exactly the predicted tick-3 pos */

    JcePredictionResult r =
        jce_prediction_reconcile(buf, 3u, &auth,
                                 jce_predict_locomotion_step,
                                 jce_predict_loco_compare, &params);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION, r);

    JcePredictState after;
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &after));
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.pos[0], after.pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.pos[2], after.pos[2]);

    jce_prediction_buffer_destroy(buf);
}

/* Tolerant compare also ignores noise WITHIN epsilon (no correction). */
static void test_reconcile_within_epsilon_noop(void)
{
    JcePredictLocoParams params = make_params();
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(JcePredictInput),
                                     sizeof(JcePredictState), 32u);
    TEST_ASSERT_NOT_NULL(buf);

    JcePredictState base;
    memset(&base, 0, sizeof base);
    jce_prediction_set_initial_state(buf, &base);

    for (uint32_t t = 1u; t <= 5u; ++t) {
        JcePredictInput in = walk_x_input(1.0f);
        jce_prediction_apply_input(buf, t, &in,
                                   jce_predict_locomotion_step, &params);
    }

    JcePredictState before;
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &before));

    const float step_dx = params.move_speed * params.dt;
    JcePredictState auth;
    memset(&auth, 0, sizeof auth);
    /* Offset well inside JCE_PREDICT_LOCO_POS_EPSILON (1cm): ~1mm of noise. */
    auth.pos[0] = step_dx * 3.0f + 0.001f;

    JcePredictionResult r =
        jce_prediction_reconcile(buf, 3u, &auth,
                                 jce_predict_locomotion_step,
                                 jce_predict_loco_compare, &params);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION, r);

    JcePredictState after;
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &after));
    TEST_ASSERT_FLOAT_WITHIN(EPS, before.pos[0], after.pos[0]);

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (3) divergent auth -> rollback + replay == in-test ground truth     */
/* ================================================================== */

static void test_reconcile_diverge_rollback_replay(void)
{
    JcePredictLocoParams params = make_params();
    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(JcePredictInput),
                                     sizeof(JcePredictState), 32u);
    TEST_ASSERT_NOT_NULL(buf);

    JcePredictState base;
    memset(&base, 0, sizeof base);
    jce_prediction_set_initial_state(buf, &base);

    /* Inputs ticks 1..5 all walk +x. */
    JcePredictInput inputs[5];
    for (int i = 0; i < 5; ++i) inputs[i] = walk_x_input(1.0f);
    for (uint32_t t = 1u; t <= 5u; ++t)
        jce_prediction_apply_input(buf, t, &inputs[t - 1u],
                                   jce_predict_locomotion_step, &params);

    /* The authority disagrees at tick 3: it says we were actually pushed to
     * x = 10.0 (a hit / teleport prediction couldn't anticipate). */
    JcePredictState auth;
    memset(&auth, 0, sizeof auth);
    auth.pos[0] = 10.0f;

    /* Ground truth: rollback to auth at t3, replay the inputs AFTER t3
     * (ticks 4 and 5) through the SAME pure step fn. */
    JcePredictState gt = auth;
    JcePredictState tmp;
    TEST_ASSERT_EQUAL_INT(0, jce_predict_locomotion_step(&gt, &inputs[3], &tmp, &params));
    gt = tmp;
    TEST_ASSERT_EQUAL_INT(0, jce_predict_locomotion_step(&gt, &inputs[4], &tmp, &params));
    gt = tmp;

    JcePredictionResult r =
        jce_prediction_reconcile(buf, 3u, &auth,
                                 jce_predict_locomotion_step,
                                 jce_predict_loco_compare, &params);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_CORRECTED, r);

    JcePredictState cur;
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, gt.pos[0], cur.pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, gt.pos[1], cur.pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, gt.pos[2], cur.pos[2]);

    /* Re-reconciling with the same auth now matches (no further correction). */
    r = jce_prediction_reconcile(buf, 3u, &auth,
                                 jce_predict_locomotion_step,
                                 jce_predict_loco_compare, &params);
    TEST_ASSERT_EQUAL_INT(JCE_PREDICT_OK_NO_CORRECTION, r);

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* (4) the step fn is PURE: same inputs => identical states            */
/* ================================================================== */

static void test_step_is_pure_deterministic(void)
{
    JcePredictLocoParams params = make_params();

    JcePredictState prev;
    memset(&prev, 0, sizeof prev);
    prev.pos[0] = 1.25f; prev.pos[1] = 2.0f; prev.pos[2] = -3.5f;
    prev.vel[1] = 1.0f;
    JcePredictInput in = walk_x_input(1.0f);
    in.sprint = 1u;

    JcePredictState a, b;
    TEST_ASSERT_EQUAL_INT(0, jce_predict_locomotion_step(&prev, &in, &a, &params));
    TEST_ASSERT_EQUAL_INT(0, jce_predict_locomotion_step(&prev, &in, &b, &params));

    /* Byte-identical: a pure float function of (prev, in, params). */
    TEST_ASSERT_EQUAL_INT(0, memcmp(&a, &b, sizeof a));

    /* NULL prev/out -> step failure (non-zero) per the contract. */
    TEST_ASSERT_NOT_EQUAL(0, jce_predict_locomotion_step(NULL, &in, &a, &params));
    TEST_ASSERT_NOT_EQUAL(0, jce_predict_locomotion_step(&prev, &in, NULL, &params));
}

/* ================================================================== */
/* (5) gravity + ground clamp is deterministic                         */
/* ================================================================== */

static void test_gravity_ground_clamp(void)
{
    JcePredictLocoParams params = make_params();
    params.gravity  = 20.0f;
    params.ground_y = 0.0f;

    JcePredictionBuffer *buf =
        jce_prediction_buffer_create(sizeof(JcePredictInput),
                                     sizeof(JcePredictState), 64u);
    TEST_ASSERT_NOT_NULL(buf);

    JcePredictState base;
    memset(&base, 0, sizeof base);
    base.pos[1] = 5.0f;       /* start 5m above ground, idle */
    jce_prediction_set_initial_state(buf, &base);

    JcePredictInput idle;
    memset(&idle, 0, sizeof idle);
    idle.speed_mult = 1.0f;

    /* 120 ticks (~2s) of falling under gravity must settle on the ground. */
    for (uint32_t t = 1u; t <= 120u; ++t)
        jce_prediction_apply_input(buf, t, &idle,
                                   jce_predict_locomotion_step, &params);

    JcePredictState cur;
    TEST_ASSERT_TRUE(jce_prediction_get_current_state(buf, &cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, params.ground_y, cur.pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cur.vel[1]);   /* clamped, at rest */
    /* No horizontal drift from idle input. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cur.pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, cur.pos[2]);

    jce_prediction_buffer_destroy(buf);
}

/* ================================================================== */
/* runner                                                             */
/* ================================================================== */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_walk_advances_deterministically);
    RUN_TEST(test_reconcile_match_noop);
    RUN_TEST(test_reconcile_within_epsilon_noop);
    RUN_TEST(test_reconcile_diverge_rollback_replay);
    RUN_TEST(test_step_is_pure_deterministic);
    RUN_TEST(test_gravity_ground_clamp);
    return UNITY_END();
}
