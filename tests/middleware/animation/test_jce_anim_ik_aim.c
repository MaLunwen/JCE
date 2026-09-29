/* test_jce_anim_ik_aim.c
 *
 * Pure-math unit tests for the multi-IK solvers added in FEATURE 3.6d:
 *   - jce_anim_ik_aim_solve     (single-bone aim: forward axis -> target)
 *   - jce_anim_ik_ccd_solve     (cyclic-coordinate-descent n-bone chain)
 *   - jce_anim_ik_fabrik_solve  (forward-and-backward-reaching n-bone chain)
 *
 * Each solver is state-free; we craft minimal inputs and verify the
 * geometric invariants each promises: the aim direction points at the
 * target, the chain end effectors reach a reachable target within
 * tolerance, bone lengths are preserved, and the chain clamps straight
 * toward an unreachable target at maximum reach.
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

static float v3_norm_len(const float *a)
{
    return sqrtf(a[0]*a[0] + a[1]*a[1] + a[2]*a[2]);
}

static int v3_has_nan(const float *a)
{
    return isnan(a[0]) || isnan(a[1]) || isnan(a[2]) ||
           isinf(a[0]) || isinf(a[1]) || isinf(a[2]);
}

/* ── Two-bone ───────────────────────────────────────────────────────── */

/* A zero-length first bone (root == mid) makes the law-of-cosines denominator
 * 2*L1*D zero → NaN/Inf without a guard.  Now reachable via the kind==1
 * dispatch, so the solver must degrade gracefully: leave the input joints
 * unchanged and produce no NaN. */
static void test_two_bone_zero_length_no_nan(void)
{
    JceIkTwoBoneInput in;
    memset(&in, 0, sizeof(in));
    /* root == mid (L1 == 0); end one unit further along +X. */
    in.root_pos[0]=0; in.root_pos[1]=0; in.root_pos[2]=0;
    in.mid_pos [0]=0; in.mid_pos [1]=0; in.mid_pos [2]=0;
    in.end_pos [0]=1; in.end_pos [1]=0; in.end_pos [2]=0;
    in.target  [0]=0; in.target  [1]=1; in.target  [2]=0;
    in.pole    [0]=0; in.pole    [1]=0; in.pole    [2]=1;
    in.weight  = 1.0f;

    JceIkTwoBoneOutput out;
    out.mid_pos[0]=out.mid_pos[1]=out.mid_pos[2]=9.0f;
    out.end_pos[0]=out.end_pos[1]=out.end_pos[2]=9.0f;

    int r = jce_anim_ik_two_bone_solve(&in, &out);
    TEST_ASSERT_EQUAL_INT(0, r);                 /* degenerate → not reached */
    TEST_ASSERT_FALSE(v3_has_nan(out.mid_pos));
    TEST_ASSERT_FALSE(v3_has_nan(out.end_pos));
    /* Joints left unchanged (copied straight from the input). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(out.mid_pos, in.mid_pos));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(out.end_pos, in.end_pos));
}

/* A target coincident with the root (D == 0) is likewise degenerate. */
static void test_two_bone_target_on_root_no_nan(void)
{
    JceIkTwoBoneInput in;
    memset(&in, 0, sizeof(in));
    in.root_pos[0]=0; in.root_pos[1]=0; in.root_pos[2]=0;
    in.mid_pos [0]=1; in.mid_pos [1]=0; in.mid_pos [2]=0;
    in.end_pos [0]=2; in.end_pos [1]=0; in.end_pos [2]=0;
    in.target  [0]=0; in.target  [1]=0; in.target  [2]=0;   /* == root */
    in.pole    [0]=0; in.pole    [1]=0; in.pole    [2]=1;
    in.weight  = 1.0f;

    JceIkTwoBoneOutput out;
    int r = jce_anim_ik_two_bone_solve(&in, &out);
    TEST_ASSERT_EQUAL_INT(0, r);
    TEST_ASSERT_FALSE(v3_has_nan(out.mid_pos));
    TEST_ASSERT_FALSE(v3_has_nan(out.end_pos));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(out.mid_pos, in.mid_pos));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(out.end_pos, in.end_pos));
}

/* ── Aim ────────────────────────────────────────────────────────────── */

static void test_aim_null_inputs_are_safe(void)
{
    float out[3] = {9, 9, 9};
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_aim_solve(NULL, out));
    JceIkAimInput in = {0};
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_aim_solve(&in, NULL));
}

