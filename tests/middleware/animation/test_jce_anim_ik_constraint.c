/* test_jce_anim_ik_constraint.c
 *
 * Pure-math unit tests for the single-target rigging-IK constraint helpers
 * added when wiring kinds 2/3/4 (MultiParent/Position/Rotation):
 *
 *   - jce_anim_ik_position_solve  (lerp current position -> target by weight)
 *   - jce_anim_ik_rotation_solve  (shortest-arc slerp current -> target, unit)
 *
 * Both helpers are state-free POD-float blends with no scene/skeleton
 * dependency.  We verify the documented contracts:
 *
 *   Position:  weight 0 -> current, 1 -> target, 0.5 -> midpoint; non-finite
 *              weight is treated as 0 (keep current); NULL-safe.
 *   Rotation:  weight 0 -> current (normalised), 1 -> target (normalised),
 *              0.5 -> halfway; result is ALWAYS unit length; the slerp picks
 *              the SHORT way for quats more than 180 deg apart; a zero-length
 *              / non-finite quaternion degrades to identity (no NaN).
 *
 * Headless; depends only on the public animation header + libm + Unity.
 */

#include <jce/middleware/animation/jce_anim_ik.h>

#include <math.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

/* ── local quaternion helpers (independent of engine math) ──────────────── */

/* Build a unit quaternion (x,y,z,w) from an axis + angle (radians). */
static void quat_axis_angle(float out[4], float ax, float ay, float az,
                            float angle)
{
    float al = sqrtf(ax*ax + ay*ay + az*az);
    if (al < 1e-8f) { out[0]=out[1]=out[2]=0.0f; out[3]=1.0f; return; }
    ax/=al; ay/=al; az/=al;
    float h = angle * 0.5f;
    float s = sinf(h);
    out[0] = ax*s; out[1] = ay*s; out[2] = az*s; out[3] = cosf(h);
}

static float quat_len(const float q[4])
{
    return sqrtf(q[0]*q[0] + q[1]*q[1] + q[2]*q[2] + q[3]*q[3]);
}

/* |dot| of two unit quaternions: 1 when they represent the SAME rotation
 * (double-cover: q and -q are equal), independent of sign. */
static float quat_abs_dot(const float a[4], const float b[4])
{
    float d = a[0]*b[0] + a[1]*b[1] + a[2]*b[2] + a[3]*b[3];
    return d < 0.0f ? -d : d;
}

static int q_has_nan(const float q[4])
{
    return isnan(q[0]) || isnan(q[1]) || isnan(q[2]) || isnan(q[3]) ||
           isinf(q[0]) || isinf(q[1]) || isinf(q[2]) || isinf(q[3]);
}

static int v3_has_nan(const float v[3])
{
    return isnan(v[0]) || isnan(v[1]) || isnan(v[2]) ||
           isinf(v[0]) || isinf(v[1]) || isinf(v[2]);
}

/* ── Position solve ─────────────────────────────────────────────────────── */

static void test_position_weight_zero_keeps_current(void)
{
    float cur[3] = {1.0f, 2.0f, 3.0f};
    float tgt[3] = {9.0f, 8.0f, 7.0f};
    float out[3] = {-1.0f, -1.0f, -1.0f};
    jce_anim_ik_position_solve(cur, tgt, 0.0f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, out[2]);
}

static void test_position_weight_one_reaches_target(void)
{
    float cur[3] = {1.0f, 2.0f, 3.0f};
    float tgt[3] = {9.0f, 8.0f, 7.0f};
    float out[3];
    jce_anim_ik_position_solve(cur, tgt, 1.0f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 9.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 8.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 7.0f, out[2]);
}

static void test_position_weight_half_is_midpoint(void)
{
    float cur[3] = {0.0f, 0.0f, 0.0f};
    float tgt[3] = {4.0f, -2.0f, 10.0f};
    float out[3];
    jce_anim_ik_position_solve(cur, tgt, 0.5f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f,  out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -1.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 5.0f,  out[2]);
}

