/*
 * test_jce_ragdoll.c  Headless self-test for RAGDOLL CORE.
 *
 * Builds a 3-bone vertical skeleton (Root at origin; Hip local (0,-1,0); Knee
 * local (0,-1,0) of Hip), wraps it in a ragdoll backed by a real Bullet
 * physics world, and exercises both sync directions:
 *
 *   1. blend_weight = 0 (full physics): step under gravity and assert the
 *      lower bodies FELL, the chain stayed connected, and every recovered
 *      local transform (and a jce_skeleton_evaluate of them) is finite.
 *   2. blend_weight = 1 (full animation): drive the bodies to a known animated
 *      pose, step, read back, and assert the bodies track the animation.
 *
 * No GPU.  Deterministic: fixed inputs / fixed step count / generous solver
 * tolerance.
 */

#include "unity.h"

#include "middleware/animation/jce_ragdoll.h"

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/os/core/jce_math.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* Joint indices for the 3-bone vertical chain. */
enum { J_ROOT = 0, J_HIP = 1, J_KNEE = 2, J_COUNT = 3 };

/* Build the 3-bone vertical skeleton.  Each joint's rest TRS is LOCAL to its
 * parent; the local matrix is filled too (jce_skeleton_create copies it). */
static JceSkeleton *make_chain_skeleton(void)
{
    JceJoint joints[J_COUNT];
    memset(joints, 0, sizeof(joints));

    jce_vec3 local_t[J_COUNT] = {
        { 0.0f,  0.0f, 0.0f },   /* Root at origin              */
        { 0.0f, -1.0f, 0.0f },   /* Hip:  1 unit below Root     */
        { 0.0f, -1.0f, 0.0f },   /* Knee: 1 unit below Hip      */
    };
    int16_t parents[J_COUNT] = { -1, J_ROOT, J_HIP };
    const char *names[J_COUNT] = { "Root", "Hip", "Knee" };

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
    memset(&wd, 0, sizeof(wd));
    wd.gravity        = jce_v3(0.0f, -9.8f, 0.0f);
    wd.max_bodies     = 256;
    wd.fixed_timestep = 1.0f / 60.0f;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

/* True only if every float in the matrix array is finite. */
static bool all_finite(const jce_mat4 *m, uint32_t count)
{
    for (uint32_t i = 0; i < count; ++i) {
        const float *f = (const float *)&m[i].raw[0][0];
        for (int k = 0; k < 16; ++k)
            if (!isfinite(f[k]))
                return false;
    }
    return true;
}

/* Bind-pose WORLD origin of each joint in the test chain (Root 0, Hip -1,
 * Knee -2 along Y) — used as the reference for "fell" / "tracks". */
static jce_vec3 bind_world_origin(int joint)
{
    return jce_v3(0.0f, -(float)joint, 0.0f);
}

/* ================================================================== */
/* Test 1: full physics — chain falls and stays connected             */
/* ================================================================== */

static void test_ragdoll_falls_under_gravity(void)
{
    JceSkeleton     *skel  = make_chain_skeleton();
    TEST_ASSERT_NOT_NULL(skel);
    TEST_ASSERT_EQUAL_UINT32(3u, jce_skeleton_joint_count(skel));

    JcePhysicsWorld *world = make_world();
    TEST_ASSERT_NOT_NULL(world);

    JceRagdoll *rd = jce_ragdoll_create(skel, world, 0.1f, 1.0f, 1.0f);
    TEST_ASSERT_NOT_NULL(rd);
    TEST_ASSERT_EQUAL_INT(3, rd->body_count);
    TEST_ASSERT_EQUAL_INT(2, rd->constraint_count);

    /* Full physics. */
    jce_ragdoll_set_blend_weight(rd, 0.0f);

    /* Record bind-pose Y of each body before stepping. */
    float bind_y[J_COUNT];
    for (int s = 0; s < rd->body_count; ++s) {
        jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
        jce_quat q = jce_q_identity();
        jce_physics_body_get_transform(world, rd->bodies[s].body, &p, &q);
        bind_y[rd->bodies[s].joint_index] = p.y;
    }

    /* Step ~20 fixed steps. */
    for (int i = 0; i < 20; ++i)
        jce_physics_step(world, 1.0f / 60.0f);

    /* Recover a bone-local pose from the fallen bodies. */
    jce_mat4 locals[J_COUNT];
    jce_ragdoll_sync_to_pose(rd, locals);

    /* All recovered local transforms must be finite. */
    TEST_ASSERT_TRUE(all_finite(locals, (uint32_t)J_COUNT));

    /* The skeleton must evaluate them to finite skinning matrices. */
    jce_mat4 skin[J_COUNT];
    jce_skeleton_evaluate(skel, locals, skin, (uint32_t)J_COUNT);
    TEST_ASSERT_TRUE(all_finite(skin, (uint32_t)J_COUNT));

    /* Read the fallen world positions back. */
    jce_vec3 world_pos[J_COUNT];
    for (int s = 0; s < rd->body_count; ++s) {
        jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
        jce_quat q = jce_q_identity();
        jce_physics_body_get_transform(world, rd->bodies[s].body, &p, &q);
        world_pos[rd->bodies[s].joint_index] = p;
        TEST_ASSERT_TRUE(isfinite(p.x) && isfinite(p.y) && isfinite(p.z));
    }

    /* The Hip and Knee bodies must have FALLEN (world Y below their bind Y).
     * (The root is unconstrained too, so it also falls — we only require the
     * lower bodies to drop, which is the load-bearing assertion.) */
    TEST_ASSERT_TRUE(world_pos[J_HIP].y  < bind_y[J_HIP]  - 0.05f);
    TEST_ASSERT_TRUE(world_pos[J_KNEE].y < bind_y[J_KNEE] - 0.05f);

    /* Knee fell at least as far as the Hip (lower bodies drop further). */
    TEST_ASSERT_TRUE(world_pos[J_KNEE].y <= world_pos[J_HIP].y + 0.01f);

    /* Chain stayed CONNECTED: each child body stays within ~one bone length of
     * its parent (the GENERIC6DOF anchor holds the joints together; generous
     * tolerance for the solver).  Bind segment length is 1 unit. */
    float d_root_hip = jce_v3_len(jce_v3_sub(world_pos[J_HIP], world_pos[J_ROOT]));
    float d_hip_knee = jce_v3_len(jce_v3_sub(world_pos[J_KNEE], world_pos[J_HIP]));
    TEST_ASSERT_TRUE(d_root_hip < 1.5f);
    TEST_ASSERT_TRUE(d_hip_knee < 1.5f);

    jce_ragdoll_destroy(rd);
    jce_physics_destroy(world);
    jce_skeleton_destroy(skel);
}

/* ================================================================== */
/* Test 2: full animation — bodies track the animated pose            */
/* ================================================================== */

static void test_ragdoll_tracks_animation(void)
{
    JceSkeleton     *skel  = make_chain_skeleton();
    TEST_ASSERT_NOT_NULL(skel);
    JcePhysicsWorld *world = make_world();
    TEST_ASSERT_NOT_NULL(world);

    JceRagdoll *rd = jce_ragdoll_create(skel, world, 0.1f, 1.0f, 1.0f);
    TEST_ASSERT_NOT_NULL(rd);

    /* A known animated pose: translate the whole chain up by +5 on Y by moving
     * the ROOT local up; the Hip/Knee keep their rest locals, so the whole
     * chain rigidly shifts +5.  Expected world Y: Root 5, Hip 4, Knee 3. */
    jce_mat4 anim_locals[J_COUNT];
    anim_locals[J_ROOT] = jce_m4_from_trs(jce_v3(0.0f, 5.0f, 0.0f),
                                          jce_q_identity(),
                                          jce_v3(1.0f, 1.0f, 1.0f));
    anim_locals[J_HIP]  = jce_m4_from_trs(jce_v3(0.0f, -1.0f, 0.0f),
                                          jce_q_identity(),
                                          jce_v3(1.0f, 1.0f, 1.0f));
    anim_locals[J_KNEE] = jce_m4_from_trs(jce_v3(0.0f, -1.0f, 0.0f),
                                          jce_q_identity(),
                                          jce_v3(1.0f, 1.0f, 1.0f));

    /* Full animation drive: snap the bodies to the pose, then step.  With
     * blend_weight 1 the bodies are re-snapped (velocities zeroed) so gravity
     * never accumulates. */
    for (int i = 0; i < 5; ++i) {
        jce_ragdoll_sync_from_pose(rd, anim_locals, 1.0f, 1.0f / 60.0f);
        jce_physics_step(world, 1.0f / 60.0f);
    }
    /* Final re-snap so the read-back reflects the exact target (post-step the
     * solver may have nudged it by a hair). */
    jce_ragdoll_sync_from_pose(rd, anim_locals, 1.0f, 1.0f / 60.0f);

    /* Expected animated world origins. */
    jce_vec3 expect[J_COUNT] = {
        { 0.0f, 5.0f, 0.0f },   /* Root  */
        { 0.0f, 4.0f, 0.0f },   /* Hip   */
        { 0.0f, 3.0f, 0.0f },   /* Knee  */
    };

    for (int s = 0; s < rd->body_count; ++s) {
        int joint = rd->bodies[s].joint_index;
        jce_vec3 p = jce_v3(0.0f, 0.0f, 0.0f);
        jce_quat q = jce_q_identity();
        jce_physics_body_get_transform(world, rd->bodies[s].body, &p, &q);

        /* body_to_bone is identity in this build (body placed at bone origin),
         * so the body origin equals the animated bone origin. */
        float dist = jce_v3_len(jce_v3_sub(p, expect[joint]));
        TEST_ASSERT_TRUE(dist < 0.25f);
        (void)bind_world_origin; /* reference helper, kept for clarity */
    }

    /* sync_to_pose of the tracked bodies must reproduce the animated locals'
     * translations (root +5, others -1) within tolerance, and be finite. */
    jce_mat4 readback[J_COUNT];
    jce_ragdoll_sync_to_pose(rd, readback);
    TEST_ASSERT_TRUE(all_finite(readback, (uint32_t)J_COUNT));

    jce_vec3 root_t = jce_v3(readback[J_ROOT].raw[3][0],
                             readback[J_ROOT].raw[3][1],
                             readback[J_ROOT].raw[3][2]);
    TEST_ASSERT_TRUE(jce_v3_len(jce_v3_sub(root_t, jce_v3(0.0f, 5.0f, 0.0f))) < 0.25f);

    jce_ragdoll_destroy(rd);
    jce_physics_destroy(world);
    jce_skeleton_destroy(skel);
}

/* ================================================================== */
/* Test 3: NULL / bounds rejection                                    */
/* ================================================================== */

static void test_ragdoll_rejects_null(void)
{
    JcePhysicsWorld *world = make_world();
    TEST_ASSERT_NOT_NULL(world);
    JceSkeleton *skel = make_chain_skeleton();
    TEST_ASSERT_NOT_NULL(skel);

    TEST_ASSERT_NULL(jce_ragdoll_create(NULL,  world, 0.1f, 1.0f, 1.0f));
    TEST_ASSERT_NULL(jce_ragdoll_create(skel,  NULL,  0.1f, 1.0f, 1.0f));

    /* NULL-safe destroy / setters. */
    jce_ragdoll_destroy(NULL);
    jce_ragdoll_set_blend_weight(NULL, 0.5f);
    jce_ragdoll_sync_to_pose(NULL, NULL);
    jce_ragdoll_sync_from_pose(NULL, NULL, 0.0f, 0.0f);

    jce_physics_destroy(world);
    jce_skeleton_destroy(skel);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_ragdoll_falls_under_gravity);
    RUN_TEST(test_ragdoll_tracks_animation);
    RUN_TEST(test_ragdoll_rejects_null);
    return UNITY_END();
}
