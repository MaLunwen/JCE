/* test_jce_humanoid_limb_goal.c
 *
 * A humanoid-limb IK goal (JceIkConstraint kind 7) names a LIMB and lets the
 * rig's humanoid ROLE map supply the three joints, where kind 1 names the
 * three joints outright.  That is the whole difference, and it is the one
 * Unity's AvatarIKGoal exists for: "pin the left hand" has to work on a rig
 * this scene has never seen, and two humanoid rigs from different tools share
 * no bone names at all.
 *
 * WHAT THIS FILE GUARDS is the chain the renderer walks for that kind:
 *
 *     limb -> jce_humanoid_limb_bones -> three roles
 *          -> JceHumanoidMap.joint[role] -> three joint indices
 *
 * and the requirement that those indices are THE SAME ONES a correct kind-1
 * constraint would have named by hand.  If they ever diverge, a goal authored
 * as "LeftArm" pins something that is not the left arm, and on a symmetric rig
 * that reads as "the IK is weak" rather than as a wrong joint.
 *
 * It is a unit test and not a rendered frame because the two things that can
 * be wrong here -- the role triple and the map lookup -- are both decidable
 * without a renderer, and a frame would only be able to say that something
 * looked off.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_skeleton.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

void setUp(void)    {}
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
    /* local_transform must AGREE with the rest TRS above.  Leaving it identity
     * is permitted by the API and is how the fixture was first written, and it
     * makes jce_skeleton_rest_pose() and jce_skeleton_rest_trs() answer
     * different questions about the same rig -- the exact shape of a defect
     * this tree has already paid for once (the hips height read through one
     * and the rotations through the other).  A fixture that is inconsistent
     * with itself cannot judge code that reads either. */
    j->local_transform = jce_m4_from_trs(t, j->rest_rotation, j->rest_scale);
}

/* Both arms and both legs, Unreal names -- one of the three conventions this
 * tree actually carries in its rigged models, and the one whose sided suffixes
 * are easiest to get backwards. */
#define NJ 11
static JceSkeleton *make_rig(void)
{
    JceJoint j[NJ];
    joint(&j[0],  "pelvis",     -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1],  "spine_01",    0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2],  "upperarm_l",  1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3],  "lowerarm_l",  2, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[4],  "hand_l",      3, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[5],  "upperarm_r",  1, jce_v3(-0.2f, 0.0f, 0.0f));
    joint(&j[6],  "lowerarm_r",  5, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[7],  "hand_r",      6, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[8],  "thigh_l",     0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[9],  "calf_l",      8, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[10], "foot_l",      9, jce_v3(0.0f, -0.5f, 0.0f));
    return jce_skeleton_create(j, NJ);
}

/* A rig with the same SHAPE and names no humanoid convention uses. */
static JceSkeleton *make_anonymous_rig(void)
{
    JceJoint j[NJ];
    joint(&j[0],  "part_0",  -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1],  "part_1",   0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2],  "part_2",   1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3],  "part_3",   2, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[4],  "part_4",   3, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[5],  "part_5",   1, jce_v3(-0.2f, 0.0f, 0.0f));
    joint(&j[6],  "part_6",   5, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[7],  "part_7",   6, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[8],  "part_8",   0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[9],  "part_9",   8, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[10], "part_10",  9, jce_v3(0.0f, -0.5f, 0.0f));
    return jce_skeleton_create(j, NJ);
}

/* The ordinary Blender IK-rig shape: the FOOT is a CONTROL bone parented to
 * the root, not a child of the shin.  PSX_BagMan in this tree is exactly this,
 * and the role matcher is RIGHT to call that joint the left foot -- it is the
 * one the shoe is skinned to.  The chain assumption is violated all the same. */
static JceSkeleton *make_rig_foot_control(void)
{
    JceJoint j[NJ];
    joint(&j[0],  "pelvis",     -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1],  "spine_01",    0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2],  "upperarm_l",  1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3],  "lowerarm_l",  2, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[4],  "hand_l",      3, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[5],  "upperarm_r",  1, jce_v3(-0.2f, 0.0f, 0.0f));
    joint(&j[6],  "lowerarm_r",  5, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[7],  "hand_r",      6, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[8],  "thigh_l",     0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[9],  "calf_l",      8, jce_v3(0.0f, -0.5f, 0.0f));
    /* the control bone: parented to the ROOT, not to calf_l */
    joint(&j[10], "foot_l",      0, jce_v3( 0.1f, -1.0f, 0.0f));
    return jce_skeleton_create(j, NJ);
}

/* A rig with BOTH legs and a head, so the body frame is derivable: the hinge
 * axis of a knee comes from the two hips and the spine, not from the two bones
 * of the leg (which are collinear on a straight rest pose, and that is the
 * blocker this closes). */
#define NJF 9
static JceSkeleton *make_full_rig(void)
{
    JceJoint j[NJF];
    joint(&j[0], "pelvis",   -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1], "spine_01",  0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2], "head",      1, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[3], "thigh_l",   0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[4], "calf_l",    3, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[5], "foot_l",    4, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[6], "thigh_r",   0, jce_v3(-0.1f, 0.0f, 0.0f));
    joint(&j[7], "calf_r",    6, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[8], "foot_r",    7, jce_v3(0.0f, -0.5f, 0.0f));
    return jce_skeleton_create(j, NJF);
}

/* The renderer's resolution for kind 7, written out so the test walks the same
 * two steps rather than asserting against a remembered answer. */
static void resolve_limb(const JceHumanoidMap *m, JceHumanoidLimb limb,
                         int *jr, int *jm, int *je)
{
    JceHumanoidBone up, lo, en;
    *jr = *jm = *je = -1;
    if (!jce_humanoid_limb_bones(limb, &up, &lo, &en)) return;
    *jr = (int)m->joint[up];
    *jm = (int)m->joint[lo];
    *je = (int)m->joint[en];
}

