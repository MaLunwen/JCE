/*
 * test_jce_ragdoll_joint_limits.c
 *
 * Ragdoll joints must be BOUNDED.
 *
 * physics.ragdoll.per-bone-joint-limits: "every joint it creates is the same
 * unlimited ball joint: jce_ragdoll.c sets type GENERIC6DOF with
 * lower_limit == upper_limit == 0 and the in-code comment states 'angular
 * axes are left free (Bullet's default 6DOF angular limits)'.  So elbows and
 * knees hyperextend and heads rotate freely."
 *
 * WHAT MAKES THIS A LIMITS TEST AND NOT A RAGDOLL TEST.  "The chain stays
 * connected" was already true -- it is exactly what the old unlimited ball
 * joint delivered -- so asserting it again would pass whether or not a single
 * limit exists.  The load-bearing case pushes one joint HARD for long enough
 * that an unconstrained one would have spun far past any angle, and reads how
 * far it got, AGAINST A CONTROL that runs the identical push with limits
 * disabled.  Same rig, same torque, same steps; the only difference is the
 * limit.
 *
 * THE BOUND COMES FROM THE ENGINE, not from this file.  It is whatever
 * jce_humanoid_muscle_limits reports for that bone, so re-tuning the table
 * cannot leave this assertion silently checking a number nobody uses.
 *
 * IN THE TREE: tests/ is tracked on this branch and gitignored on `main`.
 * Settle it with `git check-ignore -v <path>`, never from memory.
 */

#include "middleware/animation/jce_ragdoll.h"

#include <jce/middleware/animation/jce_humanoid.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

/* A three-bone arm with HUMANOID names, so jce_humanoid_bone_from_joint_name
 * resolves them and the muscle table has something to say.  The middle one is
 * the joint under test. */
enum { J_UPPER = 0, J_LOWER = 1, J_HAND = 2, J_COUNT = 3 };

static JceSkeleton *make_arm(void)
{
    JceJoint joints[J_COUNT];
    memset(joints, 0, sizeof joints);

    const jce_vec3 local_t[J_COUNT] = {
        { 0.0f,  0.0f, 0.0f },
        { 0.0f, -0.3f, 0.0f },
        { 0.0f, -0.3f, 0.0f },
    };
    const int16_t parents[J_COUNT] = { -1, J_UPPER, J_LOWER };
    const char *names[J_COUNT] = { "LeftUpperArm", "LeftLowerArm", "LeftHand" };

    for (int i = 0; i < J_COUNT; ++i) {
        strncpy(joints[i].name, names[i], sizeof(joints[i].name) - 1);
        joints[i].parent           = parents[i];
        joints[i].rest_translation = local_t[i];
        joints[i].rest_rotation    = jce_q_identity();
        joints[i].rest_scale       = jce_v3(1.0f, 1.0f, 1.0f);
        joints[i].local_transform  = jce_m4_from_trs(local_t[i],
                                                     jce_q_identity(),
                                                     jce_v3(1.0f, 1.0f, 1.0f));
        joints[i].inverse_bind_matrix = jce_m4_identity();
    }
    return jce_skeleton_create(joints, (uint32_t)J_COUNT);
}

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof wd);
    /* NO GRAVITY.  A falling chain bends on its own, and this file is about
     * what the LIMIT does -- gravity would put a bend in both arms of the
     * comparison and shrink the difference it is trying to read. */
    wd.gravity        = jce_v3(0.0f, 0.0f, 0.0f);
    wd.max_bodies     = 256;
    wd.fixed_timestep = 1.0f / 60.0f;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

/* The angle BETWEEN two bodies, in degrees -- how far the joint between them
 * has bent away from its bind pose.
 *
 * RELATIVE, NOT WORLD, and the first version of this file got that wrong in a
 * way worth recording.  It measured each body's rotation away from identity,
 * which looked right because every bind orientation here IS identity.  But a
 * joint limit constrains the RELATIVE rotation, and this chain is
 * free-floating -- so a torque tumbles the whole arm as one rigid unit, every
 * body reaches ~180 degrees of world rotation, and the joint between them
 * never moves.  The instrument reported 178.8 degrees against a 90-degree
 * limit and read exactly like "the limits do not work"; the limits were fine
 * and the quantity was wrong.
 *
 * Bind is identity for both bodies here, so the relative rotation is
 * inverse(parent) * child, and |w| = cos(theta/2). */
