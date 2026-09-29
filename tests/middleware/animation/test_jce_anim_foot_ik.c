/* test_jce_anim_foot_ik.c
 *
 * Pure-math unit tests for the Foot IK solver (jce_anim_foot_ik.h).  The
 * solver is state-free; we craft minimal biped legs (two unit bones along Y,
 * ankle at the foot) plus sampled ground heights/normals and verify the
 * documented conventions:
 *
 *   - flat ground at the animated foot height -> no pelvis drop, feet unchanged
 *   - one foot on a step ABOVE the animated foot -> that foot is raised to the
 *     ground; raising ONE foot does NOT drop the hips (over==0 convention)
 *   - one foot BELOW (stepping down) -> hips drop (pelvis_offset_y negative),
 *     magnitude == the height gap, clamped to max_step_height
 *   - blend pass-through (0) and halfway (0.5)
 *   - ungrounded leg -> solved=false, untouched
 *   - determinism + reach invariants
 *
 * Headless; depends only on the public animation header + Unity.
 */

#include <jce/middleware/animation/jce_anim_foot_ik.h>

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

/* Build a canonical biped:
 *   left  leg at x=-0.2, right leg at x=+0.2
 *   each leg: hip at y=2 (above), knee at y=1, ankle at y=0 (foot on ground)
 *   pelvis at (0, 2, 0)
 * Two unit bones along -Y, plenty of bend room (hip->knee->ankle straight). */
static JceFootIkInput make_biped(void)
{
    JceFootIkInput in;
    memset(&in, 0, sizeof(in));
    in.leg_count       = 2;
    in.max_step_height = 0.5f;
    in.foot_offset     = 0.0f;
    in.blend           = 1.0f;
    in.pelvis[0] = 0.0f; in.pelvis[1] = 2.0f; in.pelvis[2] = 0.0f;

    for (int i = 0; i < 2; i++) {
        float x = (i == 0) ? -0.2f : 0.2f;
        JceFootIkLeg *lg = &in.legs[i];
        lg->hip[0]   = x; lg->hip[1]   = 2.0f; lg->hip[2]   = 0.0f;
        lg->knee[0]  = x; lg->knee[1]  = 1.0f; lg->knee[2]  = 0.0f;
        lg->ankle[0] = x; lg->ankle[1] = 0.0f; lg->ankle[2] = 0.0f;
        lg->ground_y = 0.0f;            /* flat at the foot */
        lg->ground_normal[0] = 0.0f;
        lg->ground_normal[1] = 1.0f;
        lg->ground_normal[2] = 0.0f;
        lg->grounded = true;
    }
    return in;
}

/* ── Flat ground: no pelvis drop, feet unchanged ─────────────────────── */
static void test_flat_ground_is_identity(void)
{
    JceFootIkInput in = make_biped();
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.pelvis_offset_y);
    for (int i = 0; i < 2; i++) {
        TEST_ASSERT_TRUE(out.legs[i].solved);
        /* ankle stays at ground (y=0); the foot did not move materially. */
        TEST_ASSERT_FLOAT_WITHIN(5e-3f, 0.0f, out.legs[i].ankle[1]);
        TEST_ASSERT_FLOAT_WITHIN(5e-3f, in.legs[i].ankle[0], out.legs[i].ankle[0]);
        TEST_ASSERT_FLOAT_WITHIN(5e-3f, in.legs[i].ankle[2], out.legs[i].ankle[2]);
    }
}

/* ── One foot on a step ABOVE the animated foot ──────────────────────── */
static void test_step_up_raises_foot_no_hip_drop(void)
{
    JceFootIkInput in = make_biped();
    /* Right foot's ground is 0.3 ABOVE the animated foot (a step up). */
    in.legs[1].ground_y = 0.3f;
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    /* Raising one foot must NOT drop the hips (over==0 for both legs). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.pelvis_offset_y);

    /* Right ankle is raised to the ground contact (ground_y + foot_offset). */
    TEST_ASSERT_TRUE(out.legs[1].solved);
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 0.3f, out.legs[1].ankle[1]);
    /* Left foot untouched (still on its flat ground). */
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 0.0f, out.legs[0].ankle[1]);
}

/* ── One foot BELOW (stepping down): hips drop by the gap ─────────────── */
static void test_step_down_drops_hips_by_gap(void)
{
    JceFootIkInput in = make_biped();
    /* Right foot's ground is 0.2 BELOW the animated foot (stepping down):
     * the foot dangles 0.2 above its contact -> hips drop 0.2 so the LEFT
     * (higher) planted foot still reaches. */
    in.legs[1].ground_y = -0.2f;
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    TEST_ASSERT_FLOAT_WITHIN(EPS, -0.2f, out.pelvis_offset_y);
}

/* ── max_step clamp: huge gap clamps the pelvis drop magnitude ───────── */
static void test_step_down_clamped_to_max_step(void)
{
    JceFootIkInput in = make_biped();
    in.max_step_height = 0.5f;
    /* 3 m drop -> clamp magnitude to max_step_height (0.5). */
    in.legs[1].ground_y = -3.0f;
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    TEST_ASSERT_FLOAT_WITHIN(EPS, -0.5f, out.pelvis_offset_y);
}