/* ── 1. The goal resolves to the joints a hand-authored triple would ────── */

static void test_limb_resolves_to_the_same_joints_a_name_triple_would(void)
{
    JceSkeleton *sk = make_rig();
    TEST_ASSERT_NOT_NULL(sk);
    JceHumanoidMap m;
    TEST_ASSERT_TRUE_MESSAGE(jce_humanoid_map_build(sk, &m),
        "the role map did not build on a rig with Unreal bone names, so a "
        "humanoid-limb goal has nothing to resolve through");

    struct { JceHumanoidLimb limb; const char *r, *m_, *e; } cases[] = {
        { JCE_HUMANOID_LIMB_LEFT_ARM,  "upperarm_l", "lowerarm_l", "hand_l" },
        { JCE_HUMANOID_LIMB_RIGHT_ARM, "upperarm_r", "lowerarm_r", "hand_r" },
        { JCE_HUMANOID_LIMB_LEFT_LEG,  "thigh_l",    "calf_l",     "foot_l" },
    };
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; i++) {
        int jr, jm, je;
        resolve_limb(&m, cases[i].limb, &jr, &jm, &je);
        TEST_ASSERT_EQUAL_INT_MESSAGE(jce_skeleton_find_joint(sk, cases[i].r),
            jr, "humanoid limb resolved the WRONG root joint");
        TEST_ASSERT_EQUAL_INT_MESSAGE(jce_skeleton_find_joint(sk, cases[i].m_),
            jm, "humanoid limb resolved the WRONG mid joint");
        TEST_ASSERT_EQUAL_INT_MESSAGE(jce_skeleton_find_joint(sk, cases[i].e),
            je, "humanoid limb resolved the WRONG end joint");
    }
    jce_skeleton_destroy(sk);
}

static void test_left_and_right_do_not_resolve_to_each_other(void)
{
    /* The failure a symmetric rig hides: pinning the left hand moves the right
     * arm, and the frame still looks like an arm reaching for something. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    int lr, lm, le, rr, rm, re;
    resolve_limb(&m, JCE_HUMANOID_LIMB_LEFT_ARM,  &lr, &lm, &le);
    resolve_limb(&m, JCE_HUMANOID_LIMB_RIGHT_ARM, &rr, &rm, &re);
    TEST_ASSERT_TRUE(lr >= 0 && rr >= 0);
    TEST_ASSERT_TRUE_MESSAGE(lr != rr && lm != rm && le != re,
        "the two arms resolved to the same joints");
    jce_skeleton_destroy(sk);
}

static void test_the_triple_is_a_parent_chain(void)
{
    /* The analytic two-bone solver assumes root -> mid -> end are consecutive.
     * A map that satisfied the name assertions above while returning three
     * joints that are not a chain would make the solver produce a pose with no
     * error message anywhere. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    for (int l = 0; l < JCE_HUMANOID_LIMB_COUNT; l++) {
        int jr, jm, je;
        resolve_limb(&m, (JceHumanoidLimb)l, &jr, &jm, &je);
        if (jr < 0 || jm < 0 || je < 0) continue;   /* limb absent from rig */
        TEST_ASSERT_EQUAL_INT_MESSAGE(jr, jce_skeleton_joint_parent(sk, jm),
            "the limb's mid joint is not a child of its root");
        TEST_ASSERT_EQUAL_INT_MESSAGE(jm, jce_skeleton_joint_parent(sk, je),
            "the limb's end joint is not a child of its mid");
    }
    jce_skeleton_destroy(sk);
}

/* ── 2. The control: a rig with no roles resolves to nothing ────────────── */

static void test_a_rig_with_no_humanoid_names_resolves_to_nothing(void)
{
    /* This is what makes the constraint INERT rather than wrong on a rig that
     * is not humanoid.  If it came back with plausible indices instead, a
     * goal authored on the wrong entity would silently bend whatever joint 2
     * happens to be -- and the assertions above would still pass, because they
     * only ever look at a rig that does have roles. */
    JceSkeleton *sk = make_anonymous_rig();
    TEST_ASSERT_NOT_NULL(sk);
    JceHumanoidMap m;
    /* The build may succeed or fail; what matters is that no role claims a
     * joint, because that is what the renderer branches on. */
    if (!jce_humanoid_map_build(sk, &m)) { jce_skeleton_destroy(sk); return; }
    for (int l = 0; l < JCE_HUMANOID_LIMB_COUNT; l++) {
        int jr, jm, je;
        resolve_limb(&m, (JceHumanoidLimb)l, &jr, &jm, &je);
        TEST_ASSERT_TRUE_MESSAGE(jr < 0 && jm < 0 && je < 0,
            "a rig whose joints are named part_0..part_10 resolved a humanoid "
            "limb: the goal would bend joints nobody nominated");
    }
    jce_skeleton_destroy(sk);
}

static void test_an_out_of_range_limb_is_refused(void)
{
    JceHumanoidBone up, lo, en;
    TEST_ASSERT_FALSE_MESSAGE(
        jce_humanoid_limb_bones((JceHumanoidLimb)JCE_HUMANOID_LIMB_COUNT,
                                &up, &lo, &en),
        "an out-of-range limb was accepted, so a corrupt or future-authored "
        "constraint would index the role table out of bounds");
}


/* ── 3. The hips height is a DISTANCE, not a Y coordinate ───────────────── */

