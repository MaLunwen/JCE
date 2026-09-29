/*
 * test_jce_root_motion.c — Unit tests for FEATURE 3.2 root-motion extraction.
 *
 * Exercises the REAL dynamic path:
 *   - build an animation clip whose ROOT joint translates +1.0 in Z over its
 *     duration, plus a real skeleton, via jce_anim_clip_create /
 *     jce_skeleton_create;
 *   - sample two adjacent times through the REAL jce_anim_clip_sample;
 *   - call jce_anim_extract_root_delta and assert the per-frame Z step;
 *   - assert loop-wrap accumulation across the clip boundary;
 *   - assert the re-centered pose has the root translation removed;
 *   - drive a real JceAnimPlayer with root motion enabled end-to-end and
 *     confirm the consumed delta matches and the produced pose is re-centered.
 *
 * Links jce_core + jce_animation; the animation layer's clip/skeleton
 * constructors live in the internal src header (not the public include), so
 * this TU pulls in engine/src via the test's include dir.
 */

#include "unity.h"

/* Internal header: jce_anim_clip_create / JceAnimChannel / JceInterpolation
 * AND the root-motion API (jce_anim_extract_root_delta + player set/consume).
 * The internal header is self-contained for these; the PUBLIC animation header
 * forward-declares the same opaque typedefs, so including both in one C99 TU
 * would be a duplicate-typedef error — include only the internal one here. */
#include "middleware/animation/jce_animation.h"
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_math.h>

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define ROOT 0u
#define DUR  1.0f
#define EPS  1e-4f

/* Build a 2-joint skeleton (root + one child). The root joint is index 0. */
static JceSkeleton *make_skeleton(void)
{
    JceJoint joints[2];
    memset(joints, 0, sizeof(joints));

    /* Root */
    strncpy(joints[0].name, "root", sizeof(joints[0].name) - 1);
    joints[0].parent              = -1;
    joints[0].inverse_bind_matrix = jce_m4_identity();
    joints[0].local_transform     = jce_m4_identity();
    joints[0].rest_translation    = jce_v3(0.0f, 0.0f, 0.0f);
    joints[0].rest_rotation       = jce_q_identity();
    joints[0].rest_scale          = jce_v3(1.0f, 1.0f, 1.0f);

    /* Child */
    strncpy(joints[1].name, "child", sizeof(joints[1].name) - 1);
    joints[1].parent              = 0;
    joints[1].inverse_bind_matrix = jce_m4_identity();
    joints[1].local_transform     = jce_m4_identity();
    joints[1].rest_translation    = jce_v3(0.0f, 1.0f, 0.0f);
    joints[1].rest_rotation       = jce_q_identity();
    joints[1].rest_scale          = jce_v3(1.0f, 1.0f, 1.0f);

    return jce_skeleton_create(joints, 2);
}

/* Build a clip whose ROOT joint translates linearly from z=0 to z=+1 over
 * [0, DUR]. Two keyframes, LINEAR interpolation. */
static JceAnimClip *make_root_z_clip(void)
{
    static float    ts[2]    = { 0.0f, DUR };
    static jce_vec3 trans[2] = { { 0.0f, 0.0f, 0.0f },
                                 { 0.0f, 0.0f, 1.0f } };

    JceAnimChannel ch;
    memset(&ch, 0, sizeof(ch));
    ch.joint_index   = ROOT;
    ch.target        = JCE_ANIM_TARGET_TRANSLATION;
    ch.interpolation = JCE_INTERP_LINEAR;
    ch.timestamps    = ts;
    ch.count         = 2;
    ch.translations  = trans;

    return jce_anim_clip_create("rootwalk", &ch, 1, DUR);
}

/* Maximum root yaw (radians) reached at t=DUR by make_root_yaw_clip. Kept well
 * below PI so the per-window delta never crosses the +/-PI wrap. */
#define YAW_MAX 1.0f

/* Build a clip whose ROOT joint yaws (rotates about +Y) linearly from 0 to
 * YAW_MAX over [0, DUR]. Two keyframes, LINEAR (slerp) interpolation. Because
 * both keys share the +Y axis, slerp is linear in the yaw angle. */
