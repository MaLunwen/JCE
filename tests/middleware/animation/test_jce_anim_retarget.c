/*
 * test_jce_anim_retarget.c — Unit tests for animation retargeting (L4).
 *
 * Exercises the REAL retarget path (jce_anim_retarget_map_create /
 * jce_anim_retarget_pose) against REAL skeletons built via the internal
 * jce_skeleton_create constructor.  Headless: no GPU / physics / scene.
 *
 * Cases:
 *  1. IDENTITY  — same bone names + same bind pose: a rotation-only src pose
 *                 retargets byte-equivalently to the dst (bind-relative
 *                 transfer reduces to a copy).
 *  2. NAME-MAP  — dst has the same bones in a DIFFERENT order plus one extra
 *                 unmatched bone: each dst bone gets its same-named src pose;
 *                 the unmatched dst bone stays at dst rest.
 *  3. DIFF-BIND — dst bone has a different rest_rotation than src's; a known
 *                 src animated rotation must produce the bind-pose-relative
 *                 result computed by hand (NOT a naive copy).
 *  4. END-TO-END— feed retargeted dst locals into jce_skeleton_evaluate and
 *                 assert finite, valid skinning matrices (no NaN).
 *
 * Links jce_core + jce_animation; jce_skeleton_create / jce_anim_retarget live
 * in src-side headers, so the test pulls in engine/src via its include dir.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_math.h>

/* Internal retarget header (mirrors how test_jce_root_motion includes the
 * internal animation header off the engine/src include path). */
#include "middleware/animation/jce_anim_retarget.h"

#include <math.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

#define EPS 1e-4f

/* ------------------------------------------------------------------ */
/* Skeleton builder helpers                                            */
/* ------------------------------------------------------------------ */

static void set_joint(JceJoint *j, const char *name, int16_t parent,
                      jce_vec3 t, jce_quat r, jce_vec3 s)
{
    memset(j, 0, sizeof(*j));
    strncpy(j->name, name, sizeof(j->name) - 1);
    j->parent              = parent;
    j->rest_translation    = t;
    j->rest_rotation       = r;
    j->rest_scale          = s;
    j->local_transform     = jce_m4_from_trs(t, r, s);
    j->inverse_bind_matrix = jce_m4_identity();
}

static void assert_quat_near(jce_quat expected, jce_quat actual, float eps)
{
    /* q and -q are the same rotation; pick the closest sign before compare. */
    float dot = expected.x * actual.x + expected.y * actual.y +
                expected.z * actual.z + expected.w * actual.w;
    if (dot < 0.0f) {
        actual = jce_v4(-actual.x, -actual.y, -actual.z, -actual.w);
    }
    TEST_ASSERT_FLOAT_WITHIN(eps, expected.x, actual.x);
    TEST_ASSERT_FLOAT_WITHIN(eps, expected.y, actual.y);
    TEST_ASSERT_FLOAT_WITHIN(eps, expected.z, actual.z);
    TEST_ASSERT_FLOAT_WITHIN(eps, expected.w, actual.w);
}

static int finite_mat(const jce_mat4 *m)
{
    const float *f = &m->raw[0][0];
    for (int i = 0; i < 16; ++i)
        if (!isfinite(f[i])) return 0;
    return 1;
}

/* ------------------------------------------------------------------ */
/* 1. IDENTITY                                                         */
/* ------------------------------------------------------------------ */