/* The same rig, built Z-UP -- height along +Z, as exporters ship under a
 * converting root node.  glTF is Y-up by convention and the Khronos CesiumMan
 * sample in this tree is Z-up anyway: its hips joint sits at
 * (1.6e-8, 0.005, 0.679).
 *
 * This fixture exists because the retarget's hips-height ratio read `.y`, so
 * that rig measured 5 MILLIMETRES tall and every root translation crossing
 * onto it was scaled by ~206.  The two fixtures the retarget tests already had
 * are both Y-up, and a defect in an axis assumption cannot be found by a
 * fixture that shares the assumption. */
static JceSkeleton *make_rig_z_up(void)
{
    JceJoint j[NJ];
    joint(&j[0],  "pelvis",     -1, jce_v3(0.0f, 0.0f,  1.0f));
    joint(&j[1],  "spine_01",    0, jce_v3(0.0f, 0.0f,  0.4f));
    joint(&j[2],  "upperarm_l",  1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3],  "lowerarm_l",  2, jce_v3(0.0f, 0.0f, -0.5f));
    joint(&j[4],  "hand_l",      3, jce_v3(0.0f, 0.0f, -0.5f));
    joint(&j[5],  "upperarm_r",  1, jce_v3(-0.2f, 0.0f, 0.0f));
    joint(&j[6],  "lowerarm_r",  5, jce_v3(0.0f, 0.0f, -0.5f));
    joint(&j[7],  "hand_r",      6, jce_v3(0.0f, 0.0f, -0.5f));
    joint(&j[8],  "thigh_l",     0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[9],  "calf_l",      8, jce_v3(0.0f, 0.0f, -0.5f));
    joint(&j[10], "foot_l",      9, jce_v3(0.0f, 0.0f, -0.5f));
    return jce_skeleton_create(j, NJ);
}

static void test_hips_height_survives_a_z_up_rig(void)
{
    JceSkeleton *y = make_rig();
    JceSkeleton *z = make_rig_z_up();
    JceHumanoidMap my, mz;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(y, &my));
    TEST_ASSERT_TRUE(jce_humanoid_map_build(z, &mz));
    printf("  hips_height  Y-up %.4f   Z-up %.4f\n",
           (double)my.hips_height, (double)mz.hips_height);
    /* Same skeleton, one axis apart: the number that scales a retargeted
     * stride must not depend on which one. */
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, my.hips_height, mz.hips_height,
        "the hips height changed when the rig was built along +Z instead of "
        "+Y, so it is reading an axis rather than measuring a distance -- a "
        "retarget onto that rig scales its root translation by the error");
    TEST_ASSERT_TRUE_MESSAGE(mz.hips_height > 0.5f,
        "a Z-up rig measured under half a metre tall: this is the 5 mm answer "
        "the Khronos CesiumMan sample used to produce");
    jce_skeleton_destroy(y);
    jce_skeleton_destroy(z);
}

static void test_hips_height_is_measured_to_the_feet(void)
{
    /* Pins WHICH distance, so the fix cannot drift back into "distance from
     * the origin" -- the same answer only while the feet happen to sit at the
     * origin, which is exactly the coincidence the Y-up fixtures had.
     *
     * The rig's hips are 1.0 above a foot that is also 0.1 to the side, so the
     * answer is sqrt(1 + 0.01) = 1.00499 and NOT 1.0.  That is deliberate: the
     * number scales a retargeted stride, and what a stride scales with is LEG
     * LENGTH, which is the hips-to-foot distance including the offset.  The
     * field is still called hips_height because it crosses the ABI; on a rig
     * standing with its feet under it the two are the same number. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.00499f, m.hips_height,
        "the hips height is not the hips-to-feet distance");
    /* And it is NOT the bare vertical drop, which this rig would report as
     * exactly 1.0 -- the assertion above would pass on either until the
     * tolerance is this tight, so the tolerance is load-bearing. */
    TEST_ASSERT_TRUE(m.hips_height > 1.0f);
    jce_skeleton_destroy(sk);
}

/* ── 4. A non-chain triple is REFUSED, not solved into nonsense ─────────── */

static void test_a_foot_control_bone_is_refused_not_solved(void)
{
    /* The analytic solver rotates upper about its own origin and lower about
     * the mid joint, which only moves the end joint if the end descends from
     * the mid.  On a Blender IK rig the foot is a control bone hanging off the
     * root, so solving anyway rotates the leg while the foot stays where the
     * control put it -- a shoe detached from its own leg, with nothing
     * reporting an error.  Found on PSX_BagMan, in a rendered frame.
     *
     * The contract is refuse-and-leave-alone, so the pose must come back
     * BIT-IDENTICAL: a solver that "mostly" refused would still have written
     * something. */
    JceSkeleton *sk = make_rig_foot_control();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    /* The role matcher must still FIND the foot -- refusing to map it would be
     * a different fix, and the wrong one: that joint is the left foot. */
    TEST_ASSERT_TRUE_MESSAGE(m.joint[JCE_HB_LEFT_FOOT] >= 0,
        "the fixture no longer maps a left foot, so this test cannot reach "
        "the case it was written for");

    jce_quat before[NJ], after[NJ];
    jce_humanoid_rest_locals(sk, before);
    memcpy(after, before, sizeof before);

    const bool ok = jce_humanoid_ik_two_bone(&m, sk,
                        JCE_HUMANOID_LIMB_LEFT_LEG,
                        jce_v3(0.4f, 0.2f, 0.3f), NULL, 1.0f, after);
    TEST_ASSERT_FALSE_MESSAGE(ok,
        "the solver accepted a limb whose end joint is not a child of its mid");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(before, after, sizeof before,
        "the solver refused and still wrote to the pose");

    /* And the control: the ARM of the same rig IS a chain, so it must solve --
     * otherwise this test would pass on a solver that refuses everything. */
    const bool arm_ok = jce_humanoid_ik_two_bone(&m, sk,
                            JCE_HUMANOID_LIMB_LEFT_ARM,
                            jce_v3(0.5f, 1.2f, 0.2f), NULL, 1.0f, after);
    TEST_ASSERT_TRUE_MESSAGE(arm_ok,
        "the arm of the same rig was refused too, so the guard is rejecting "
        "more than non-chains");
    jce_skeleton_destroy(sk);
}

