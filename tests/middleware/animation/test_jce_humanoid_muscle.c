/*
 * test_jce_humanoid_muscle.c -- the muscle range, and whether it fires only
 * when it should.
 *
 * A retarget transports the rotation the animation ADDED, which is the right
 * thing and is also unbounded: a source rig whose knee the animator bent 120
 * degrees the wrong way hands the target 120 degrees the wrong way.  The
 * clamp is what bounds it.
 *
 * A clamp is easy to write and easy to get wrong in the direction that does
 * not show: one that fires on everything quietly re-quantises every pose in
 * the project, and one that fires on nothing is a feature nobody can tell
 * from its own absence.  So the assertions come in pairs -- something that
 * MUST be clamped and something that MUST come back bit-for-bit.
 */
#include "unity.h"

#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_skeleton.h>

#include <math.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

#define DEG2RAD 0.017453292519943295f
#define RAD2DEG 57.29577951308232f

static void joint(JceJoint *j, const char *name, int parent, jce_vec3 t)
{
    memset(j, 0, sizeof(*j));
    strncpy(j->name, name, sizeof(j->name) - 1);
    j->parent = (int16_t)parent;
    j->rest_translation = t;
    j->rest_rotation = jce_q_identity();
    j->rest_scale = jce_v3(1.0f, 1.0f, 1.0f);
    j->inverse_bind_matrix = jce_m4_identity();
    j->local_transform = jce_m4_identity();
}

static jce_quat about(jce_vec3 a, float deg)
{
    const float h = deg * 0.5f * DEG2RAD, s = sinf(h);
    jce_quat q; q.x = a.x * s; q.y = a.y * s; q.z = a.z * s; q.w = cosf(h);
    return jce_q_normalize(q);
}

static float angle_deg(jce_quat q)
{
    float w = fabsf(q.w);
    if (w > 1.0f) w = 1.0f;
    return 2.0f * acosf(w) * RAD2DEG;
}

/* A rig with LEGS, because the interesting limits are on a knee.  Straight
 * down the -Y axis, Unreal names so the mapper recognises them. */
static JceSkeleton *make_rig(void)
{
    JceJoint j[6];
    joint(&j[0], "pelvis",   -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1], "spine_01",  0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2], "neck_01",   1, jce_v3(0.0f,  0.3f, 0.0f));
    joint(&j[3], "thigh_l",   0, jce_v3(0.1f,  0.0f, 0.0f));
    joint(&j[4], "calf_l",    3, jce_v3(0.0f, -0.45f, 0.0f));
    joint(&j[5], "foot_l",    4, jce_v3(0.0f, -0.45f, 0.0f));
    return jce_skeleton_create(j, 6);
}

/* ── the axis comes from the rig, not from a convention ───────────────── */

static void test_the_bone_axis_points_down_the_bone(void)
{
    JceSkeleton *sk = make_rig();
    TEST_ASSERT_NOT_NULL(sk);
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));

    jce_vec3 a;
    TEST_ASSERT_TRUE(jce_humanoid_bone_axis(&m, sk, JCE_HB_LEFT_UPPER_LEG, &a));
    /* thigh -> knee is straight down in this rig. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, a.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, -1.0f, a.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f,  0.0f, a.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f,
                             sqrtf(a.x * a.x + a.y * a.y + a.z * a.z));
    jce_skeleton_destroy(sk);
}

static void test_a_tip_has_no_axis_and_says_so(void)
{
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_vec3 a;
    /* The foot's child role (toes) is not in this rig, so there is nothing to
     * take a direction from.  A made-up axis would clamp the wrong component
     * and nothing downstream could attribute the result. */
    TEST_ASSERT_FALSE(jce_humanoid_bone_axis(&m, sk, JCE_HB_LEFT_FOOT, &a));
    /* And a role that is a tip by definition. */
    TEST_ASSERT_FALSE(jce_humanoid_bone_axis(&m, sk, JCE_HB_HEAD, &a));
    jce_skeleton_destroy(sk);
}