static JceAnimClip *make_root_yaw_clip(void)
{
    static float    ts[2] = { 0.0f, DUR };
    static jce_quat rot[2];
    rot[0] = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), 0.0f);
    rot[1] = jce_q_from_axis_angle(jce_v3(0.0f, 1.0f, 0.0f), YAW_MAX);

    JceAnimChannel ch;
    memset(&ch, 0, sizeof(ch));
    ch.joint_index   = ROOT;
    ch.target        = JCE_ANIM_TARGET_ROTATION;
    ch.interpolation = JCE_INTERP_LINEAR;
    ch.timestamps    = ts;
    ch.count         = 2;
    ch.rotations     = rot;

    return jce_anim_clip_create("rootturn", &ch, 1, DUR);
}

/* Local copy of the engine's quat_yaw convention (the engine helper is static):
 * heading about +Y, atan2(siny, cosy). */
static float test_quat_yaw(jce_quat q)
{
    float siny = 2.0f * (q.w * q.y + q.x * q.z);
    float cosy = 1.0f - 2.0f * (q.y * q.y + q.x * q.x);
    return atan2f(siny, cosy);
}

/* ------------------------------------------------------------------ */
/* Pure extractor                                                      */
/* ------------------------------------------------------------------ */

/* Per-frame Z step over an interior, non-wrapping window equals the linear
 * slope (1.0 / DUR) * window. */
static void test_extract_per_frame_z_step(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_z_clip();
    TEST_ASSERT_NOT_NULL(sk);
    TEST_ASSERT_NOT_NULL(clip);

    const float t0 = 0.20f, t1 = 0.30f;     /* adjacent sample times */
    JceAnimRootDelta d = jce_anim_extract_root_delta(
        clip, sk, ROOT, t0, t1, /*loop*/false, JCE_ROOT_MOTION_NONE, NULL, 0);

    TEST_ASSERT_TRUE(d.valid);
    /* Slope is 1.0 Z per DUR seconds, window is (t1-t0). */
    float expect_z = (1.0f / DUR) * (t1 - t0);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,     d.translation.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,     d.translation.y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, expect_z, d.translation.z);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f,     d.yaw_delta);

    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* The sum of per-frame deltas across the whole clip equals the full +1.0 Z. */
static void test_extract_sum_equals_full_translation(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_z_clip();

    float sum = 0.0f;
    const int N = 50;
    for (int i = 0; i < N; i++) {
        float a = (DUR * i)       / N;
        float b = (DUR * (i + 1)) / N;
        JceAnimRootDelta d = jce_anim_extract_root_delta(
            clip, sk, ROOT, a, b, false, JCE_ROOT_MOTION_NONE, NULL, 0);
        TEST_ASSERT_TRUE(d.valid);
        sum += d.translation.z;
    }
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, sum);

    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* Loop wrap: when cur_time < prev_time (the playhead crossed the clip end),
 * the accumulated delta is (end - prev) + (cur - start), NOT (cur - prev). */
static void test_extract_loop_wrap_accumulation(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_z_clip();

    /* Step the playhead forward by 0.2 across the boundary:
     * prev = 0.9, cur wraps to 0.1.  The true forward step is 0.2 of Z. */
    const float prev = 0.9f, cur = 0.1f;
    JceAnimRootDelta d = jce_anim_extract_root_delta(
        clip, sk, ROOT, prev, cur, /*loop*/true, JCE_ROOT_MOTION_NONE, NULL, 0);

    TEST_ASSERT_TRUE(d.valid);
    /* (end z=1.0 - prev z=0.9) + (cur z=0.1 - start z=0.0) = 0.1 + 0.1 = 0.2 */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.2f, d.translation.z);

    /* Sanity: WITHOUT the loop flag the same args read as a big backward jump
     * (cur - prev) = 0.1 - 0.9 = -0.8, proving the wrap branch is doing work. */
    JceAnimRootDelta nd = jce_anim_extract_root_delta(
        clip, sk, ROOT, prev, cur, /*loop*/false, JCE_ROOT_MOTION_NONE, NULL, 0);
    TEST_ASSERT_TRUE(nd.valid);
    TEST_ASSERT_FLOAT_WITHIN(EPS, -0.8f, nd.translation.z);

    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* Re-centering: with JCE_ROOT_MOTION_RECENTER the root translation is stripped
 * from the supplied (already-sampled) pose, while the delta is still reported. */
