/*
 * test_jce_humanoid_ik.c -- two-bone IK, and whether the hand actually lands.
 *
 * An IK solver has one job and two ways to fail quietly.  It can MISS -- the
 * end effector ends up near the target and a viewer calls it "close enough"
 * -- and it can move joints it was never asked to move, which shows up three
 * weeks later as a shoulder that drifts whenever a hand is pinned.  So every
 * reach assertion here is paired with one about what must NOT have changed.
 *
 * The rig is deliberately measured in whole numbers: upper 0.5, lower 0.5, so
 * reach is exactly 1.0 and the triangle cases can be written down rather than
 * read off the solver.
 */
#include "unity.h"

#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_skeleton.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void) {}
void tearDown(void) {}

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

/* An arm of two 0.5 bones hanging down -Y from the shoulder, Unreal names so
 * jce_humanoid_map_build recognises the roles. */
#define NJ 5
static JceSkeleton *make_rig(void)
{
    JceJoint j[NJ];
    joint(&j[0], "pelvis",      -1, jce_v3(0.0f, 1.0f, 0.0f));
    joint(&j[1], "spine_01",     0, jce_v3(0.0f, 0.4f, 0.0f));
    joint(&j[2], "upperarm_l",   1, jce_v3(0.2f, 0.0f, 0.0f));
    joint(&j[3], "lowerarm_l",   2, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[4], "hand_l",       3, jce_v3(0.0f, -0.5f, 0.0f));
    return jce_skeleton_create(j, NJ);
}

static void rest_locals(jce_quat *q)
{
    for (int i = 0; i < NJ; i++) q[i] = jce_q_identity();
}

/* Model-space position of a joint under `local`, computed HERE rather than
 * by calling the engine's own walk: a test that measures the result with the
 * function under test agrees with itself no matter what either one does. */
static jce_vec3 model_pos(const JceSkeleton *sk, const jce_quat *local, int idx)
{
    const jce_vec3 *t = NULL, *s = NULL;
    const jce_quat *r = NULL;
    jce_skeleton_rest_trs(sk, &t, &r, &s);
    int chain[16], n = 0, cur = idx;
    while (cur >= 0 && n < 16) { chain[n++] = cur; cur = jce_skeleton_joint_parent(sk, (uint32_t)cur); }
    jce_quat rot = jce_q_identity();
    jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
    for (int i = n - 1; i >= 0; i--) {
        const int j = chain[i];
        const jce_vec3 off = jce_q_rotate(rot, t[j]);
        p.x += off.x; p.y += off.y; p.z += off.z;
        rot = jce_q_multiply(rot, local[j]);
    }
    return p;
}

static float dist(jce_vec3 a, jce_vec3 b)
{
    const jce_vec3 d = jce_v3_sub(a, b);
    return sqrtf(d.x * d.x + d.y * d.y + d.z * d.z);
}

static int same_quat(jce_quat a, jce_quat b)
{
    return a.x == b.x && a.y == b.y && a.z == b.z && a.w == b.w;
}

/* ── it reaches ───────────────────────────────────────────────────────── */