/* ── 5. Twist redistribution moves the twist, not the pose ─────────────── */

static void test_twist_redistribution_does_not_move_the_pose(void)
{
    /* The property the whole thing rests on, and the one that fails silently:
     * the axis runs from the lower joint THROUGH the end joint, so rotating
     * the lower bone about it leaves the end where it was, and taking the same
     * rotation off the end's local leaves its orientation alone.  If that ever
     * stops holding, a project that turns the slider up finds its hands have
     * moved and nothing says why. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));

    jce_quat q[NJ];
    jce_humanoid_rest_locals(sk, q);
    /* Twist the hand about the forearm's own axis -- the candy-wrapper pose. */
    const int hand  = jce_skeleton_find_joint(sk, "hand_l");
    const int fore  = jce_skeleton_find_joint(sk, "lowerarm_l");
    TEST_ASSERT_TRUE(hand >= 0 && fore >= 0);
    jce_vec3 axis;
    TEST_ASSERT_TRUE(jce_humanoid_bone_axis(&m, sk, JCE_HB_LEFT_LOWER_ARM,
                                            &axis));
    const float a = 1.2f;                       /* ~69 degrees */
    const float sh = sinf(a * 0.5f);
    q[hand] = jce_q_normalize(jce_v4(axis.x * sh, axis.y * sh, axis.z * sh,
                                     cosf(a * 0.5f)));

    jce_vec3 hand_before, hand_after;
    jce_quat rot_before, rot_after, fore_before;
    TEST_ASSERT_TRUE(jce_humanoid_role_model_xform(&m, sk, q,
        JCE_HB_LEFT_HAND, &hand_before, &rot_before));
    fore_before = q[fore];

    const uint32_t n = jce_humanoid_redistribute_twist(&m, sk, 0.5f, q);
    TEST_ASSERT_TRUE_MESSAGE(n > 0,
        "no limb was redistributed, so the assertions below compare a pose "
        "with itself");

    TEST_ASSERT_TRUE(jce_humanoid_role_model_xform(&m, sk, q,
        JCE_HB_LEFT_HAND, &hand_after, &rot_after));
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, hand_before.x, hand_after.x,
        "the hand MOVED: redistribution is supposed to be geometry-neutral");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, hand_before.y, hand_after.y,
        "the hand MOVED: redistribution is supposed to be geometry-neutral");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-5f, hand_before.z, hand_after.z,
        "the hand MOVED: redistribution is supposed to be geometry-neutral");
    /* Orientation too -- a hand in the right place facing the wrong way is
     * the same defect one derivative along. */
    const float dot = rot_before.x * rot_after.x + rot_before.y * rot_after.y
                    + rot_before.z * rot_after.z + rot_before.w * rot_after.w;
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f, fabsf(dot),
        "the hand's ORIENTATION changed");

    /* And it actually DID something: the forearm now carries twist it did not
     * carry before.  Without this the three assertions above would pass on a
     * function that returns immediately. */
    const float fdot = fore_before.x * q[fore].x + fore_before.y * q[fore].y
                     + fore_before.z * q[fore].z + fore_before.w * q[fore].w;
    TEST_ASSERT_TRUE_MESSAGE(fabsf(fdot) < 0.999f,
        "the forearm's rotation is unchanged, so no twist was moved onto it");
    jce_skeleton_destroy(sk);
}

static void test_twist_fraction_zero_is_bit_identical(void)
{
    /* A project that never touches the slider must render byte-identically,
     * so 0 has to mean "do nothing", not "recompose and land in the same
     * place to within the last bit". */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat a[NJ], b[NJ];
    jce_humanoid_rest_locals(sk, a);
    const int hand = jce_skeleton_find_joint(sk, "hand_l");
    a[hand] = jce_q_normalize(jce_v4(0.2f, 0.1f, 0.3f, 0.9f));
    memcpy(b, a, sizeof a);
    TEST_ASSERT_EQUAL_UINT32(0, jce_humanoid_redistribute_twist(&m, sk, 0.0f, b));
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(a, b, sizeof a,
        "fraction 0 wrote to the pose");
    jce_skeleton_destroy(sk);
}

static void test_twist_skips_a_foot_control_bone(void)
{
    /* Same guard as the solver's, for the same reason: on a Blender IK rig the
     * foot is not a child of the shin, so moving twist "up" would move the
     * foot.  The ARM of the same rig must still be redistributed, or this
     * would pass on a function that skips everything. */
    JceSkeleton *sk = make_rig_foot_control();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_quat q[NJ];
    jce_humanoid_rest_locals(sk, q);
    const int hand = jce_skeleton_find_joint(sk, "hand_l");
    const int foot = jce_skeleton_find_joint(sk, "foot_l");
    TEST_ASSERT_TRUE(hand >= 0 && foot >= 0);
    q[hand] = jce_q_normalize(jce_v4(0.2f, 0.1f, 0.3f, 0.9f));
    q[foot] = jce_q_normalize(jce_v4(0.2f, 0.1f, 0.3f, 0.9f));
    const jce_quat foot_before = q[foot];
    const uint32_t n = jce_humanoid_redistribute_twist(&m, sk, 1.0f, q);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, n,
        "expected exactly the left arm to be redistributed on this rig");
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&foot_before, &q[foot], sizeof foot_before,
        "the control-bone foot was redistributed anyway");
    jce_skeleton_destroy(sk);
}