static void test_extract_recenter_removes_root_translation(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_z_clip();

    /* Sample the pose at cur_time through the REAL sampler. */
    const float prev = 0.40f, cur = 0.60f;
    jce_mat4 pose[2];
    pose[0] = jce_m4_identity();
    pose[1] = jce_m4_identity();
    const jce_vec3 *rt = NULL; const jce_quat *rr = NULL; const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(sk, &rt, &rr, &rs);
    jce_anim_clip_sample(clip, cur, pose, 2, rt, rr, rs);

    /* Before re-centering the root pose carries z = cur (0.6). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, cur, pose[ROOT].raw[3][2]);

    JceAnimRootDelta d = jce_anim_extract_root_delta(
        clip, sk, ROOT, prev, cur, false,
        JCE_ROOT_MOTION_RECENTER, pose, 2);

    TEST_ASSERT_TRUE(d.valid);
    /* Delta still measured: (cur - prev) = 0.2 of Z. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, cur - prev, d.translation.z);
    /* Root translation stripped from the pose. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, pose[ROOT].raw[3][0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, pose[ROOT].raw[3][1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, pose[ROOT].raw[3][2]);

    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* Root ROTATION channel: the extractor must (a) report the per-window yaw_delta
 * and (b) when re-centering, strip the root +Y yaw from the pose so the runtime
 * (which turns the entity by yaw_delta) is the SOLE heading source — otherwise
 * the heading is double-applied. */
static void test_extract_recenter_strips_root_yaw(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_yaw_clip();
    TEST_ASSERT_NOT_NULL(sk);
    TEST_ASSERT_NOT_NULL(clip);

    /* Sample the pose at cur_time through the REAL sampler. */
    const float prev = 0.20f, cur = 0.40f;
    jce_mat4 pose[2];
    pose[0] = jce_m4_identity();
    pose[1] = jce_m4_identity();
    const jce_vec3 *rt = NULL; const jce_quat *rr = NULL; const jce_vec3 *rs = NULL;
    jce_skeleton_rest_trs(sk, &rt, &rr, &rs);
    jce_anim_clip_sample(clip, cur, pose, 2, rt, rr, rs);

    /* Before re-centering the root pose carries yaw = YAW_MAX * cur/DUR. */
    jce_quat r_before = jce_m4_to_quat(&pose[ROOT]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, YAW_MAX * (cur / DUR), test_quat_yaw(r_before));

    JceAnimRootDelta d = jce_anim_extract_root_delta(
        clip, sk, ROOT, prev, cur, false,
        JCE_ROOT_MOTION_RECENTER, pose, 2);

    TEST_ASSERT_TRUE(d.valid);
    /* (a) Per-window yaw delta is the linear slope over the window. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, YAW_MAX * ((cur - prev) / DUR), d.yaw_delta);
    /* Pure-yaw clip → no translation. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d.translation.x);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d.translation.y);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d.translation.z);

    /* (b) The re-centered pose root has NO yaw left (it would otherwise be
     * double-applied with the entity-side rm_dyaw). */
    jce_quat r_after = jce_m4_to_quat(&pose[ROOT]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, test_quat_yaw(r_after));

    /* And translation is still stripped. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, pose[ROOT].raw[3][0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, pose[ROOT].raw[3][1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, pose[ROOT].raw[3][2]);

    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* NULL clip is handled gracefully (valid=false, zero delta). */
static void test_extract_null_clip_safe(void)
{
    JceAnimRootDelta d = jce_anim_extract_root_delta(
        NULL, NULL, ROOT, 0.0f, 0.1f, false, JCE_ROOT_MOTION_NONE, NULL, 0);
    TEST_ASSERT_FALSE(d.valid);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d.translation.z);
}

/* ------------------------------------------------------------------ */
/* Player end-to-end (the wired dynamic path)                          */
/* ------------------------------------------------------------------ */

/* Drive a real player with root motion enabled: the consumed delta over one
 * update equals the advanced window's Z step, and the produced skinning matrix
 * for the root is re-centered (no root translation leaks into the pose). */
static void test_player_root_motion_end_to_end(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_z_clip();
    JceAnimPlayer *p  = jce_anim_player_create(sk);
    TEST_ASSERT_NOT_NULL(p);

    jce_anim_player_set_root_motion(p, true, ROOT);
    jce_anim_player_play(p, clip, /*loop*/true, /*speed*/1.0f);

    /* Set the playhead to a known interior time, then advance by a fixed dt so
     * the (prev,cur] window is deterministic. */
    jce_anim_player_set_time(p, 0.30f);

    jce_mat4 palette[2];
    const float dt = 0.10f;

    /* First update after a seek establishes the root-motion BASELINE: it
     * re-centers the pose but must report a ZERO delta (no spurious jump across
     * the seek discontinuity). */
    uint32_t n0 = jce_anim_player_update(p, dt, palette, 2);  /* 0.30 -> 0.40 */
    TEST_ASSERT_TRUE(n0 >= 1u);
    JceAnimRootDelta d0 = jce_anim_player_consume_root_motion(p);
    TEST_ASSERT_TRUE(d0.valid);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d0.translation.z);

    /* Second update reports the real per-frame delta. */
    uint32_t n = jce_anim_player_update(p, dt, palette, 2);   /* 0.40 -> 0.50 */
    TEST_ASSERT_TRUE(n >= 1u);

    JceAnimRootDelta d = jce_anim_player_consume_root_motion(p);
    TEST_ASSERT_TRUE(d.valid);
    /* Z step over a 0.10s window at slope 1.0/DUR. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, (1.0f / DUR) * dt, d.translation.z);

    /* Re-centering: with an identity skeleton the root skinning matrix is the
     * re-centered local transform; its translation column must be ~0 even
     * though the clip authored z=0.50 at this time. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, palette[ROOT].raw[3][2]);

    /* A second consume with no intervening update reports zero (one-shot). */
    JceAnimRootDelta d2 = jce_anim_player_consume_root_motion(p);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, d2.translation.z);

    jce_anim_player_destroy(p);
    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* With root motion DISABLED the player pose is unchanged (root translation is
 * preserved) — proves the apply_root_motion=false path is a no-op. */