static void test_identity_same_bind_copies_pose(void)
{
    /* Two 3-joint skeletons with IDENTICAL names and bind pose. */
    JceJoint joints[3];
    set_joint(&joints[0], "hips",  -1, jce_v3(0,1,0), jce_q_identity(),
              jce_v3(1,1,1));
    set_joint(&joints[1], "spine",  0, jce_v3(0,0.5f,0), jce_q_identity(),
              jce_v3(1,1,1));
    set_joint(&joints[2], "head",   1, jce_v3(0,0.5f,0), jce_q_identity(),
              jce_v3(1,1,1));

    JceSkeleton *src = jce_skeleton_create(joints, 3);
    JceSkeleton *dst = jce_skeleton_create(joints, 3);
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceAnimRetargetMap *map = jce_anim_retarget_map_create(src, dst);
    TEST_ASSERT_NOT_NULL(map);
    TEST_ASSERT_EQUAL_UINT32(3u, jce_anim_retarget_mapped_count(map));

    /* A rotation-only animated source pose (translations/scales == bind). */
    jce_quat spin = jce_q_from_axis_angle(jce_v3(0,1,0), 0.7f);
    jce_mat4 src_locals[3];
    src_locals[0] = jce_m4_from_trs(jce_v3(0,1,0), spin, jce_v3(1,1,1));      /* root */
    src_locals[1] = jce_m4_from_trs(jce_v3(0,0.5f,0),
                                    jce_q_from_axis_angle(jce_v3(1,0,0), 0.3f),
                                    jce_v3(1,1,1));
    src_locals[2] = jce_m4_from_trs(jce_v3(0,0.5f,0), jce_q_identity(),
                                    jce_v3(1,1,1));

    jce_mat4 dst_locals[3];
    jce_anim_retarget_pose(map, src_locals, dst_locals);

    /* Same bind => rotation transfer is a copy; translations/scales match. */
    for (uint32_t i = 0; i < 3; ++i) {
        assert_quat_near(jce_m4_to_quat(&src_locals[i]),
                         jce_m4_to_quat(&dst_locals[i]), EPS);
        for (int k = 0; k < 3; ++k)
            TEST_ASSERT_FLOAT_WITHIN(EPS, src_locals[i].raw[3][k],
                                     dst_locals[i].raw[3][k]);
    }

    jce_anim_retarget_map_destroy(map);
    jce_skeleton_destroy(src);
    jce_skeleton_destroy(dst);
}

/* ------------------------------------------------------------------ */
/* 2. NAME-MAP (different order + extra unmatched bone)                */
/* ------------------------------------------------------------------ */

static void test_name_map_reorder_and_unmatched(void)
{
    /* SRC order: hips, spine, head. */
    JceJoint sj[3];
    set_joint(&sj[0], "hips",  -1, jce_v3(0,1,0), jce_q_identity(), jce_v3(1,1,1));
    set_joint(&sj[1], "spine",  0, jce_v3(0,0.4f,0), jce_q_identity(), jce_v3(1,1,1));
    set_joint(&sj[2], "head",   1, jce_v3(0,0.6f,0), jce_q_identity(), jce_v3(1,1,1));
    JceSkeleton *src = jce_skeleton_create(sj, 3);

    /* DST order: hips, head, spine + an extra unmatched "tail". Parent links
     * keep parent-before-child ordering (tail parents to head). */
    JceJoint dj[4];
    set_joint(&dj[0], "hips",  -1, jce_v3(0,1,0), jce_q_identity(), jce_v3(1,1,1));
    set_joint(&dj[1], "spine",  0, jce_v3(0,0.4f,0), jce_q_identity(), jce_v3(1,1,1));
    set_joint(&dj[2], "head",   1, jce_v3(0,0.6f,0), jce_q_identity(), jce_v3(1,1,1));
    set_joint(&dj[3], "tail",   2, jce_v3(0,0.2f,0), jce_q_identity(), jce_v3(1,1,1));
    JceSkeleton *dst = jce_skeleton_create(dj, 4);

    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceAnimRetargetMap *map = jce_anim_retarget_map_create(src, dst);
    TEST_ASSERT_NOT_NULL(map);
    /* 3 of 4 dst joints matched; "tail" unmatched. */
    TEST_ASSERT_EQUAL_UINT32(3u, jce_anim_retarget_mapped_count(map));
    TEST_ASSERT_EQUAL_INT(0,  jce_anim_retarget_source_of(map, 0)); /* hips<-hips  */
    TEST_ASSERT_EQUAL_INT(1,  jce_anim_retarget_source_of(map, 1)); /* spine<-spine*/
    TEST_ASSERT_EQUAL_INT(2,  jce_anim_retarget_source_of(map, 2)); /* head<-head  */
    TEST_ASSERT_EQUAL_INT(-1, jce_anim_retarget_source_of(map, 3)); /* tail none   */

    /* Distinct rotation per source bone so we can tell them apart. */
    jce_quat r_hips  = jce_q_from_axis_angle(jce_v3(0,1,0), 0.5f);
    jce_quat r_spine = jce_q_from_axis_angle(jce_v3(1,0,0), 0.25f);
    jce_quat r_head  = jce_q_from_axis_angle(jce_v3(0,0,1), 0.9f);

    jce_mat4 src_locals[3];
    src_locals[0] = jce_m4_from_trs(jce_v3(0,1,0),    r_hips,  jce_v3(1,1,1));
    src_locals[1] = jce_m4_from_trs(jce_v3(0,0.4f,0), r_spine, jce_v3(1,1,1));
    src_locals[2] = jce_m4_from_trs(jce_v3(0,0.6f,0), r_head,  jce_v3(1,1,1));

    jce_mat4 dst_locals[4];
    jce_anim_retarget_pose(map, src_locals, dst_locals);

    /* Same bind orientation => each matched dst bone == its same-named src
     * rotation, regardless of index order. */
    assert_quat_near(r_hips,  jce_m4_to_quat(&dst_locals[0]), EPS);
    assert_quat_near(r_spine, jce_m4_to_quat(&dst_locals[1]), EPS);
    assert_quat_near(r_head,  jce_m4_to_quat(&dst_locals[2]), EPS);

    /* Unmatched "tail" stays at dst rest (identity rotation, bind translation). */
    assert_quat_near(jce_q_identity(), jce_m4_to_quat(&dst_locals[3]), EPS);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.2f, dst_locals[3].raw[3][1]);

    jce_anim_retarget_map_destroy(map);
    jce_skeleton_destroy(src);
    jce_skeleton_destroy(dst);
}