/* ── 6. A knee bends one way ─────────────────────────────────────────── */

/* Where a joint sits in MODEL space, accumulated from the rest LOCALS.
 *
 * jce_skeleton_rest_pose() is named "pose" and returns LOCAL matrices -- its
 * docstring says so, and this test read it as model-space anyway.  On the
 * bent-arm rig that turned a hinge axis of (-1,0,0) into (-0.925,0.370,-0.084)
 * and made a correct clamp look like an under-correction to -15.7 degrees.
 * Accumulating is the second view; it must agree with the engine's, which
 * walks the rest TRS instead. */
static jce_vec3 model_pos_from_locals(const JceSkeleton *sk, int joint)
{
    const jce_mat4 *lc = jce_skeleton_rest_pose(sk);
    int chain[32];
    int n = 0;
    for (int cur = joint; cur >= 0 && n < 32;
         cur = jce_skeleton_joint_parent(sk, (uint32_t)cur))
        chain[n++] = cur;
    jce_mat4 m = jce_m4_identity();
    for (int i = n - 1; i >= 0; i--) m = jce_m4_multiply(&m, &lc[chain[i]]);
    return jce_v3(m.raw[3][0], m.raw[3][1], m.raw[3][2]);
}

/* The signed rotation of `q` about `axis`, in degrees. */
static float signed_angle_about(jce_quat q, jce_vec3 axis)
{
    q = jce_q_normalize(q);
    if (q.w < 0.0f) { q.x = -q.x; q.y = -q.y; q.z = -q.z; q.w = -q.w; }
    const float d = q.x * axis.x + q.y * axis.y + q.z * axis.z;
    float a = 2.0f * acosf(q.w > 1.0f ? 1.0f : (q.w < -1.0f ? -1.0f : q.w));
    a *= 57.29577951308232f;
    return (d < 0.0f) ? -a : a;
}

/* Which way this rig's knee legally bends, derived the way the engine does:
 * +theta about the lateral axis displaces the ankle along cross(axis, shin),
 * and a knee sends the foot BACK. */
static void knee_setup(JceSkeleton *sk, const JceHumanoidMap *m,
                       jce_vec3 *out_axis, float *out_sign)
{
    const int hipL = jce_skeleton_find_joint(sk, "thigh_l");
    const int hipR = jce_skeleton_find_joint(sk, "thigh_r");
    const int head = jce_skeleton_find_joint(sk, "head");
    const int knee = jce_skeleton_find_joint(sk, "calf_l");
    const int foot = jce_skeleton_find_joint(sk, "foot_l");
    jce_quat rest[NJF];
    jce_humanoid_rest_locals(sk, rest);
    const jce_vec3 pL   = model_pos_from_locals(sk, hipL);
    const jce_vec3 pR   = model_pos_from_locals(sk, hipR);
    const jce_vec3 pH   = model_pos_from_locals(sk, head);
    const jce_vec3 pHip = model_pos_from_locals(sk, 0);
    const jce_vec3 pK   = model_pos_from_locals(sk, knee);
    const jce_vec3 pF   = model_pos_from_locals(sk, foot);
    jce_vec3 right = jce_v3_normalize(jce_v3_sub(pR, pL));
    jce_vec3 up = jce_v3_sub(pH, pHip);
    up = jce_v3_normalize(jce_v3_sub(up, jce_v3_scale(right,
                                                      jce_v3_dot(up, right))));
    const jce_vec3 fwd = jce_v3_normalize(jce_v3_cross(right, up));
    const jce_vec3 shin = jce_v3_sub(pF, pK);
    const float s = jce_v3_dot(jce_v3_cross(right, shin), fwd);
    *out_axis = right;
    *out_sign = (s > 0.0f) ? -1.0f : 1.0f;

    /* And the engine must say the same thing.  This second derivation is the
     * only reason the elbow's disagreement was findable; leaving the knee
     * without one would leave the cheaper half of the pair untested. */
    jce_vec3 eaxis; float esign;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_humanoid_hinge_axis(m, sk, JCE_HB_LEFT_LOWER_LEG, &eaxis, &esign),
        "the engine refused a hinge axis for a knee on a two-hip rig");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f,
        jce_v3_dot(eaxis, right) * esign * (*out_sign),
        "the engine and this test disagree about which way the knee folds");
}

static jce_quat about(jce_vec3 axis, float deg)
{
    const float h = deg * 0.017453292519943295f * 0.5f;
    const float s = sinf(h);
    return jce_q_normalize(jce_v4(axis.x * s, axis.y * s, axis.z * s, cosf(h)));
}

static void test_a_backward_knee_is_clamped(void)
{
    JceSkeleton *sk = make_full_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    TEST_ASSERT_TRUE_MESSAGE(m.joint[JCE_HB_LEFT_LOWER_LEG] >= 0 &&
                             m.joint[JCE_HB_RIGHT_UPPER_LEG] >= 0,
        "the fixture no longer maps both legs, so the body frame -- and this "
        "whole test -- has nothing to derive from");

    jce_vec3 axis; float sign;
    knee_setup(sk, &m, &axis, &sign);

    /* 40 degrees the WRONG way: clamped back to the 5-degree allowance. */
    const jce_quat bad = about(axis, -40.0f * sign);
    const jce_quat out = jce_humanoid_clamp_knee_direction(&m, sk,
                             JCE_HB_LEFT_LOWER_LEG, 5.0f, bad);
    const float got = signed_angle_about(out, axis) * sign;
    printf("  knee: asked %+.1f deg, got %+.1f deg (allowance 5)\n",
           -40.0, (double)got);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, -5.0f, got,
        "a knee bent 40 degrees BACKWARDS was not clamped to the "
        "hyperextension allowance");
    jce_skeleton_destroy(sk);
}

