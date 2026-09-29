/* test_jce_anim_ik.c
 *
 * Pure-math unit tests for the two-bone analytic IK solver and the
 * frame-event dispatcher.  Both functions are state-free; we craft
 * minimal inputs and verify the geometric invariants the solver
 * promises, then check the event firing rules across the four
 * playback regimes (forward, no-op, loop wrap, idle).
 */

#include <jce/middleware/animation/jce_anim_ik.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-3f

static float v3_distance(const float *a, const float *b)
{
    float dx = a[0] - b[0];
    float dy = a[1] - b[1];
    float dz = a[2] - b[2];
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* ── IK ─────────────────────────────────────────────────────────────── */

static void test_ik_null_inputs_are_safe(void)
{
    JceIkTwoBoneOutput out;
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_two_bone_solve(NULL, &out));
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_two_bone_solve(
        &(JceIkTwoBoneInput){0}, NULL));
}

static void test_ik_preserves_bone_lengths_when_reachable(void)
{
    /* Two unit bones along Y; target within reach. */
    JceIkTwoBoneInput in = {
        .root_pos = {0, 0, 0},
        .mid_pos  = {0, 1, 0},
        .end_pos  = {0, 2, 0},
        .target   = {1.0f, 1.0f, 0},
        .pole     = {0, 1, 1},
        .weight   = 1.0f,
    };
    JceIkTwoBoneOutput out;
    int reached = jce_anim_ik_two_bone_solve(&in, &out);
    TEST_ASSERT_EQUAL_INT(1, reached);

    float l1 = v3_distance(in.root_pos, out.mid_pos);
    float l2 = v3_distance(out.mid_pos, out.end_pos);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, l1);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, l2);

    /* End effector lands on the target. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, in.target[0], out.end_pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, in.target[1], out.end_pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, in.target[2], out.end_pos[2]);
}

static void test_ik_clamps_when_target_out_of_reach(void)
{
    /* Bones of length 1 + 1, target at distance 5 — unreachable. */
    JceIkTwoBoneInput in = {
        .root_pos = {0, 0, 0},
        .mid_pos  = {0, 1, 0},
        .end_pos  = {0, 2, 0},
        .target   = {5, 0, 0},
        .pole     = {0, 1, 0},
        .weight   = 1.0f,
    };
    JceIkTwoBoneOutput out;
    int reached = jce_anim_ik_two_bone_solve(&in, &out);
    TEST_ASSERT_EQUAL_INT(0, reached);

    /* End must be at distance L1+L2 = 2 from root, pointing along x. */
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 2.0f, v3_distance(in.root_pos, out.end_pos));
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 1.0f, v3_distance(in.root_pos, out.mid_pos));
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 1.0f, v3_distance(out.mid_pos, out.end_pos));
}