static void test_aim_points_forward_at_target(void)
{
    /* Bone at origin currently looking +X; target is up +Y. */
    JceIkAimInput in = {
        .pivot   = {0, 0, 0},
        .forward = {1, 0, 0},
        .up      = {0, 0, 1},
        .target  = {0, 5, 0},
        .weight  = 1.0f,
    };
    float out[3];
    TEST_ASSERT_EQUAL_INT(1, jce_anim_ik_aim_solve(&in, out));

    /* Result must be unit length and point straight at the target (+Y). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, v3_norm_len(out));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[2]);
}

static void test_aim_offset_pivot_aims_at_target(void)
{
    /* Pivot not at origin; aim direction must be (target - pivot) normalised. */
    JceIkAimInput in = {
        .pivot   = {2, 1, 0},
        .forward = {0, 0, 1},
        .up      = {0, 1, 0},
        .target  = {5, 1, 0},   /* +X of pivot */
        .weight  = 1.0f,
    };
    float out[3];
    TEST_ASSERT_EQUAL_INT(1, jce_anim_ik_aim_solve(&in, out));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[2]);
}

static void test_aim_weight_zero_keeps_forward(void)
{
    JceIkAimInput in = {
        .pivot   = {0, 0, 0},
        .forward = {1, 0, 0},
        .up      = {0, 1, 0},
        .target  = {0, 5, 0},
        .weight  = 0.0f,
    };
    float out[3];
    jce_anim_ik_aim_solve(&in, out);
    /* weight 0 -> result is the (normalised) input forward. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[2]);
}

static void test_aim_target_on_pivot_is_safe(void)
{
    JceIkAimInput in = {
        .pivot   = {1, 2, 3},
        .forward = {0, 0, 1},
        .up      = {0, 1, 0},
        .target  = {1, 2, 3},   /* coincident with pivot */
        .weight  = 1.0f,
    };
    float out[3];
    /* Degenerate: returns 0 and leaves forward unchanged (normalised). */
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_aim_solve(&in, out));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out[1]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 1.0f, out[2]);
}

/* ── Chain helpers ──────────────────────────────────────────────────── */

/* A straight 3-bone chain along +X: joints at 0,1,2,3 (unit bones). */
static void make_chain4(float *j)
{
    j[0]=0; j[1]=0; j[2]=0;
    j[3]=1; j[4]=0; j[5]=0;
    j[6]=2; j[7]=0; j[8]=0;
    j[9]=3; j[10]=0; j[11]=0;
}

static void assert_lengths_preserved(const float *j, int count,
                                     const float *orig)
{
    for (int i = 0; i + 1 < count; ++i) {
        float lo = v3_distance(&orig[i*3], &orig[(i+1)*3]);
        float ln = v3_distance(&j[i*3],   &j[(i+1)*3]);
        TEST_ASSERT_FLOAT_WITHIN(5e-3f, lo, ln);
    }
}

/* ── CCD ────────────────────────────────────────────────────────────── */

static void test_ccd_null_inputs_are_safe(void)
{
    float j[6] = {0,0,0, 1,0,0};
    float tgt[3] = {0,1,0};
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_ccd_solve(NULL, 2, tgt, 16, 1e-3f));
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_ccd_solve(j, 1, tgt, 16, 1e-3f));
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_ccd_solve(j, 2, NULL, 16, 1e-3f));
}

static void test_ccd_reaches_reachable_target(void)
{
    float j[12]; make_chain4(j);
    float orig[12]; memcpy(orig, j, sizeof orig);

    /* Total reach 3; target within reach, off-axis so a real bend is needed.
     * CCD has only linear convergence and a known slow-tail/stall behaviour,
     * so the practical reach tolerance is looser than FABRIK's. */
    float tgt[3] = {1.5f, 1.5f, 0.0f};
    int iters = jce_anim_ik_ccd_solve(j, 4, tgt, 64, 1e-4f);
    TEST_ASSERT_TRUE(iters > 0);

    /* End effector lands on target within tolerance. */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.0f, v3_distance(&j[9], tgt));
    /* Root unmoved. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(&j[0], &orig[0]));
    assert_lengths_preserved(j, 4, orig);
}

static void test_ccd_clamps_unreachable_target(void)
{
    float j[12]; make_chain4(j);
    float orig[12]; memcpy(orig, j, sizeof orig);

    /* Reach 3, target at distance 10 along +Y -> straight, clamped at 3. */
    float tgt[3] = {0.0f, 10.0f, 0.0f};
    jce_anim_ik_ccd_solve(j, 4, tgt, 64, 1e-4f);

    /* End effector at max reach (3) from the root. */
    TEST_ASSERT_FLOAT_WITHIN(2e-2f, 3.0f, v3_distance(&j[0], &j[9]));
    /* Pointing toward the target (roughly +Y). */
    TEST_ASSERT_TRUE(j[10] > 2.9f);
    assert_lengths_preserved(j, 4, orig);
}

static void test_ccd_already_at_target_zero_iters(void)
{
    float j[12]; make_chain4(j);
    /* End effector already exactly on the target. */
    float tgt[3] = {3.0f, 0.0f, 0.0f};
    int iters = jce_anim_ik_ccd_solve(j, 4, tgt, 16, 1e-3f);
    TEST_ASSERT_EQUAL_INT(0, iters);
}