static void test_a_forward_knee_is_untouched_bit_for_bit(void)
{
    /* The control that stops the clamp from being "always fire": a knee bent
     * the way knees bend must come back as the caller's own bits, so a test
     * asking "did it fire" has an exact answer and a legal pose is not
     * re-quantised every frame. */
    JceSkeleton *sk = make_full_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_vec3 axis; float sign;
    knee_setup(sk, &m, &axis, &sign);

    const jce_quat good = about(axis, 60.0f * sign);
    const jce_quat out = jce_humanoid_clamp_knee_direction(&m, sk,
                             JCE_HB_LEFT_LOWER_LEG, 5.0f, good);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&good, &out, sizeof good,
        "a knee bent the RIGHT way was modified");

    /* And a small wrong-way bend, inside the allowance, is also untouched. */
    const jce_quat small = about(axis, -3.0f * sign);
    const jce_quat out2 = jce_humanoid_clamp_knee_direction(&m, sk,
                              JCE_HB_LEFT_LOWER_LEG, 5.0f, small);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&small, &out2, sizeof small,
        "a 3-degree hyperextension inside a 5-degree allowance was clamped");
    jce_skeleton_destroy(sk);
}

static void test_the_knee_clamp_leaves_other_bones_alone(void)
{
    /* Knees only.  An elbow's hinge axis is not the body's lateral axis -- it
     * depends on how the rig rolls the upper arm -- so clamping one about this
     * axis would reject correct animation. */
    JceSkeleton *sk = make_full_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_vec3 axis; float sign;
    knee_setup(sk, &m, &axis, &sign);
    const jce_quat bad = about(axis, -40.0f * sign);
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        if (b == JCE_HB_LEFT_LOWER_LEG || b == JCE_HB_RIGHT_LOWER_LEG) continue;
        const jce_quat out = jce_humanoid_clamp_knee_direction(&m, sk,
                                 (JceHumanoidBone)b, 5.0f, bad);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&bad, &out, sizeof bad,
            "a bone that is not a knee was clamped by the knee rule");
    }
    jce_skeleton_destroy(sk);
}

/* One leg and one arm: no LATERAL PAIR at all, so the body frame cannot be
 * derived.  make_rig does not serve here -- it has only one leg but BOTH arms,
 * and the frame legitimately falls back to the shoulders.  That fallback is
 * the reason this fixture has to be built on purpose: a test aimed at "the
 * frame is underivable" that actually hits the fallback would pass while
 * proving the opposite. */
#define NJH 6
static JceSkeleton *make_half_rig(void)
{
    JceJoint j[NJH];
    joint(&j[0], "pelvis",     -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1], "spine_01",    0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2], "upperarm_l",  1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3], "thigh_l",     0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[4], "calf_l",      3, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[5], "foot_l",      4, jce_v3(0.0f, -0.5f, 0.0f));
    return jce_skeleton_create(j, NJH);
}

static void test_without_a_lateral_pair_the_frame_is_refused(void)
{
    /* Refusing leaves the delta alone, the same contract every other "cannot
     * be derived" case in this file keeps -- and it is what stops the clamp
     * from inventing an axis and rejecting correct motion on a rig it cannot
     * reason about. */
    JceSkeleton *sk = make_half_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    TEST_ASSERT_TRUE_MESSAGE(m.joint[JCE_HB_RIGHT_UPPER_LEG] < 0 &&
                             m.joint[JCE_HB_RIGHT_UPPER_ARM] < 0,
        "the fixture grew a right limb, so the frame is derivable after all "
        "and this test no longer reaches the case it was written for");
    TEST_ASSERT_TRUE_MESSAGE(m.joint[JCE_HB_LEFT_LOWER_LEG] >= 0,
        "the fixture lost its left knee, so nothing would be clamped either "
        "way");
    /* SIX directions, not one.  A single arbitrary delta is a weak control:
     * the first version used one, and a mutation that replaced the underivable
     * frame with a fixed guess passed it -- that delta simply happened to be
     * legal under the guess.  A 60-degree bend about each signed axis cannot
     * all be legal under ANY frame, so if the clamp invents one, at least one
     * of these six is clamped and this fails. */
    static const jce_vec3 axes[6] = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f }, {  0.0f,-1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }, {  0.0f, 0.0f,-1.0f },
    };
    for (int i = 0; i < 6; i++) {
        const jce_quat d = about(axes[i], 60.0f);
        const jce_quat out = jce_humanoid_clamp_knee_direction(&m, sk,
                                 JCE_HB_LEFT_LOWER_LEG, 5.0f, d);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&d, &out, sizeof d,
            "the clamp fired on a rig whose body frame cannot be derived, so "
            "it is bending a knee about an axis it invented");
    }
    jce_skeleton_destroy(sk);
}

/* ── 7. An elbow bends one way, WHERE THE RIG SAYS WHICH ───────────────── */

/* make_rig's arm is dead straight: upperarm_l -> lowerarm_l -> hand_l all fall
 * along -Y, which is PSX_BagMan's shape (its rest arm measures exactly 180
 * degrees).  This one bends the forearm forward at rest, which is CesiumMan's
 * shape (147 degrees) and what riggers leave so an IK solver knows the side. */
/* IT HAS LEGS, and they are not decoration.  Without them the "the elbow rule
 * leaves knees alone" test below ran on a rig whose knees do not exist, so it
 * passed with the elbow clamp's bone filter DELETED -- measured, as a mutation
 * survivor.  A test that cannot reach the thing it is about is not evidence
 * about that thing. */