static void test_ik_weight_zero_keeps_input_pose(void)
{
    JceIkTwoBoneInput in = {
        .root_pos = {0, 0, 0},
        .mid_pos  = {0, 1, 0},
        .end_pos  = {0, 2, 0},
        .target   = {1.0f, 1.0f, 0},
        .pole     = {0, 1, 1},
        .weight   = 0.0f,
    };
    JceIkTwoBoneOutput out;
    jce_anim_ik_two_bone_solve(&in, &out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, in.mid_pos[1], out.mid_pos[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, in.end_pos[1], out.end_pos[1]);
}

static void test_ik_weight_clamped_above_one(void)
{
    JceIkTwoBoneInput a = {
        .root_pos = {0, 0, 0},
        .mid_pos  = {0, 1, 0},
        .end_pos  = {0, 2, 0},
        .target   = {1.2f, 1.4f, 0.1f},
        .pole     = {0, 1, 0},
        .weight   = 1.0f,
    };
    JceIkTwoBoneInput b = a;
    b.weight = 5.0f;
    JceIkTwoBoneOutput oa, ob;
    jce_anim_ik_two_bone_solve(&a, &oa);
    jce_anim_ik_two_bone_solve(&b, &ob);
    TEST_ASSERT_FLOAT_WITHIN(EPS, oa.end_pos[0], ob.end_pos[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, oa.end_pos[1], ob.end_pos[1]);
}

/* ── Events ─────────────────────────────────────────────────────────── */

static int   g_fired_count;
static uint32_t g_fired_ids[16];

static void on_event(const JceAnimEvent *ev, void *user)
{
    (void)user;
    if (g_fired_count < 16) g_fired_ids[g_fired_count] = ev->id;
    g_fired_count++;
}

static JceAnimEvent g_track[4];

static JceAnimEventTrack make_track(float duration)
{
    g_track[0] = (JceAnimEvent){ .time = 0.25f, .id = 10 };
    g_track[1] = (JceAnimEvent){ .time = 0.50f, .id = 20 };
    g_track[2] = (JceAnimEvent){ .time = 0.75f, .id = 30 };
    g_track[3] = (JceAnimEvent){ .time = 0.95f, .id = 40 };
    JceAnimEventTrack t = {
        .events = g_track, .count = 4, .clip_duration = duration
    };
    return t;
}

static void test_events_forward_window_fires_in_order(void)
{
    g_fired_count = 0;
    JceAnimEventTrack t = make_track(1.0f);
    jce_anim_events_advance(&t, 0.20f, 0.80f, on_event, NULL);
    TEST_ASSERT_EQUAL_INT(3, g_fired_count);
    TEST_ASSERT_EQUAL_UINT32(10, g_fired_ids[0]);
    TEST_ASSERT_EQUAL_UINT32(20, g_fired_ids[1]);
    TEST_ASSERT_EQUAL_UINT32(30, g_fired_ids[2]);
}

static void test_events_no_op_when_times_equal(void)
{
    g_fired_count = 0;
    JceAnimEventTrack t = make_track(1.0f);
    jce_anim_events_advance(&t, 0.50f, 0.50f, on_event, NULL);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
}

static void test_events_loop_wrap_fires_both_segments(void)
{
    g_fired_count = 0;
    JceAnimEventTrack t = make_track(1.0f);
    /* Wrapped: prev=0.80 -> end of clip -> 0.30 */
    jce_anim_events_advance(&t, 0.80f, 0.30f, on_event, NULL);
    TEST_ASSERT_EQUAL_INT(2, g_fired_count);
    TEST_ASSERT_EQUAL_UINT32(40, g_fired_ids[0]); /* tail */
    TEST_ASSERT_EQUAL_UINT32(10, g_fired_ids[1]); /* head */
}

static void test_events_null_or_empty_safe(void)
{
    g_fired_count = 0;
    JceAnimEventTrack empty = { .events = NULL, .count = 0, .clip_duration = 1.0f };
    jce_anim_events_advance(NULL, 0.0f, 1.0f, on_event, NULL);
    jce_anim_events_advance(&empty, 0.0f, 1.0f, on_event, NULL);
    JceAnimEventTrack t = make_track(1.0f);
    jce_anim_events_advance(&t, 0.0f, 1.0f, NULL, NULL);
    TEST_ASSERT_EQUAL_INT(0, g_fired_count);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ik_null_inputs_are_safe);
    RUN_TEST(test_ik_preserves_bone_lengths_when_reachable);
    RUN_TEST(test_ik_clamps_when_target_out_of_reach);
    RUN_TEST(test_ik_weight_zero_keeps_input_pose);
    RUN_TEST(test_ik_weight_clamped_above_one);
    RUN_TEST(test_events_forward_window_fires_in_order);
    RUN_TEST(test_events_no_op_when_times_equal);
    RUN_TEST(test_events_loop_wrap_fires_both_segments);
    RUN_TEST(test_events_null_or_empty_safe);
    return UNITY_END();
}