/* Weight clamps to [0,1]; a >1 weight does NOT overshoot the target. */
static void test_position_weight_clamps_above_one(void)
{
    float cur[3] = {0.0f, 0.0f, 0.0f};
    float tgt[3] = {10.0f, 0.0f, 0.0f};
    float out[3];
    jce_anim_ik_position_solve(cur, tgt, 5.0f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 10.0f, out[0]);
}

/* A non-finite weight is treated as 0 (keep current) — never NaN. */
static void test_position_nonfinite_weight_keeps_current(void)
{
    float cur[3] = {1.0f, 2.0f, 3.0f};
    float tgt[3] = {9.0f, 8.0f, 7.0f};
    float out[3];
    jce_anim_ik_position_solve(cur, tgt, NAN, out);
    TEST_ASSERT_FALSE(v3_has_nan(out));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 2.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 3.0f, out[2]);
}

static void test_position_null_safe(void)
{
    float out[3] = {5, 5, 5};
    float cur[3] = {1, 2, 3};
    float tgt[3] = {7, 8, 9};
    /* NULL out -> no crash, nothing else asserted. */
    jce_anim_ik_position_solve(cur, tgt, 0.5f, NULL);
    /* NULL cur -> snaps to target. */
    jce_anim_ik_position_solve(NULL, tgt, 0.5f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 7.0f, out[0]);
    /* NULL target -> keeps current. */
    jce_anim_ik_position_solve(cur, NULL, 0.5f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
}

/* ── Rotation solve ─────────────────────────────────────────────────────── */

static void test_rotation_weight_zero_keeps_current(void)
{
    float cur[4]; quat_axis_angle(cur, 0, 0, 1, 0.7f);
    float tgt[4]; quat_axis_angle(tgt, 0, 1, 0, 2.0f);
    float out[4];
    jce_anim_ik_rotation_solve(cur, tgt, 0.0f, out);
    /* Same rotation as the (normalised) current quaternion. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quat_abs_dot(out, cur));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quat_len(out));
}

static void test_rotation_weight_one_reaches_target(void)
{
    float cur[4]; quat_axis_angle(cur, 0, 0, 1, 0.3f);
    float tgt[4]; quat_axis_angle(tgt, 1, 0, 0, 1.1f);
    float out[4];
    jce_anim_ik_rotation_solve(cur, tgt, 1.0f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quat_abs_dot(out, tgt));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quat_len(out));
}

/* Halfway between two rotations about the SAME axis is the half-angle
 * rotation; result must be unit length. */
static void test_rotation_weight_half_is_halfway(void)
{
    float cur[4]; quat_axis_angle(cur, 0, 1, 0, 0.0f);    /* identity */
    float tgt[4]; quat_axis_angle(tgt, 0, 1, 0, 1.6f);    /* 1.6 rad about Y */
    float exp[4]; quat_axis_angle(exp, 0, 1, 0, 0.8f);    /* the midpoint */
    float out[4];
    jce_anim_ik_rotation_solve(cur, tgt, 0.5f, out);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, quat_len(out));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_abs_dot(out, exp));
}

/* Result is ALWAYS unit length even for arbitrary (already-unit) inputs. */
static void test_rotation_result_always_unit(void)
{
    float cur[4]; quat_axis_angle(cur, 1, 2, 3, 0.9f);
    float tgt[4]; quat_axis_angle(tgt, -2, 1, 0.5f, 2.4f);
    for (int i = 0; i <= 10; ++i) {
        float out[4];
        jce_anim_ik_rotation_solve(cur, tgt, (float)i / 10.0f, out);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_len(out));
        TEST_ASSERT_FALSE(q_has_nan(out));
    }
}

/* Shortest-arc: two rotations more than 180 deg apart (about the same axis)
 * must blend the SHORT way.  cur = +170 deg about Z, tgt = -170 deg about Z
 * (i.e. +190 deg); the short path crosses +180 deg, so the half-blend is at
 * +180 deg about Z, NOT at 0 deg (the long way).  We assert the half-blend is
 * far from identity (the long-way midpoint) and equals the +180 deg rotation. */