/* ── the clamp itself ─────────────────────────────────────────────────── */

static void test_a_delta_inside_the_range_comes_back_bit_for_bit(void)
{
    /* THE HALF THAT IS EASY TO LOSE.  A clamp that recomposes every rotation
     * "just in case" changes poses nobody asked it to change, and does it by
     * a fraction of a degree per frame where no test looking for a big
     * difference would see it.  Identity, exactly. */
    const jce_vec3 axis = jce_v3(0.0f, -1.0f, 0.0f);
    JceHumanoidMuscleLimits L;
    TEST_ASSERT_TRUE(jce_humanoid_muscle_limits(JCE_HB_LEFT_LOWER_LEG, &L));

    const jce_quat twist = about(axis, L.twist_max * 0.5f);
    const jce_quat out =
        jce_humanoid_clamp_delta(JCE_HB_LEFT_LOWER_LEG, axis, twist);
    TEST_ASSERT_EQUAL_FLOAT(twist.x, out.x);
    TEST_ASSERT_EQUAL_FLOAT(twist.y, out.y);
    TEST_ASSERT_EQUAL_FLOAT(twist.z, out.z);
    TEST_ASSERT_EQUAL_FLOAT(twist.w, out.w);
}

static void test_twist_past_the_limit_lands_on_the_limit(void)
{
    const jce_vec3 axis = jce_v3(0.0f, -1.0f, 0.0f);
    JceHumanoidMuscleLimits L;
    TEST_ASSERT_TRUE(jce_humanoid_muscle_limits(JCE_HB_LEFT_LOWER_LEG, &L));

    /* A knee twisted 120 degrees about its own length: a shin does not do
     * this, and a name-matched retarget from a rig that rolls its bones the
     * other way asks for exactly it. */
    const jce_quat too_much = about(axis, 120.0f);
    const jce_quat out =
        jce_humanoid_clamp_delta(JCE_HB_LEFT_LOWER_LEG, axis, too_much);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, L.twist_max, angle_deg(out));

    /* And the same the other way, against twist_min rather than |twist_max|. */
    const jce_quat too_much_neg = about(axis, -120.0f);
    const jce_quat out_neg =
        jce_humanoid_clamp_delta(JCE_HB_LEFT_LOWER_LEG, axis, too_much_neg);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, -L.twist_min, angle_deg(out_neg));
}

static void test_swing_past_the_cone_lands_on_the_cone(void)
{
    const jce_vec3 axis = jce_v3(0.0f, -1.0f, 0.0f);
    JceHumanoidMuscleLimits L;
    TEST_ASSERT_TRUE(jce_humanoid_muscle_limits(JCE_HB_LEFT_LOWER_LEG, &L));

    /* 150 degrees about X -- perpendicular to the bone, so pure swing. */
    const jce_quat too_much = about(jce_v3(1.0f, 0.0f, 0.0f), 150.0f);
    const jce_quat out =
        jce_humanoid_clamp_delta(JCE_HB_LEFT_LOWER_LEG, axis, too_much);
    TEST_ASSERT_FLOAT_WITHIN(0.5f, L.swing_max, angle_deg(out));
}

static void test_the_limits_differ_by_role(void)
{
    /* If every role had the same numbers the table would be decoration.  A
     * shoulder is the tightest thing in the set and an upper arm the
     * loosest; that ordering is the claim. */
    JceHumanoidMuscleLimits sh, ua;
    TEST_ASSERT_TRUE(jce_humanoid_muscle_limits(JCE_HB_LEFT_SHOULDER, &sh));
    TEST_ASSERT_TRUE(jce_humanoid_muscle_limits(JCE_HB_LEFT_UPPER_ARM, &ua));
    TEST_ASSERT_TRUE(sh.swing_max < ua.swing_max);
    TEST_ASSERT_TRUE(sh.twist_max < ua.twist_max);

    JceHumanoidMuscleLimits bad;
    TEST_ASSERT_FALSE(jce_humanoid_muscle_limits(JCE_HB_COUNT, &bad));
}