#define NJE 10
static JceSkeleton *make_bent_arm_rig(void)
{
    JceJoint j[NJE];
    joint(&j[0], "pelvis",     -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1], "spine_01",    0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2], "upperarm_l",  1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3], "lowerarm_l",  2, jce_v3(0.0f, -0.5f, 0.0f));
    /* the bend: the hand is forward of the forearm's line, not straight down */
    joint(&j[4], "hand_l",      3, jce_v3(0.0f, -0.45f, 0.22f));
    joint(&j[5], "thigh_l",     0, jce_v3( 0.1f, 0.0f, 0.0f));
    joint(&j[6], "calf_l",      5, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[7], "foot_l",      6, jce_v3(0.0f, -0.5f, 0.0f));
    joint(&j[8], "thigh_r",     0, jce_v3(-0.1f, 0.0f, 0.0f));
    joint(&j[9], "calf_r",      8, jce_v3(0.0f, -0.5f, 0.0f));
    return jce_skeleton_create(j, NJE);
}

static void test_a_backward_elbow_is_clamped_when_the_rig_has_a_rest_bend(void)
{
    JceSkeleton *sk = make_bent_arm_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));

    /* ASK THE ENGINE FOR THE AXIS.  An earlier version of this test derived it
     * again, here, from jce_skeleton_rest_pose(); the clamp derives it from
     * jce_skeleton_rest_trs().  The two read the same rig through different
     * accessors, and on this fixture they disagreed enough that a 40-degree
     * hyperextension came back as -15.7 instead of -5 -- a red that was about
     * the TEST, not the engine.  So the assertion below uses the engine's own
     * answer, and the cross-check that follows is what would have said so. */
    jce_vec3 axis; float sign;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_humanoid_hinge_axis(&m, sk, JCE_HB_LEFT_LOWER_ARM, &axis, &sign),
        "the engine refused an axis on a rig with 26 degrees of rest bend");

    /* The same derivation, read through the OTHER accessor.  It must agree, or
     * one of the two views of this skeleton is wrong and every model-space
     * conclusion drawn from either is suspect. */
    const jce_vec3 pu = model_pos_from_locals(
        sk, jce_skeleton_find_joint(sk, "upperarm_l"));
    const jce_vec3 pe = model_pos_from_locals(
        sk, jce_skeleton_find_joint(sk, "lowerarm_l"));
    const jce_vec3 ph = model_pos_from_locals(
        sk, jce_skeleton_find_joint(sk, "hand_l"));
    const jce_vec3 lower = jce_v3_sub(ph, pe);
    const jce_vec3 axis2 =
        jce_v3_normalize(jce_v3_cross(jce_v3_sub(pe, pu), lower));
    const float toward = jce_v3_dot(jce_v3_cross(axis2, lower),
                                    jce_v3_sub(pu, ph));
    const float sign2 = (toward > 0.0f) ? 1.0f : -1.0f;
    printf("  elbow axis: engine (%+.4f,%+.4f,%+.4f) sign %+.0f | "
           "locals (%+.4f,%+.4f,%+.4f) sign %+.0f\n",
           (double)axis.x, (double)axis.y, (double)axis.z, (double)sign,
           (double)axis2.x, (double)axis2.y, (double)axis2.z, (double)sign2);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(1e-4f, 1.0f,
        jce_v3_dot(axis, axis2) * sign * sign2,
        "the rest TRS and the rest locals describe different arms");

    const jce_quat bad = about(axis, -40.0f * sign);
    const jce_quat out = jce_humanoid_clamp_elbow_direction(&m, sk,
                             JCE_HB_LEFT_LOWER_ARM, 5.0f, bad);
    const float got = signed_angle_about(out, axis) * sign;
    printf("  elbow: asked %+.1f deg, got %+.1f deg (allowance 5)\n",
           -40.0, (double)got);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.5f, -5.0f, got,
        "an elbow bent 40 degrees BACKWARDS was not clamped");

    /* Folding the arm is legal and must come back bit-for-bit. */
    const jce_quat good = about(axis, 70.0f * sign);
    const jce_quat out2 = jce_humanoid_clamp_elbow_direction(&m, sk,
                              JCE_HB_LEFT_LOWER_ARM, 5.0f, good);
    TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&good, &out2, sizeof good,
        "an elbow folded the RIGHT way was modified");
    jce_skeleton_destroy(sk);
}

/* TWO degrees of bend: not straight, and not enough.
 *
 * make_rig's arm is EXACTLY 180, and an exactly-straight arm is caught by the
 * degeneracy guard further down whether or not the 5-degree floor exists --
 * which is why the floor survived its first mutation control with every test
 * still green.  A rig bent by an amount that is real to the arithmetic but
 * meaningless as intent is the only thing that tests the floor itself. */
#define NJB 5
static JceSkeleton *make_barely_bent_arm_rig(void)
{
    JceJoint j[NJB];
    joint(&j[0], "pelvis",     -1, jce_v3(0.0f,  1.0f, 0.0f));
    joint(&j[1], "spine_01",    0, jce_v3(0.0f,  0.4f, 0.0f));
    joint(&j[2], "upperarm_l",  1, jce_v3( 0.2f, 0.0f, 0.0f));
    joint(&j[3], "lowerarm_l",  2, jce_v3(0.0f, -0.5f, 0.0f));
    /* 0.45 m at 2 degrees off the forearm's line */
    joint(&j[4], "hand_l",      3, jce_v3(0.0f, -0.449726f, 0.015707f));
    return jce_skeleton_create(j, NJB);
}