static float joint_bend_deg(const JcePhysicsWorld *w,
                            JceBodyHandle parent, JceBodyHandle child)
{
    jce_vec3 pa, pb;
    jce_quat qa, qb;
    jce_physics_body_get_transform(w, parent, &pa, &qa);
    jce_physics_body_get_transform(w, child,  &pb, &qb);
    (void)pa; (void)pb;

    /* Spelled out rather than via a helper: jce_math.h has no conjugate, and
     * for a unit quaternion the inverse IS the conjugate. */
    const jce_quat inv_a = { -qa.x, -qa.y, -qa.z, qa.w };
    const jce_quat rel   = jce_q_multiply(inv_a, qb);
    float qw = fabsf(rel.w);
    if (qw > 1.0f) qw = 1.0f;
    return 2.0f * acosf(qw) * (180.0f / 3.14159265358979323846f);
}

/* The widest angle the muscle table allows this bone -- the looser of its
 * twist range and its swing cone, which is what the ragdoll uses because it
 * cannot tell which local axis is twist (jce_humanoid.h says why). */
static float allowed_deg(void)
{
    JceHumanoidMuscleLimits L;
    TEST_ASSERT_TRUE_MESSAGE(
        jce_humanoid_muscle_limits(JCE_HB_LEFT_LOWER_ARM, &L),
        "the muscle table has no entry for the lower arm, so this file has no "
        "bound to check against");
    float a = -L.twist_min;
    if (L.twist_max > a) a = L.twist_max;
    if (L.swing_max > a) a = L.swing_max;
    return a;
}

/* Spin the LOWER ARM about all three axes for long enough that an
 * unconstrained joint would be far past any limit, and report the furthest it
 * got from rest. */
static float push_and_measure(float limit_scale)
{
    JceSkeleton *skel = make_arm();
    TEST_ASSERT_NOT_NULL(skel);
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceRagdoll *rd = jce_ragdoll_create(skel, w, 0.05f, 1.0f, limit_scale);
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_INT(J_COUNT, rd->body_count);
    /* THE CHAIN MUST BE CONNECTED WHICHEVER PATH BUILT IT.  A constraint that
     * failed to create leaves the bone a free body, and a free body spins to
     * ~180 degrees under any torque -- which reads exactly like "the limit is
     * not working" while the real fault is that there is no joint at all. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(J_COUNT - 1, rd->constraint_count,
        "the ragdoll did not create one constraint per non-root joint");

    /* Full physics: at blend_weight 1 the bodies are driven back to the
     * animated pose every step and nothing the solver does survives. */
    jce_ragdoll_set_blend_weight(rd, 0.0f);

    const JceBodyHandle upper = rd->bodies[J_UPPER].body;
    const JceBodyHandle lower = rd->bodies[J_LOWER].body;
    TEST_ASSERT_TRUE(jce_body_valid(upper));
    TEST_ASSERT_TRUE(jce_body_valid(lower));

    /* THE MAGNITUDE MATTERS AND IT TOOK THREE WRONG ANSWERS TO FIND OUT.
     * These capsules are 0.05 m in radius, so their inertia is tiny, and an
     * iterative solver under a torque far past what the joint can resist
     * simply blows through the limit -- the first version of this file pushed
     * with 4.0 and read 178.8 degrees against a 90-degree bound, which looks
     * exactly like "the limits do not work".  It was not the value (capping
     * the limit changed nothing) and not the measurement (switching from
     * world to relative angle changed nothing); it was the push.  Writing the
     * missing angular-limit case for the configurable joint itself is what
     * separated them: that feature works, and this instrument was over-driving
     * it.  0.05 is a torque this joint can actually argue with. */
    #define TORQUE 0.05f

    float worst = 0.0f;
    for (int step = 0; step < 240; ++step) {
        /* All three axes, so the result does not depend on which local axis
         * this rig happens to call twist. */
        jce_physics_body_apply_torque(w, lower, jce_v3(TORQUE, TORQUE, TORQUE));
        jce_physics_step(w, 1.0f / 60.0f);
        const float d = joint_bend_deg(w, upper, lower);
        if (d > worst) worst = d;
    }

    jce_ragdoll_destroy(rd);
    jce_physics_destroy(w);
    jce_skeleton_destroy(skel);
    return worst;
}