static void test_the_hand_lands_on_a_reachable_target(void)
{
    JceSkeleton *sk = make_rig();
    TEST_ASSERT_NOT_NULL(sk);
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));

    jce_quat local[NJ];
    rest_locals(local);
    const jce_vec3 shoulder = model_pos(sk, local, 2);

    /* 0.8 of the 1.0 reach, forward and down: a real triangle, both joints
     * have to move. */
    const jce_vec3 target = jce_v3(shoulder.x, shoulder.y - 0.5f, shoulder.z + 0.6f);

    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, NULL, 1.0f, local));
    /* 1e-4 of a rig whose bones are 0.5 long: a miss an animator could see
     * is orders of magnitude bigger than this. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(model_pos(sk, local, 4), target));
    jce_skeleton_destroy(sk);
}

static void test_it_reaches_targets_all_around_the_shoulder(void)
{
    /* One target is an anecdote.  A solver can be right on the plane it was
     * debugged in and wrong behind the shoulder. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat rest[NJ];
    rest_locals(rest);
    const jce_vec3 sh = model_pos(sk, rest, 2);

    static const float dirs[8][3] = {
        { 1, 0, 0}, {-1, 0, 0}, {0, 0, 1}, {0, 0,-1},
        { 0,-1, 0}, { 0.6f,-0.6f, 0.5f}, {-0.5f,-0.7f,-0.5f}, {0.3f, 0.8f, 0.5f},
    };
    for (int i = 0; i < 8; i++) {
        jce_quat local[NJ];
        rest_locals(local);
        const jce_vec3 d = jce_v3_normalize(jce_v3(dirs[i][0], dirs[i][1], dirs[i][2]));
        const jce_vec3 target = jce_v3(sh.x + d.x * 0.75f,
                                       sh.y + d.y * 0.75f,
                                       sh.z + d.z * 0.75f);
        TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                                  target, NULL, 1.0f, local));
        char msg[96];
        snprintf(msg, sizeof msg, "direction %d = (%.2f, %.2f, %.2f)",
                 i, (double)d.x, (double)d.y, (double)d.z);
        TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 0.0f,
                                 dist(model_pos(sk, local, 4), target), msg);
    }
    jce_skeleton_destroy(sk);
}

static void test_an_unreachable_target_points_the_limb_straight_at_it(void)
{
    /* Reach is 1.0; ask for 3.0.  A solver that refused would leave the pose
     * alone and be indistinguishable from one that never ran. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat local[NJ];
    rest_locals(local);
    const jce_vec3 sh = model_pos(sk, local, 2);
    const jce_vec3 target = jce_v3(sh.x, sh.y, sh.z + 3.0f);

    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, NULL, 1.0f, local));
    const jce_vec3 hand = model_pos(sk, local, 4);
    /* Straight: the hand is at full reach along the direction of the target. */
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 1.0f, dist(hand, sh));
    /* And ON that line: elbow, shoulder and hand collinear toward the goal. */
    const jce_vec3 want = jce_v3(sh.x, sh.y, sh.z + 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.0f, dist(hand, want));
    jce_skeleton_destroy(sk);
}

/* ── and it does not move anything else ───────────────────────────────── */

static void test_it_touches_only_the_two_joints_it_solves(void)
{
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat local[NJ], before[NJ];
    rest_locals(local);
    memcpy(before, local, sizeof before);

    const jce_vec3 sh = model_pos(sk, local, 2);
    const jce_vec3 target = jce_v3(sh.x + 0.3f, sh.y - 0.4f, sh.z + 0.5f);
    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, NULL, 1.0f, local));

    /* pelvis, spine and the HAND itself are not the solver's business:
     * a two-bone solve rotates the upper and the lower and nothing else. */
    TEST_ASSERT_TRUE(same_quat(before[0], local[0]));
    TEST_ASSERT_TRUE(same_quat(before[1], local[1]));
    TEST_ASSERT_TRUE(same_quat(before[4], local[4]));
    /* ...and the two it does own really did move, so this test cannot pass
     * by the solver doing nothing at all. */
    TEST_ASSERT_FALSE(same_quat(before[2], local[2]));
    TEST_ASSERT_FALSE(same_quat(before[3], local[3]));
    jce_skeleton_destroy(sk);
}

static void test_weight_zero_is_bit_for_bit_and_still_true(void)
{
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat local[NJ], before[NJ];
    rest_locals(local);
    memcpy(before, local, sizeof before);

    /* TRUE, not false: the limb was solvable and the caller asked for none of
     * it.  False would make "the blend is at zero" read as "this rig has no
     * arm", and a caller ramping IK in would log an error every frame until
     * the blend left zero. */
    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              jce_v3(9.0f, 9.0f, 9.0f), NULL,
                                              0.0f, local));
    for (int i = 0; i < NJ; i++) TEST_ASSERT_TRUE(same_quat(before[i], local[i]));
    jce_skeleton_destroy(sk);
}

static void test_a_partial_weight_lands_between_the_two_poses(void)
{
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));

    jce_quat full[NJ], half[NJ], rest[NJ];
    rest_locals(full); rest_locals(half); rest_locals(rest);
    const jce_vec3 sh = model_pos(sk, rest, 2);
    const jce_vec3 target = jce_v3(sh.x, sh.y - 0.4f, sh.z + 0.6f);

    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, NULL, 1.0f, full));
    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, NULL, 0.5f, half));

    const float d_rest = dist(model_pos(sk, rest, 4), target);
    const float d_half = dist(model_pos(sk, half, 4), target);
    const float d_full = dist(model_pos(sk, full, 4), target);
    /* STRICTLY between, both ways: a weight that silently behaved as 1 would
     * pass "closer than rest", and one that behaved as 0 would pass
     * "no closer than full". */
    TEST_ASSERT_TRUE(d_half < d_rest);
    TEST_ASSERT_TRUE(d_half > d_full);
    jce_skeleton_destroy(sk);
}