/* ── FABRIK ─────────────────────────────────────────────────────────── */

static void test_fabrik_null_inputs_are_safe(void)
{
    float j[6] = {0,0,0, 1,0,0};
    float tgt[3] = {0,1,0};
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_fabrik_solve(NULL, 2, tgt, 16, 1e-3f));
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_fabrik_solve(j, 1, tgt, 16, 1e-3f));
    TEST_ASSERT_EQUAL_INT(0, jce_anim_ik_fabrik_solve(j, 2, NULL, 16, 1e-3f));
}

static void test_fabrik_reaches_reachable_target(void)
{
    float j[12]; make_chain4(j);
    float orig[12]; memcpy(orig, j, sizeof orig);

    float tgt[3] = {1.0f, 2.0f, 0.5f};   /* dist ~2.29 < reach 3 */
    int iters = jce_anim_ik_fabrik_solve(j, 4, tgt, 64, 1e-4f);
    TEST_ASSERT_TRUE(iters > 0);

    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 0.0f, v3_distance(&j[9], tgt));
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(&j[0], &orig[0]));
    assert_lengths_preserved(j, 4, orig);
}

static void test_fabrik_clamps_unreachable_target(void)
{
    float j[12]; make_chain4(j);
    float orig[12]; memcpy(orig, j, sizeof orig);

    float tgt[3] = {0.0f, 0.0f, 12.0f};  /* dist 12 > reach 3 */
    int iters = jce_anim_ik_fabrik_solve(j, 4, tgt, 64, 1e-4f);
    TEST_ASSERT_EQUAL_INT(1, iters);     /* unreachable path: single layout */

    /* End at max reach 3 from root, pointing toward target (+Z). */
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 3.0f, v3_distance(&j[0], &j[9]));
    TEST_ASSERT_TRUE(j[11] > 2.9f);
    assert_lengths_preserved(j, 4, orig);
}

static void test_fabrik_root_stays_fixed(void)
{
    float j[12]; make_chain4(j);
    /* Move root away from origin first to prove it is pinned back. */
    j[0]=0.5f; j[1]=0.5f; j[2]=0.0f;
    float root[3] = {0.5f, 0.5f, 0.0f};
    float tgt[3] = {2.0f, 1.0f, 0.0f};
    jce_anim_ik_fabrik_solve(j, 4, tgt, 64, 1e-4f);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, v3_distance(&j[0], root));
}

/* ── Cross-check: CCD and FABRIK agree on the end effector ──────────── */

static void test_ccd_fabrik_agree_on_end_effector(void)
{
    float ja[12]; make_chain4(ja);
    float jb[12]; make_chain4(jb);
    float tgt[3] = {2.0f, 1.0f, -0.5f};

    jce_anim_ik_ccd_solve(ja, 4, tgt, 128, 1e-5f);
    jce_anim_ik_fabrik_solve(jb, 4, tgt, 128, 1e-5f);

    /* Both end effectors converge onto the (reachable) target.  FABRIK
     * converges tightly; CCD plateaus a little looser (linear convergence). */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.0f, v3_distance(&ja[9], tgt));
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 0.0f, v3_distance(&jb[9], tgt));
    /* And they agree on where the effector ended up. */
    TEST_ASSERT_FLOAT_WITHIN(1e-2f, 0.0f, v3_distance(&ja[9], &jb[9]));
}

int main(void)
{
    UNITY_BEGIN();
    /* Two-bone degenerate guards */
    RUN_TEST(test_two_bone_zero_length_no_nan);
    RUN_TEST(test_two_bone_target_on_root_no_nan);
    /* Aim */
    RUN_TEST(test_aim_null_inputs_are_safe);
    RUN_TEST(test_aim_points_forward_at_target);
    RUN_TEST(test_aim_offset_pivot_aims_at_target);
    RUN_TEST(test_aim_weight_zero_keeps_forward);
    RUN_TEST(test_aim_target_on_pivot_is_safe);
    /* CCD */
    RUN_TEST(test_ccd_null_inputs_are_safe);
    RUN_TEST(test_ccd_reaches_reachable_target);
    RUN_TEST(test_ccd_clamps_unreachable_target);
    RUN_TEST(test_ccd_already_at_target_zero_iters);
    /* FABRIK */
    RUN_TEST(test_fabrik_null_inputs_are_safe);
    RUN_TEST(test_fabrik_reaches_reachable_target);
    RUN_TEST(test_fabrik_clamps_unreachable_target);
    RUN_TEST(test_fabrik_root_stays_fixed);
    /* Cross-check */
    RUN_TEST(test_ccd_fabrik_agree_on_end_effector);
    return UNITY_END();
}
