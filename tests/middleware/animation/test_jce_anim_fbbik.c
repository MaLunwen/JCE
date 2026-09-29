/*
 * test_jce_anim_fbbik.c — Full-Body IK solver core (FABRIK on a tree).
 *
 * 100% headless + deterministic.  Builds position-node bodies, pulls effectors,
 * and asserts the FABRIK-tree contract:
 *   (1) a reachable single-chain effector reaches its target;
 *   (2) an unreachable target -> the chain extends straight toward it (max reach);
 *   (3) two effectors on a shared-spine tree both move toward their targets,
 *       symmetrically, and error is reduced;
 *   (4) weight 0 -> no movement;
 *   (5) every solve preserves bone lengths (rigidity);
 *   (6) compute_lengths derives rest distances;
 *   (7) NULL / degenerate safety.
 */

#include "unity.h"

#include <jce/middleware/animation/jce_anim_fbbik.h>
#include <jce/middleware/animation/jce_skeleton.h>
#include <jce/os/core/jce_math.h>

#include <math.h>
#include <stdio.h>
#include <string.h>

#define TOL 1e-3f

void setUp(void)    {}
void tearDown(void) {}

/* Identity + translation in col[3] (FBBIK reads col[3] as the joint position). */
static jce_mat4 mat_t(float x, float y, float z)
{
    jce_mat4 m;
    memset(&m, 0, sizeof m);
    m.col[0].x = 1.0f; m.col[1].y = 1.0f; m.col[2].z = 1.0f; m.col[3].w = 1.0f;
    m.col[3].x = x; m.col[3].y = y; m.col[3].z = z;
    return m;
}

static float dist(jce_vec3 a, jce_vec3 b)
{
    float dx = a.x - b.x, dy = a.y - b.y, dz = a.z - b.z;
    return sqrtf(dx * dx + dy * dy + dz * dz);
}

/* Build a straight chain of `n` nodes along +X (unit bones), root at origin. */
static void make_chain(JceFbbikBody *b, int n)
{
    memset(b, 0, sizeof *b);
    b->node_count = n;
    for (int i = 0; i < n; ++i) {
        b->positions[i] = jce_v3((float)i, 0.0f, 0.0f);
        b->parents[i]   = i - 1;     /* node 0 -> -1 (root) */
    }
    jce_anim_fbbik_compute_lengths(b);
}

static void assert_lengths_preserved(const JceFbbikBody *b, const float *orig_len)
{
    for (int i = 1; i < b->node_count; ++i) {
        float l = dist(b->positions[i], b->positions[b->parents[i]]);
        TEST_ASSERT_FLOAT_WITHIN(1e-3f, orig_len[i], l);
    }
}

/* ── (1) reachable single-chain effector reaches target ────────────────── */
static void test_reachable_single_chain(void)
{
    JceFbbikBody b; make_chain(&b, 3);           /* reach = 2 */
    float ol[JCE_FBBIK_MAX_NODES];
    memcpy(ol, b.lengths, sizeof ol);

    JceFbbikEffector e = { 2, jce_v3(1.0f, 1.0f, 0.0f), 1.0f }; /* dist 1.414 < 2 */
    int it = jce_anim_fbbik_solve(&b, &e, 1, 30, TOL);
    TEST_ASSERT_TRUE(it >= 1);
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 0.0f, dist(b.positions[2], e.target));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(b.positions[0], jce_v3(0,0,0))); /* root anchored */
    assert_lengths_preserved(&b, ol);
}

/* ── (2) unreachable target -> straight extension toward it ─────────────── */
static void test_unreachable_extends(void)
{
    JceFbbikBody b; make_chain(&b, 3);           /* reach = 2 */
    float ol[JCE_FBBIK_MAX_NODES];
    memcpy(ol, b.lengths, sizeof ol);

    JceFbbikEffector e = { 2, jce_v3(10.0f, 0.0f, 0.0f), 1.0f }; /* far past reach */
    jce_anim_fbbik_solve(&b, &e, 1, 30, TOL);

    /* End sits at max reach (2) from the root, along +X toward the target. */
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 2.0f, dist(b.positions[2], jce_v3(0,0,0)));
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 2.0f, b.positions[2].x);
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 0.0f, b.positions[2].y);
    assert_lengths_preserved(&b, ol);
}