/* ── blend == 0 -> pass-through (no pelvis shift, legs unsolved) ─────── */
static void test_blend_zero_is_passthrough(void)
{
    JceFootIkInput in = make_biped();
    in.legs[1].ground_y = -0.2f;   /* would otherwise drop the hips */
    in.blend = 0.0f;
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.pelvis_offset_y);
    TEST_ASSERT_FALSE(out.legs[0].solved);
    TEST_ASSERT_FALSE(out.legs[1].solved);
}

/* ── blend == 0.5 -> halfway pelvis shift ────────────────────────────── */
static void test_blend_half_is_halfway(void)
{
    JceFootIkInput full = make_biped();
    full.legs[1].ground_y = -0.2f;
    JceFootIkOutput o_full;
    jce_anim_foot_ik_solve(&full, &o_full);

    JceFootIkInput half = full;
    half.blend = 0.5f;
    JceFootIkOutput o_half;
    jce_anim_foot_ik_solve(&half, &o_half);

    /* pelvis offset is linearly scaled by blend. */
    TEST_ASSERT_FLOAT_WITHIN(EPS, o_full.pelvis_offset_y * 0.5f,
                             o_half.pelvis_offset_y);
    TEST_ASSERT_TRUE(o_half.legs[1].solved);
    /* The half-blend ankle lies between the animated and full-solved ankle. */
    float a_anim = full.legs[1].ankle[1];
    float a_full = o_full.legs[1].ankle[1];
    float a_half = o_half.legs[1].ankle[1];
    TEST_ASSERT_FLOAT_WITHIN(5e-3f, 0.5f * (a_anim + a_full), a_half);
}

/* ── ungrounded leg -> solved=false, untouched ──────────────────────── */
static void test_ungrounded_leg_passthrough(void)
{
    JceFootIkInput in = make_biped();
    in.legs[0].grounded = false;     /* left foot has no ground hit */
    in.legs[1].ground_y = 0.3f;      /* right foot steps up */
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    TEST_ASSERT_FALSE(out.legs[0].solved);
    TEST_ASSERT_TRUE(out.legs[1].solved);
    /* The ungrounded leg's output stays zeroed (the caller passes the
     * animated pose through for solved==false). */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.legs[0].ankle[0]);
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.legs[0].ankle[1]);
}

/* ── determinism: same input twice -> identical output ──────────────── */
static void test_determinism(void)
{
    JceFootIkInput in = make_biped();
    in.legs[0].ground_y = -0.15f;
    in.legs[1].ground_y =  0.25f;
    JceFootIkOutput a, b;
    jce_anim_foot_ik_solve(&in, &a);
    jce_anim_foot_ik_solve(&in, &b);

    TEST_ASSERT_EQUAL_MEMORY(&a, &b, sizeof(a));
}

/* ── reach invariant: solved ankle stays within hip ± total bone length ─ */
static void test_solved_within_reach(void)
{
    JceFootIkInput in = make_biped();
    in.legs[1].ground_y = 0.4f;   /* big step up */
    JceFootIkOutput out;
    jce_anim_foot_ik_solve(&in, &out);

    TEST_ASSERT_TRUE(out.legs[1].solved);
    /* shifted hip = animated hip + pelvis offset (here ~0). */
    float hip[3] = {
        in.legs[1].hip[0],
        in.legs[1].hip[1] + out.pelvis_offset_y,
        in.legs[1].hip[2]
    };
    float upper = v3_distance(in.legs[1].hip, in.legs[1].knee);
    float lower = v3_distance(in.legs[1].knee, in.legs[1].ankle);
    float reach = upper + lower + 1e-3f;
    float d = v3_distance(hip, out.legs[1].ankle);
    TEST_ASSERT_TRUE(d <= reach);
}

/* ── NULL / degenerate safety ───────────────────────────────────────── */
static void test_null_and_degenerate_safe(void)
{
    JceFootIkOutput out;
    memset(&out, 0xAA, sizeof(out));
    jce_anim_foot_ik_solve(NULL, &out);   /* must zero the output, not crash */
    TEST_ASSERT_FLOAT_WITHIN(EPS, 0.0f, out.pelvis_offset_y);
    TEST_ASSERT_FALSE(out.legs[0].solved);
    TEST_ASSERT_FALSE(out.legs[1].solved);

    /* NULL out is a clean no-op. */
    jce_anim_foot_ik_solve(&(JceFootIkInput){0}, NULL);

    /* Degenerate (zero-length) bones -> leg passes through (solved=false). */
    JceFootIkInput deg = make_biped();
    deg.legs[0].knee[1]  = deg.legs[0].hip[1];   /* hip==knee (zero upper bone) */
    deg.legs[0].ankle[1] = deg.legs[0].hip[1];
    deg.legs[0].ground_y = 0.2f;
    JceFootIkOutput dout;
    jce_anim_foot_ik_solve(&deg, &dout);
    TEST_ASSERT_FALSE(dout.legs[0].solved);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_flat_ground_is_identity);
    RUN_TEST(test_step_up_raises_foot_no_hip_drop);
    RUN_TEST(test_step_down_drops_hips_by_gap);
    RUN_TEST(test_step_down_clamped_to_max_step);
    RUN_TEST(test_blend_zero_is_passthrough);
    RUN_TEST(test_blend_half_is_halfway);
    RUN_TEST(test_ungrounded_leg_passthrough);
    RUN_TEST(test_determinism);
    RUN_TEST(test_solved_within_reach);
    RUN_TEST(test_null_and_degenerate_safe);
    return UNITY_END();
}