static void test_player_disabled_preserves_pose(void)
{
    JceSkeleton *sk   = make_skeleton();
    JceAnimClip *clip = make_root_z_clip();
    JceAnimPlayer *p  = jce_anim_player_create(sk);

    /* root motion left disabled (default) */
    jce_anim_player_play(p, clip, true, 1.0f);
    jce_anim_player_set_time(p, 0.30f);

    jce_mat4 palette[2];
    (void)jce_anim_player_update(p, 0.10f, palette, 2);

    /* Root translation is PRESERVED at the authored z=0.40 (identity skeleton
     * → skin matrix translation == local translation). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.40f, palette[ROOT].raw[3][2]);

    /* consume returns invalid when disabled. */
    JceAnimRootDelta d = jce_anim_player_consume_root_motion(p);
    TEST_ASSERT_FALSE(d.valid);

    jce_anim_player_destroy(p);
    jce_anim_clip_destroy(clip);
    jce_skeleton_destroy(sk);
}

/* ------------------------------------------------------------------ */
/* runner                                                              */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_extract_per_frame_z_step);
    RUN_TEST(test_extract_sum_equals_full_translation);
    RUN_TEST(test_extract_loop_wrap_accumulation);
    RUN_TEST(test_extract_recenter_removes_root_translation);
    RUN_TEST(test_extract_recenter_strips_root_yaw);
    RUN_TEST(test_extract_null_clip_safe);
    RUN_TEST(test_player_root_motion_end_to_end);
    RUN_TEST(test_player_disabled_preserves_pose);
    return UNITY_END();
}