/* ── (3) two effectors on a shared-spine tree ──────────────────────────── */
static void test_two_effectors_shared_spine(void)
{
    /* node0 root(0,0,0); node1 spine(0,1,0); node2 left(-1,1,0); node3 right(1,1,0). */
    JceFbbikBody b;
    memset(&b, 0, sizeof b);
    b.node_count = 4;
    b.positions[0] = jce_v3( 0, 0, 0); b.parents[0] = -1;
    b.positions[1] = jce_v3( 0, 1, 0); b.parents[1] = 0;
    b.positions[2] = jce_v3(-1, 1, 0); b.parents[2] = 1;
    b.positions[3] = jce_v3( 1, 1, 0); b.parents[3] = 1;
    jce_anim_fbbik_compute_lengths(&b);
    float ol[JCE_FBBIK_MAX_NODES];
    memcpy(ol, b.lengths, sizeof ol);

    jce_vec3 tL = jce_v3(-0.5f, 1.5f, 0.0f);
    jce_vec3 tR = jce_v3( 0.5f, 1.5f, 0.0f);
    float e0L = dist(b.positions[2], tL);
    float e0R = dist(b.positions[3], tR);

    JceFbbikEffector es[2] = {
        { 2, tL, 1.0f },
        { 3, tR, 1.0f },
    };
    jce_anim_fbbik_solve(&b, es, 2, 40, TOL);

    float eL = dist(b.positions[2], tL);
    float eR = dist(b.positions[3], tR);
    TEST_ASSERT_TRUE(eL < e0L);                 /* both got closer */
    TEST_ASSERT_TRUE(eR < e0R);
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, eL, eR);    /* symmetric setup -> symmetric error */
    assert_lengths_preserved(&b, ol);
}

/* ── (4) weight 0 -> no movement ───────────────────────────────────────── */
static void test_zero_weight_no_move(void)
{
    JceFbbikBody b; make_chain(&b, 3);
    jce_vec3 before2 = b.positions[2];

    JceFbbikEffector e = { 2, jce_v3(1.0f, 1.0f, 0.0f), 0.0f };
    jce_anim_fbbik_solve(&b, &e, 1, 30, TOL);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, dist(b.positions[2], before2));
}

/* ── (6) compute_lengths derives rest distances ────────────────────────── */
static void test_compute_lengths(void)
{
    JceFbbikBody b;
    memset(&b, 0, sizeof b);
    b.node_count = 3;
    b.positions[0] = jce_v3(0, 0, 0); b.parents[0] = -1;
    b.positions[1] = jce_v3(0, 3, 0); b.parents[1] = 0;   /* len 3 */
    b.positions[2] = jce_v3(4, 3, 0); b.parents[2] = 1;   /* len 4 */
    jce_anim_fbbik_compute_lengths(&b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, b.lengths[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 3.0f, b.lengths[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 4.0f, b.lengths[2]);
}

/* ── (7) NULL / degenerate safety ──────────────────────────────────────── */
static void test_null_safe(void)
{
    JceFbbikEffector e = { 0, { 0, 0, 0 }, 1.0f };
    TEST_ASSERT_EQUAL_INT(0, jce_anim_fbbik_solve(NULL, &e, 1, 10, TOL));

    JceFbbikBody b; memset(&b, 0, sizeof b);
    b.node_count = 0;
    TEST_ASSERT_EQUAL_INT(0, jce_anim_fbbik_solve(&b, &e, 1, 10, TOL));

    jce_anim_fbbik_compute_lengths(NULL);   /* must not crash */
}

/* Build a 3-joint chain skeleton along +X (unit bones): j0 root @origin,
 * j1 child of j0 (+1 X), j2 child of j1 (+1 X).  Rest FK -> (0,0,0)/(1,0,0)/(2,0,0). */
static JceSkeleton *make_skel_chain(void)
{
    JceJoint j[3];
    memset(j, 0, sizeof j);
    for (int i = 0; i < 3; ++i) {
        snprintf(j[i].name, sizeof j[i].name, "j%d", i);
        j[i].inverse_bind_matrix = mat_t(0, 0, 0);   /* identity (unused by FBBIK) */
        j[i].rest_rotation = (jce_quat){ 0, 0, 0, 1 };
        j[i].rest_scale    = jce_v3(1, 1, 1);
    }
    j[0].parent = -1; j[0].local_transform = mat_t(0, 0, 0); j[0].rest_translation = jce_v3(0,0,0);
    j[1].parent = 0;  j[1].local_transform = mat_t(1, 0, 0); j[1].rest_translation = jce_v3(1,0,0);
    j[2].parent = 1;  j[2].local_transform = mat_t(1, 0, 0); j[2].rest_translation = jce_v3(1,0,0);
    return jce_skeleton_create(j, 3);
}

/* ── (8) skeleton binding: rest pose FK passes through ─────────────────── */
static void test_solve_pose_rest_passthrough(void)
{
    JceSkeleton *sk = make_skel_chain();
    TEST_ASSERT_NOT_NULL(sk);
    jce_vec3 out[3];

    /* No effectors, rest pose -> output is the pure FK of the rest pose. */
    jce_anim_fbbik_solve_pose(sk, NULL, NULL, NULL, 0, out, 10, TOL);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(out[0], jce_v3(0,0,0)));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(out[1], jce_v3(1,0,0)));
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(out[2], jce_v3(2,0,0)));

    jce_skeleton_destroy(sk);
}