/* ------------------------------------------------------------------ */
/* 3. DIFFERENT-BIND (proves it is NOT a naive copy)                   */
/* ------------------------------------------------------------------ */

static void test_different_bind_uses_bind_relative_transfer(void)
{
    /* One shared-name bone "arm" with DIFFERENT rest rotations. */
    jce_quat src_bind = jce_q_from_axis_angle(jce_v3(0,0,1),  0.6f);
    jce_quat dst_bind = jce_q_from_axis_angle(jce_v3(0,0,1), -0.4f);

    JceJoint sj[1];
    set_joint(&sj[0], "arm", -1, jce_v3(0,0,0), src_bind, jce_v3(1,1,1));
    JceSkeleton *src = jce_skeleton_create(sj, 1);

    JceJoint dj[1];
    set_joint(&dj[0], "arm", -1, jce_v3(0,0,0), dst_bind, jce_v3(1,1,1));
    JceSkeleton *dst = jce_skeleton_create(dj, 1);

    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceAnimRetargetMap *map = jce_anim_retarget_map_create(src, dst);
    TEST_ASSERT_NOT_NULL(map);

    /* A known animated source local rotation. */
    jce_quat r_src_anim = jce_q_from_axis_angle(jce_v3(0,0,1), 1.2f);
    jce_mat4 src_locals[1];
    src_locals[0] = jce_m4_from_trs(jce_v3(0,0,0), r_src_anim, jce_v3(1,1,1));

    jce_mat4 dst_locals[1];
    jce_anim_retarget_pose(map, src_locals, dst_locals);

    /* Expected (computed by hand here, mirroring the formula):
     *   R_dst = R_dst_bind * conj(R_src_bind) * R_src_anim          */
    jce_quat conj_src = jce_v4(-src_bind.x, -src_bind.y, -src_bind.z, src_bind.w);
    jce_quat rel      = jce_q_multiply(conj_src, r_src_anim);
    jce_quat expected = jce_q_normalize(jce_q_multiply(dst_bind, rel));

    jce_quat got = jce_m4_to_quat(&dst_locals[0]);
    assert_quat_near(expected, got, EPS);

    /* And prove it's NOT a naive copy: differs from the raw source rotation. */
    float dot_naive = r_src_anim.x * got.x + r_src_anim.y * got.y +
                      r_src_anim.z * got.z + r_src_anim.w * got.w;
    TEST_ASSERT_TRUE(fabsf(fabsf(dot_naive) - 1.0f) > 1e-3f);

    jce_anim_retarget_map_destroy(map);
    jce_skeleton_destroy(src);
    jce_skeleton_destroy(dst);
}