static void test_a_straight_rest_arm_is_refused_not_guessed(void)
{
    /* make_rig's arm is straight, which is PSX_BagMan's shape.  There is no
     * plane to derive, and an elbow clamped about an INVENTED axis rejects
     * correct animation -- strictly worse than not clamping.  Six directions,
     * for the same reason the knee's version needs them: one arbitrary delta
     * can be legal under a guessed frame by luck. */
    JceSkeleton *sk = make_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    TEST_ASSERT_TRUE_MESSAGE(m.joint[JCE_HB_LEFT_LOWER_ARM] >= 0,
        "the fixture lost its left elbow, so nothing would be clamped either "
        "way and this test proves nothing");
    static const jce_vec3 axes[6] = {
        { 1.0f, 0.0f, 0.0f }, { -1.0f, 0.0f, 0.0f },
        { 0.0f, 1.0f, 0.0f }, {  0.0f,-1.0f, 0.0f },
        { 0.0f, 0.0f, 1.0f }, {  0.0f, 0.0f,-1.0f },
    };
    for (int i = 0; i < 6; i++) {
        const jce_quat d = about(axes[i], 60.0f);
        const jce_quat out = jce_humanoid_clamp_elbow_direction(&m, sk,
                                 JCE_HB_LEFT_LOWER_ARM, 5.0f, d);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&d, &out, sizeof d,
            "the elbow clamp fired on a rig with no rest bend, so it is "
            "bending an arm about an axis it invented");
    }
    jce_skeleton_destroy(sk);

    /* AND THE SAME FOR A RIG THAT IS ONLY ALMOST STRAIGHT.  Two degrees is
     * enough bend for the cross product to be non-zero and point somewhere
     * definite, so the degeneracy guard lets it through; only the 5-degree
     * floor refuses it.  Without this rig, deleting that floor changed no
     * test's verdict -- measured, not assumed. */
    sk = make_barely_bent_arm_rig();
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    jce_vec3 haxis; float hsign;
    TEST_ASSERT_FALSE_MESSAGE(
        jce_humanoid_hinge_axis(&m, sk, JCE_HB_LEFT_LOWER_ARM, &haxis, &hsign),
        "two degrees of rest bend was accepted as the artist's hinge plane");
    for (int i = 0; i < 6; i++) {
        const jce_quat d = about(axes[i], 60.0f);
        const jce_quat out = jce_humanoid_clamp_elbow_direction(&m, sk,
                                 JCE_HB_LEFT_LOWER_ARM, 5.0f, d);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&d, &out, sizeof d,
            "the elbow clamp fired on two degrees of rest bend, which is "
            "rounding, not intent");
    }
    jce_skeleton_destroy(sk);
}

static void test_the_elbow_clamp_leaves_knees_and_others_alone(void)
{
    JceSkeleton *sk = make_bent_arm_rig();
    JceHumanoidMap m;
    TEST_ASSERT_TRUE(jce_humanoid_map_build(sk, &m));
    /* The knee must be REACHABLE, or this test is about an absence. */
    jce_vec3 ka; float ks;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_humanoid_hinge_axis(&m, sk, JCE_HB_LEFT_LOWER_LEG, &ka, &ks),
        "the fixture has no answerable knee, so 'the elbow rule leaves knees "
        "alone' would pass on a rig that has none");
    /* ...and the delta must be ILLEGAL about it, or being left alone proves
     * only that it was already legal. */
    const jce_quat d = jce_q_normalize(jce_v4(0.4f, 0.2f, 0.3f, 0.8f));
    const jce_quat kclamped = jce_humanoid_clamp_knee_direction(&m, sk,
                                  JCE_HB_LEFT_LOWER_LEG, 5.0f, d);
    TEST_ASSERT_TRUE_MESSAGE(memcmp(&d, &kclamped, sizeof d) != 0,
        "the probe delta is already legal for this knee, so the elbow rule "
        "leaving it alone would be indistinguishable from doing nothing");
    for (int b = 0; b < JCE_HB_COUNT; b++) {
        if (b == JCE_HB_LEFT_LOWER_ARM || b == JCE_HB_RIGHT_LOWER_ARM) continue;
        const jce_quat out = jce_humanoid_clamp_elbow_direction(&m, sk,
                                 (JceHumanoidBone)b, 5.0f, d);
        TEST_ASSERT_EQUAL_MEMORY_MESSAGE(&d, &out, sizeof d,
            "a bone that is not an elbow was clamped by the elbow rule");
    }
    jce_skeleton_destroy(sk);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_limb_resolves_to_the_same_joints_a_name_triple_would);
    RUN_TEST(test_left_and_right_do_not_resolve_to_each_other);
    RUN_TEST(test_the_triple_is_a_parent_chain);
    RUN_TEST(test_a_rig_with_no_humanoid_names_resolves_to_nothing);
    RUN_TEST(test_an_out_of_range_limb_is_refused);
    RUN_TEST(test_hips_height_survives_a_z_up_rig);
    RUN_TEST(test_hips_height_is_measured_to_the_feet);
    RUN_TEST(test_a_foot_control_bone_is_refused_not_solved);
    RUN_TEST(test_twist_redistribution_does_not_move_the_pose);
    RUN_TEST(test_twist_fraction_zero_is_bit_identical);
    RUN_TEST(test_twist_skips_a_foot_control_bone);
    RUN_TEST(test_a_backward_knee_is_clamped);
    RUN_TEST(test_a_forward_knee_is_untouched_bit_for_bit);
    RUN_TEST(test_the_knee_clamp_leaves_other_bones_alone);
    RUN_TEST(test_without_a_lateral_pair_the_frame_is_refused);
    RUN_TEST(test_a_backward_elbow_is_clamped_when_the_rig_has_a_rest_bend);
    RUN_TEST(test_a_straight_rest_arm_is_refused_not_guessed);
    RUN_TEST(test_the_elbow_clamp_leaves_knees_and_others_alone);
    return UNITY_END();
}