/* ── and whether the retarget actually uses it ────────────────────────── */

static void retarget_pair(float knee_twist_deg,
                          jce_quat *out_off, jce_quat *out_on,
                          uint32_t *n_clamped)
{
    JceSkeleton *src = make_rig();
    JceSkeleton *dst = make_rig();
    JceHumanoidMap ms, md;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(src, &ms));
    TEST_ASSERT_TRUE(jce_humanoid_map_build(dst, &md));

    const uint32_t sn = jce_skeleton_joint_count(src);
    const uint32_t dn = jce_skeleton_joint_count(dst);
    jce_quat spose[8], off[8], on[8];
    for (uint32_t i = 0; i < sn; i++) spose[i] = jce_q_identity();
    spose[(uint32_t)ms.joint[JCE_HB_LEFT_LOWER_LEG]] =
        about(jce_v3(0.0f, -1.0f, 0.0f), knee_twist_deg);

    jce_humanoid_rest_locals(dst, off);
    jce_humanoid_rest_locals(dst, on);

    uint32_t dummy = 0;
    jce_humanoid_retarget_clamped(&ms, src, spose, &md, dst, off, NULL,
                                  false, &dummy);
    TEST_ASSERT_EQUAL_UINT32(0, dummy);   /* off means off */
    jce_humanoid_retarget_clamped(&ms, src, spose, &md, dst, on, NULL,
                                  true, n_clamped);

    const uint32_t k = (uint32_t)md.joint[JCE_HB_LEFT_LOWER_LEG];
    TEST_ASSERT_TRUE(k < dn);
    *out_off = off[k];
    *out_on  = on[k];
    jce_skeleton_destroy(src);
    jce_skeleton_destroy(dst);
}

static void test_the_retarget_clamps_a_knee_that_needs_it(void)
{
    jce_quat off, on; uint32_t n = 0;
    retarget_pair(120.0f, &off, &on, &n);
    TEST_ASSERT_EQUAL_UINT32(1, n);
    /* 120 in, 30 out (the knee's twist_max). */
    TEST_ASSERT_FLOAT_WITHIN(0.5f, 120.0f, angle_deg(off));
    TEST_ASSERT_FLOAT_WITHIN(0.5f,  30.0f, angle_deg(on));
}

static void test_the_retarget_leaves_a_pose_inside_the_range_alone(void)
{
    /* THE CONTROL.  Without it the test above passes for a clamp that
     * flattens everything, and a project's whole animation set would come
     * back subtly different with nothing saying why. */
    jce_quat off, on; uint32_t n = 0;
    retarget_pair(20.0f, &off, &on, &n);
    TEST_ASSERT_EQUAL_UINT32(0, n);
    TEST_ASSERT_EQUAL_FLOAT(off.x, on.x);
    TEST_ASSERT_EQUAL_FLOAT(off.y, on.y);
    TEST_ASSERT_EQUAL_FLOAT(off.z, on.z);
    TEST_ASSERT_EQUAL_FLOAT(off.w, on.w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_bone_axis_points_down_the_bone);
    RUN_TEST(test_a_tip_has_no_axis_and_says_so);
    RUN_TEST(test_a_delta_inside_the_range_comes_back_bit_for_bit);
    RUN_TEST(test_twist_past_the_limit_lands_on_the_limit);
    RUN_TEST(test_swing_past_the_cone_lands_on_the_cone);
    RUN_TEST(test_the_limits_differ_by_role);
    RUN_TEST(test_the_retarget_clamps_a_knee_that_needs_it);
    RUN_TEST(test_the_retarget_leaves_a_pose_inside_the_range_alone);
    return UNITY_END();
}