/* ── the pole decides which way it bends ──────────────────────────────── */

static void test_the_pole_hint_moves_the_elbow_and_keeps_the_hand(void)
{
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));

    jce_quat rest[NJ], a[NJ], b[NJ];
    rest_locals(rest); rest_locals(a); rest_locals(b);
    const jce_vec3 sh = model_pos(sk, rest, 2);
    const jce_vec3 target = jce_v3(sh.x, sh.y - 0.8f, sh.z + 0.2f);

    const jce_vec3 pole_front = jce_v3(sh.x, sh.y, sh.z + 2.0f);
    const jce_vec3 pole_back  = jce_v3(sh.x, sh.y, sh.z - 2.0f);
    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, &pole_front, 1.0f, a));
    TEST_ASSERT_TRUE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_ARM,
                                              target, &pole_back, 1.0f, b));

    /* Both still reach -- the hint decides the elbow, not the goal. */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(model_pos(sk, a, 4), target));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(model_pos(sk, b, 4), target));
    /* And the elbows are on opposite sides, by a lot rather than by noise. */
    const jce_vec3 ea = model_pos(sk, a, 3);
    const jce_vec3 eb = model_pos(sk, b, 3);
    TEST_ASSERT_TRUE(dist(ea, eb) > 0.1f);
    jce_skeleton_destroy(sk);
}

/* ── what it refuses ──────────────────────────────────────────────────── */

static void test_a_rig_without_the_limb_says_so(void)
{
    /* The rig above has a LEFT arm and no legs.  A solver that returned true
     * here would report success for a limb it never touched. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat local[NJ];
    rest_locals(local);
    TEST_ASSERT_FALSE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_LEFT_LEG,
                                               jce_v3(0.0f, 0.0f, 0.0f), NULL,
                                               1.0f, local));
    TEST_ASSERT_FALSE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_RIGHT_ARM,
                                               jce_v3(0.0f, 0.0f, 0.0f), NULL,
                                               1.0f, local));
    TEST_ASSERT_FALSE(jce_humanoid_ik_two_bone(&m, sk, JCE_HUMANOID_LIMB_COUNT,
                                               jce_v3(0.0f, 0.0f, 0.0f), NULL,
                                               1.0f, local));
    jce_skeleton_destroy(sk);
}

static void test_limb_bones_names_the_three_roles(void)
{
    JceHumanoidBone u, l, e;
    TEST_ASSERT_TRUE(jce_humanoid_limb_bones(JCE_HUMANOID_LIMB_RIGHT_LEG, &u, &l, &e));
    TEST_ASSERT_EQUAL_INT(JCE_HB_RIGHT_UPPER_LEG, u);
    TEST_ASSERT_EQUAL_INT(JCE_HB_RIGHT_LOWER_LEG, l);
    TEST_ASSERT_EQUAL_INT(JCE_HB_RIGHT_FOOT, e);
    TEST_ASSERT_FALSE(jce_humanoid_limb_bones(JCE_HUMANOID_LIMB_COUNT, &u, &l, &e));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_the_hand_lands_on_a_reachable_target);
    RUN_TEST(test_it_reaches_targets_all_around_the_shoulder);
    RUN_TEST(test_an_unreachable_target_points_the_limb_straight_at_it);
    RUN_TEST(test_it_touches_only_the_two_joints_it_solves);
    RUN_TEST(test_weight_zero_is_bit_for_bit_and_still_true);
    RUN_TEST(test_a_partial_weight_lands_between_the_two_poses);
    RUN_TEST(test_the_pole_hint_moves_the_elbow_and_keeps_the_hand);
    RUN_TEST(test_a_rig_without_the_limb_says_so);
    RUN_TEST(test_limb_bones_names_the_three_roles);
    return UNITY_END();
}