static void test_rotation_shortest_arc(void)
{
    const float D2R = 3.14159265358979323846f / 180.0f;
    float cur[4]; quat_axis_angle(cur, 0, 0, 1,  170.0f * D2R);
    float tgt[4]; quat_axis_angle(tgt, 0, 0, 1, -170.0f * D2R);

    float out[4];
    jce_anim_ik_rotation_solve(cur, tgt, 0.5f, out);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_len(out));

    /* Short-way midpoint = 180 deg about Z. */
    float expect_short[4]; quat_axis_angle(expect_short, 0, 0, 1, 180.0f * D2R);
    /* Long-way midpoint would be identity (0 deg). */
    float identity[4] = {0, 0, 0, 1};

    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 1.0f, quat_abs_dot(out, expect_short));
    /* And it is decidedly NOT the long-way (identity) result. */
    TEST_ASSERT_TRUE(quat_abs_dot(out, identity) < 0.1f);
}

/* A zero-length / non-finite quaternion degrades to identity — no NaN. */
static void test_rotation_zero_quat_safe(void)
{
    float zero[4] = {0, 0, 0, 0};
    float good[4]; quat_axis_angle(good, 0, 1, 0, 1.0f);
    float out[4];

    /* zero current -> identity blended toward good. */
    jce_anim_ik_rotation_solve(zero, good, 1.0f, out);
    TEST_ASSERT_FALSE(q_has_nan(out));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_len(out));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_abs_dot(out, good));

    /* zero target -> good blended toward identity. */
    jce_anim_ik_rotation_solve(good, zero, 0.0f, out);
    TEST_ASSERT_FALSE(q_has_nan(out));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_abs_dot(out, good));

    /* both zero -> identity. */
    float identity[4] = {0, 0, 0, 1};
    jce_anim_ik_rotation_solve(zero, zero, 0.5f, out);
    TEST_ASSERT_FALSE(q_has_nan(out));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_abs_dot(out, identity));
}

static void test_rotation_nonfinite_weight_keeps_current(void)
{
    float cur[4]; quat_axis_angle(cur, 0, 0, 1, 0.5f);
    float tgt[4]; quat_axis_angle(tgt, 1, 0, 0, 2.0f);
    float out[4];
    jce_anim_ik_rotation_solve(cur, tgt, NAN, out);
    TEST_ASSERT_FALSE(q_has_nan(out));
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, quat_abs_dot(out, cur));
}

static void test_rotation_null_out_safe(void)
{
    float cur[4]; quat_axis_angle(cur, 0, 0, 1, 0.5f);
    float tgt[4]; quat_axis_angle(tgt, 1, 0, 0, 2.0f);
    /* NULL out -> no crash. */
    jce_anim_ik_rotation_solve(cur, tgt, 0.5f, NULL);
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();
    /* Position */
    RUN_TEST(test_position_weight_zero_keeps_current);
    RUN_TEST(test_position_weight_one_reaches_target);
    RUN_TEST(test_position_weight_half_is_midpoint);
    RUN_TEST(test_position_weight_clamps_above_one);
    RUN_TEST(test_position_nonfinite_weight_keeps_current);
    RUN_TEST(test_position_null_safe);
    /* Rotation */
    RUN_TEST(test_rotation_weight_zero_keeps_current);
    RUN_TEST(test_rotation_weight_one_reaches_target);
    RUN_TEST(test_rotation_weight_half_is_halfway);
    RUN_TEST(test_rotation_result_always_unit);
    RUN_TEST(test_rotation_shortest_arc);
    RUN_TEST(test_rotation_zero_quat_safe);
    RUN_TEST(test_rotation_nonfinite_weight_keeps_current);
    RUN_TEST(test_rotation_null_out_safe);
    return UNITY_END();
}