/* ── (9) skeleton binding: effector reaches target + bones preserved ───── */
static void test_solve_pose_effector_reaches(void)
{
    JceSkeleton *sk = make_skel_chain();
    TEST_ASSERT_NOT_NULL(sk);
    jce_vec3 out[3];

    JceFbbikSkelEffector e = { 2, jce_v3(1.0f, 1.0f, 0.0f), 1.0f }; /* reach 2 > 1.414 */
    int it = jce_anim_fbbik_solve_pose(sk, NULL, NULL, &e, 1, out, 30, TOL);
    TEST_ASSERT_TRUE(it >= 1);

    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 0.0f, dist(out[2], e.target));   /* end reaches */
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, dist(out[0], jce_v3(0,0,0))); /* root anchored */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, dist(out[0], out[1]));     /* bone 1 rigid */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 1.0f, dist(out[1], out[2]));     /* bone 2 rigid */

    jce_skeleton_destroy(sk);
}

/* ── (10) skeleton binding: NULL safety ────────────────────────────────── */
static void test_solve_pose_null_safe(void)
{
    jce_vec3 out[3];
    JceFbbikSkelEffector e = { 0, { 0,0,0 }, 1.0f };
    TEST_ASSERT_EQUAL_INT(0, jce_anim_fbbik_solve_pose(NULL, NULL, NULL, &e, 1, out, 10, TOL));
    JceSkeleton *sk = make_skel_chain();
    TEST_ASSERT_EQUAL_INT(0, jce_anim_fbbik_solve_pose(sk, NULL, NULL, &e, 1, NULL, 10, TOL));
    jce_skeleton_destroy(sk);
}

/* ── (11) write-back: joints land at solved positions + bones re-orient ── */
static void test_write_back_orients_bones(void)
{
    JceSkeleton *sk = make_skel_chain();
    TEST_ASSERT_NOT_NULL(sk);

    /* Pre-solve: straight along +X.  Solved: bent 90 deg up (+Y). */
    jce_mat4 old_g[3] = { mat_t(0,0,0), mat_t(1,0,0), mat_t(2,0,0) };
    jce_vec3 solved[3] = { jce_v3(0,0,0), jce_v3(0,1,0), jce_v3(0,2,0) };
    jce_mat4 out[3];

    jce_anim_fbbik_write_back(sk, old_g, solved, out);

    /* Joints sit exactly at their solved world positions. */
    for (int i = 0; i < 3; ++i) {
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, solved[i].x, out[i].col[3].x);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, solved[i].y, out[i].col[3].y);
        TEST_ASSERT_FLOAT_WITHIN(1e-4f, solved[i].z, out[i].col[3].z);
    }
    /* Bone of joint0 (old +X) re-oriented to new +Y: X-basis -> (0,1,0). */
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.0f, out[0].col[0].x);
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 1.0f, out[0].col[0].y);
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 0.0f, out[0].col[0].z);
    /* Joint1's bone likewise re-orients to +Y. */
    TEST_ASSERT_FLOAT_WITHIN(2e-3f, 1.0f, out[1].col[0].y);

    jce_skeleton_destroy(sk);
}

/* ── (12) write-back NULL safety ────────────────────────────────────────── */
static void test_write_back_null_safe(void)
{
    jce_mat4 g[1] = { mat_t(0,0,0) };
    jce_vec3 s[1] = { jce_v3(0,0,0) };
    jce_mat4 o[1];
    jce_anim_fbbik_write_back(NULL, g, s, o);   /* must not crash */
    JceSkeleton *sk = make_skel_chain();
    jce_anim_fbbik_write_back(sk, NULL, s, o);
    jce_anim_fbbik_write_back(sk, g, NULL, o);
    jce_anim_fbbik_write_back(sk, g, s, NULL);
    jce_skeleton_destroy(sk);
    TEST_PASS();
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_reachable_single_chain);
    RUN_TEST(test_unreachable_extends);
    RUN_TEST(test_two_effectors_shared_spine);
    RUN_TEST(test_zero_weight_no_move);
    RUN_TEST(test_compute_lengths);
    RUN_TEST(test_null_safe);
    RUN_TEST(test_solve_pose_rest_passthrough);
    RUN_TEST(test_solve_pose_effector_reaches);
    RUN_TEST(test_solve_pose_null_safe);
    RUN_TEST(test_write_back_orients_bones);
    RUN_TEST(test_write_back_null_safe);
    return UNITY_END();
}