static float g_free_bend = -1.0f;

/* THE CONTROL RUNS FIRST, because every number below is read against it.
 * With limits disabled the same push must send the joint well past the range
 * the muscle table allows -- otherwise "it stayed inside the limit" is also
 * what a joint nothing could move looks like. */
static void test_without_limits_the_joint_spins_past_any_range(void)
{
    g_free_bend = push_and_measure(-1.0f);          /* negative = no limits */
    const float allowed = allowed_deg();
    printf("  unlimited bend %.1f deg, muscle range %.1f deg\n",
           (double)g_free_bend, (double)allowed);

    TEST_ASSERT_TRUE_MESSAGE(g_free_bend > allowed,
        "an UNLIMITED joint did not exceed the range its humanoid role "
        "allows, so the torque is too weak or something else is holding it -- "
        "and the limited case below would prove nothing");
}

static void test_with_limits_the_joint_stays_near_its_humanoid_range(void)
{
    TEST_ASSERT_TRUE_MESSAGE(g_free_bend > 0.0f,
        "the control did not run, so there is nothing to read this against");

    const float limited = push_and_measure(1.0f);
    const float allowed = allowed_deg();
    printf("  limited bend   %.1f deg (unlimited was %.1f)\n",
           (double)limited, (double)g_free_bend);

    /* Bounded NEAR its range, not exact to the degree: the solver is
     * iterative and a sustained torque overshoots a limit slightly.  What is
     * being asserted is that a bound exists at all and is roughly the
     * authored one -- the second assertion is what makes that meaningful. */
    TEST_ASSERT_TRUE_MESSAGE(limited < allowed * 1.5f + 15.0f,
        "the joint went far past the angular range its humanoid role allows, "
        "so the limits are not reaching the solver -- elbows and knees still "
        "hyperextend");
    TEST_ASSERT_TRUE_MESSAGE(limited < g_free_bend,
        "the limited joint bent as far as the unlimited one under an "
        "identical push: whatever was configured, nothing is constraining it");
}

/* A bone whose name maps to NO humanoid role is left unlimited rather than
 * given a guessed cone -- a tail, a cape bone or a prop is not a limb.  It
 * must still build. */
static void test_a_non_humanoid_rig_still_builds(void)
{
    JceJoint joints[2];
    memset(joints, 0, sizeof joints);
    const char *names[2] = { "prop_root", "prop_swinger" };
    const int16_t parents[2] = { -1, 0 };
    for (int i = 0; i < 2; ++i) {
        strncpy(joints[i].name, names[i], sizeof(joints[i].name) - 1);
        joints[i].parent           = parents[i];
        joints[i].rest_translation = jce_v3(0.0f, (i == 0) ? 0.0f : -0.3f, 0.0f);
        joints[i].rest_rotation    = jce_q_identity();
        joints[i].rest_scale       = jce_v3(1.0f, 1.0f, 1.0f);
        joints[i].local_transform  =
            jce_m4_from_trs(joints[i].rest_translation, jce_q_identity(),
                            jce_v3(1.0f, 1.0f, 1.0f));
        joints[i].inverse_bind_matrix = jce_m4_identity();
    }
    JceSkeleton *skel = jce_skeleton_create(joints, 2u);
    TEST_ASSERT_NOT_NULL(skel);
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* Limits requested, but no name matches a humanoid role. */
    JceRagdoll *rd = jce_ragdoll_create(skel, w, 0.05f, 1.0f, 1.0f);
    TEST_ASSERT_NOT_NULL_MESSAGE(rd,
        "a rig with no humanoid bone names failed to build a ragdoll at all");
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, rd->constraint_count,
        "the non-humanoid chain lost its constraint -- asking for limits on a "
        "rig that has no roles must leave the chain connected, not drop it");

    jce_ragdoll_destroy(rd);
    jce_physics_destroy(w);
    jce_skeleton_destroy(skel);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_without_limits_the_joint_spins_past_any_range);
    RUN_TEST(test_with_limits_the_joint_stays_near_its_humanoid_range);
    RUN_TEST(test_a_non_humanoid_rig_still_builds);
    return UNITY_END();
}