/* ------------------------------------------------------------------ */
/* 4. END-TO-END (skinning matrices finite)                           */
/* ------------------------------------------------------------------ */

static void test_end_to_end_skinning_finite(void)
{
    JceJoint sj[3];
    set_joint(&sj[0], "hips",  -1, jce_v3(0,1,0), jce_q_identity(), jce_v3(1,1,1));
    set_joint(&sj[1], "spine",  0, jce_v3(0,0.5f,0),
              jce_q_from_axis_angle(jce_v3(1,0,0), 0.1f), jce_v3(1,1,1));
    set_joint(&sj[2], "head",   1, jce_v3(0,0.5f,0), jce_q_identity(), jce_v3(1,1,1));
    JceSkeleton *src = jce_skeleton_create(sj, 3);

    /* Target with different bind orientations + different limb lengths. */
    JceJoint dj[3];
    set_joint(&dj[0], "hips",  -1, jce_v3(0,1.3f,0),
              jce_q_from_axis_angle(jce_v3(0,1,0), 0.2f), jce_v3(1,1,1));
    set_joint(&dj[1], "spine",  0, jce_v3(0,0.7f,0),
              jce_q_from_axis_angle(jce_v3(1,0,0), 0.4f), jce_v3(1,1,1));
    set_joint(&dj[2], "head",   1, jce_v3(0,0.6f,0), jce_q_identity(), jce_v3(1,1,1));
    JceSkeleton *dst = jce_skeleton_create(dj, 3);

    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceAnimRetargetMap *map = jce_anim_retarget_map_create(src, dst);
    TEST_ASSERT_NOT_NULL(map);

    jce_mat4 src_locals[3];
    src_locals[0] = jce_m4_from_trs(jce_v3(0.1f,1.0f,0.2f),
                                    jce_q_from_axis_angle(jce_v3(0,1,0), 0.8f),
                                    jce_v3(1,1,1));
    src_locals[1] = jce_m4_from_trs(jce_v3(0,0.5f,0),
                                    jce_q_from_axis_angle(jce_v3(1,0,0), 0.5f),
                                    jce_v3(1,1,1));
    src_locals[2] = jce_m4_from_trs(jce_v3(0,0.5f,0),
                                    jce_q_from_axis_angle(jce_v3(0,0,1), 0.3f),
                                    jce_v3(1,1,1));

    jce_mat4 dst_locals[3];
    jce_anim_retarget_pose(map, src_locals, dst_locals);

    /* Retargeted locals must be finite. */
    for (uint32_t i = 0; i < 3; ++i)
        TEST_ASSERT_TRUE(finite_mat(&dst_locals[i]));

    /* Feed into skeleton evaluation -> skinning matrices must be finite. */
    jce_mat4 skin[3];
    jce_skeleton_evaluate(dst, dst_locals, skin, 3);
    for (uint32_t i = 0; i < 3; ++i)
        TEST_ASSERT_TRUE(finite_mat(&skin[i]));

    jce_anim_retarget_map_destroy(map);
    jce_skeleton_destroy(src);
    jce_skeleton_destroy(dst);
}

/* ------------------------------------------------------------------ */
/* null-safety                                                         */
/* ------------------------------------------------------------------ */

static void test_null_safety(void)
{
    TEST_ASSERT_NULL(jce_anim_retarget_map_create(NULL, NULL));
    jce_anim_retarget_map_destroy(NULL);   /* must not crash */
    jce_anim_retarget_pose(NULL, NULL, NULL);
    TEST_ASSERT_EQUAL_UINT32(0u, jce_anim_retarget_mapped_count(NULL));
    TEST_ASSERT_EQUAL_INT(-1, jce_anim_retarget_source_of(NULL, 0));
    TEST_PASS();
}

/* ------------------------------------------------------------------ */
/* runner                                                             */
/* ------------------------------------------------------------------ */

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identity_same_bind_copies_pose);
    RUN_TEST(test_name_map_reorder_and_unmatched);
    RUN_TEST(test_different_bind_uses_bind_relative_transfer);
    RUN_TEST(test_end_to_end_skinning_finite);
    RUN_TEST(test_null_safety);
    return UNITY_END();
}
