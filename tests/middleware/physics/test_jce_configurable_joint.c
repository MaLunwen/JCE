/*
 * test_jce_configurable_joint.c — Headless self-test for the per-axis 6DOF
 * configurable-joint physics primitive that the runtime's ConfigurableJoint
 * last-mile is built on.
 *
 * The runtime glue (rt_spawn_configurable_joint + rt_monitor_configurable_joints
 * in jce_runtime.c) is correct-by-construction: in the post-spawn joint pass it
 * resolves body_a (the jointed entity) + body_b (connected_body, or the world)
 * and calls jce_physics_configurable_joint_create from the authored
 * JceConfigurableJointComponent; each fixed tick AFTER jce_physics_step it
 * compares jce_physics_constraint_applied_impulse(...) against
 * (break_force * fixed_dt) — impulse = force * dt — and destroys the joint on
 * overrun.  This test exercises those PUBLIC physics primitives directly against
 * a real Bullet world (mirrors test_jce_constant_force / test_jce_vehicle),
 * proving the behaviour the runtime relies on:
 *
 * Coverage:
 *   1. per-axis-lock-vs-free — two dynamic bodies joined with linear X LOCKED,
 *      linear Y FREE (zero gravity, equal-and-opposite separating forces on X
 *      and on Y).  The X separation stays ~constant (locked) while the Y
 *      separation GROWS (free) — the defining capability of the per-axis joint.
 *   2. limited-axis          — linear X LIMITED to ±L; a sustained separating
 *      force is applied on +X.  The X separation never meaningfully exceeds L,
 *      AND REACHES it: a locked axis also never exceeds L, so the cap alone
 *      cannot tell LIMITED from LOCKED.  (It could not, for as long as
 *      cfg_apply_axis discarded the limit it was handed.)
 *   3. break-applied-impulse — a joint resisting a large sustained separating
 *      force accumulates a NON-ZERO, growing applied impulse, and that impulse
 *      exceeds a small break threshold expressed as break_force * dt (the exact
 *      quantity the runtime monitor compares — impulse vs force*dt).  This is
 *      the physics-level certainty behind the runtime's break-destroy glue.
 *   4. determinism / finite  — positions/impulses stay finite, and two identical
 *      runs produce identical separations + impulses.
 *
 * UNIT CHOICE (documented, mirrors the runtime): jce_physics_constraint_applied_
 * impulse returns an IMPULSE (N·s); break_force is a FORCE (N).  The threshold is
 * therefore break_force * dt so both sides are impulse units.
 *
 * Tolerances are generous: Bullet is deterministic for identical input on one
 * build but not bit-exact in absolute magnitudes across machines.
 *
 * Linked against jce_core + jce_physics (mirrors test_jce_constant_force).
 */

#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── shared constants ──────────────────────────────────────────────────────── */

static const float DT       = 1.0f / 60.0f;
static const float BOX_HALF = 0.5f;
static const float BOX_MASS = 1.0f;

/* CFG joint motion enum mirror (JCE_CFG_JOINT_* in jce_scene.h; the physics desc
 * takes the same 0=locked 1=limited 2=free values). */
enum { LOCKED = 0, LIMITED = 1, FREE = 2 };

/* ── helpers ───────────────────────────────────────────────────────────────── */

/* Zero-gravity world so the only motion is what we apply / what the joint
 * constrains (isolates the joint behaviour). */
static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity        = jce_v3(0.0f, 0.0f, 0.0f);
    wd.max_bodies     = 64u;
    wd.fixed_timestep = DT;
    wd.max_sub_steps  = 4;
    return jce_physics_create(&wd);
}

static JceBodyHandle make_box(JcePhysicsWorld *w, jce_vec3 pos)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type            = JCE_BODY_DYNAMIC;
    bd.shape           = JCE_SHAPE_BOX;
    bd.position        = pos;
    bd.rotation        = jce_q_identity();
    bd.half_extents    = jce_v3(BOX_HALF, BOX_HALF, BOX_HALF);
    bd.mass            = BOX_MASS;
    bd.friction        = 0.5f;
    bd.linear_damping  = 0.0f;   /* no damping so the joint behaviour is clean */
    bd.angular_damping = 0.0f;
    bd.collision_group = JCE_COLLISION_DEFAULT_GROUP;
    bd.collision_mask  = JCE_COLLISION_ALL_MASK;
    return jce_physics_body_create(w, &bd);
}

static int finite3(jce_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

static float body_x(JcePhysicsWorld *w, JceBodyHandle b)
{
    jce_vec3 p; jce_quat r;
    jce_physics_body_get_transform(w, b, &p, &r);
    return p.x;
}

static float body_y(JcePhysicsWorld *w, JceBodyHandle b)
{
    jce_vec3 p; jce_quat r;
    jce_physics_body_get_transform(w, b, &p, &r);
    return p.y;
}

/* Build a configurable joint between A (at -X) and B (at +X) with the given
 * per-axis motion and linear limit.  Anchor at each body centre so the joint
 * pins the two centres (separation == centre distance). */
static JceConstraintHandle make_joint(JcePhysicsWorld *w,
                                      JceBodyHandle a, JceBodyHandle b,
                                      int xm, int ym, int zm, float lin_limit)
{
    JceConfigurableJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.body_a            = a;
    jd.body_b            = b;
    jd.anchor_a          = jce_v3(0.0f, 0.0f, 0.0f);
    jd.anchor_b          = jce_v3(0.0f, 0.0f, 0.0f);
    jd.lin_motion[0]     = xm;
    jd.lin_motion[1]     = ym;
    jd.lin_motion[2]     = zm;
    /* Angular axes locked for these linear-behaviour tests. */
    jd.ang_motion[0]     = LOCKED;
    jd.ang_motion[1]     = LOCKED;
    jd.ang_motion[2]     = LOCKED;
    jd.linear_limit      = lin_limit;
    jd.angular_limit_deg[0] = 0.0f;
    jd.angular_limit_deg[1] = 0.0f;
    jd.angular_limit_deg[2] = 0.0f;
    jd.disable_collision = true;   /* the two bodies overlap-adjacent; ignore contacts */
    return jce_physics_configurable_joint_create(w, &jd);
}

/* ── tests ─────────────────────────────────────────────────────────────────── */

/* Linear X LOCKED, Y FREE: equal-and-opposite separating forces on X and on Y.
 * The X separation must hold (locked); the Y separation must grow (free). */
static void test_per_axis_lock_vs_free(void)
{
    enum { FRAMES = 120 };
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* Start the two bodies coincident in Y but offset in X by exactly the locked
     * separation; the joint will hold X at that initial offset. */
    JceBodyHandle a = make_box(w, jce_v3(-0.75f, 0.0f, 0.0f));
    JceBodyHandle b = make_box(w, jce_v3( 0.75f, 0.0f, 0.0f));
    TEST_ASSERT_TRUE(jce_physics_body_is_dynamic(w, a));
    TEST_ASSERT_TRUE(jce_physics_body_is_dynamic(w, b));

    JceConstraintHandle j = make_joint(w, a, b, LOCKED, FREE, LOCKED, 0.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    const float x_sep0 = body_x(w, b) - body_x(w, a);
    const float y_sep0 = body_y(w, b) - body_y(w, a);

    const jce_vec3 push_a = jce_v3(-30.0f, -30.0f, 0.0f); /* pull A toward -X,-Y */
    const jce_vec3 push_b = jce_v3( 30.0f,  30.0f, 0.0f); /* pull B toward +X,+Y */

    for (int i = 0; i < FRAMES; ++i) {
        jce_physics_body_apply_force(w, a, push_a);
        jce_physics_body_apply_force(w, b, push_b);
        jce_physics_step(w, DT);
    }

    const float x_sep = body_x(w, b) - body_x(w, a);
    const float y_sep = body_y(w, b) - body_y(w, a);

    TEST_ASSERT_TRUE(finite3(jce_physics_body_get_velocity(w, a)));
    TEST_ASSERT_TRUE(finite3(jce_physics_body_get_velocity(w, b)));

    /* Locked X: separation barely changes despite the sustained pull. */
    TEST_ASSERT_FLOAT_WITHIN(0.1f, x_sep0, x_sep);
    /* Free Y: separation grows well past where it started (unambiguous). */
    TEST_ASSERT_TRUE(y_sep > y_sep0 + 1.0f);

    jce_physics_constraint_destroy(w, j);
    jce_physics_destroy(w);
}

/* Linear X LIMITED to ±L: a sustained +X separating force must not pull the X
 * separation meaningfully past L. */
static void test_limited_axis(void)
{
    enum { FRAMES = 240 };
    const float L = 0.5f;
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* Two coincident pairs under the SAME separating pull: one X-FREE (flies
     * apart), one X-LIMITED to +-L (the limit must CAP it).  Comparing the two
     * makes the assertion non-vacuous — the limit is proven to constrain
     * because the free reference clearly travels past L under the identical
     * force.  The within-range free-slide this used to defer to a followup is
     * now asserted below, because it is the ONLY property that distinguishes
     * LIMITED from LOCKED. */
    JceBodyHandle fa = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceBodyHandle fb = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
    JceConstraintHandle fj = make_joint(w, fa, fb, FREE, LOCKED, LOCKED, 0.0f);

    JceBodyHandle la = make_box(w, jce_v3(0.0f, 6.0f, 0.0f));
    JceBodyHandle lb = make_box(w, jce_v3(0.0f, 6.0f, 0.0f));
    JceConstraintHandle lj = make_joint(w, la, lb, LIMITED, LOCKED, LOCKED, L);
    TEST_ASSERT_TRUE(jce_constraint_valid(fj));
    TEST_ASSERT_TRUE(jce_constraint_valid(lj));

    const jce_vec3 push_neg = jce_v3(-40.0f, 0.0f, 0.0f);
    const jce_vec3 push_pos = jce_v3( 40.0f, 0.0f, 0.0f);
    for (int i = 0; i < FRAMES; ++i) {
        jce_physics_body_apply_force(w, fa, push_neg);
        jce_physics_body_apply_force(w, fb, push_pos);
        jce_physics_body_apply_force(w, la, push_neg);
        jce_physics_body_apply_force(w, lb, push_pos);
        jce_physics_step(w, DT);
    }

    const float free_sep = fabsf(body_x(w, fb) - body_x(w, fa));
    const float lim_sep  = fabsf(body_x(w, lb) - body_x(w, la));
    TEST_ASSERT_TRUE(isfinite(free_sep));
    TEST_ASSERT_TRUE(isfinite(lim_sep));
    /* The free reference flies well past the limit under this pull... */
    TEST_ASSERT_TRUE(free_sep > L + 0.5f);
    /* ...while the LIMITED axis is CAPPED within ~L (small solver slack). */
    TEST_ASSERT_TRUE(lim_sep <= L + 0.2f);
    /* ...and is clearly more constrained than the free reference. */
    TEST_ASSERT_TRUE(lim_sep < free_sep - 0.3f);
    /* ...AND IT ACTUALLY GETS THERE.  Everything above this line is equally
     * true of a LOCKED axis, whose separation is ~0: never exceeds L, and
     * certainly more constrained than the free reference.  That is not a
     * hypothetical -- cfg_apply_axis ignored its `limit` parameter and sent
     * LIMITED through the LOCKED branch, and this case stayed green the whole
     * time.  The distinguishing property is that a limited axis MOVES, up to
     * its bound; a locked one does not move at all. */
    TEST_ASSERT_TRUE_MESSAGE(lim_sep > L * 0.5f,
        "the LIMITED axis barely moved -- it is behaving as LOCKED, which "
        "every other assertion in this case is equally happy with");

    jce_physics_constraint_destroy(w, fj);
    jce_physics_constraint_destroy(w, lj);
    jce_physics_destroy(w);
}

/* A joint resisting a large sustained separating force accumulates a non-zero,
 * growing applied impulse that exceeds a small break threshold (break_force*dt)
 * — the exact quantity the runtime break monitor compares. */
static void test_break_applied_impulse(void)
{
    enum { WARMUP = 5, FRAMES = 60 };
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle a = make_box(w, jce_v3(-0.75f, 0.0f, 0.0f));
    JceBodyHandle b = make_box(w, jce_v3( 0.75f, 0.0f, 0.0f));

    /* All linear axes LOCKED so the joint must resist the full pull. */
    JceConstraintHandle j = make_joint(w, a, b, LOCKED, LOCKED, LOCKED, 0.0f);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    /* A large separating force on each body. */
    const float    F = 200.0f;
    const jce_vec3 push_a = jce_v3(-F, 0.0f, 0.0f);
    const jce_vec3 push_b = jce_v3( F, 0.0f, 0.0f);

    /* Warm up a few steps so the solver settles a steady reaction impulse. */
    for (int i = 0; i < WARMUP; ++i) {
        jce_physics_body_apply_force(w, a, push_a);
        jce_physics_body_apply_force(w, b, push_b);
        jce_physics_step(w, DT);
    }

    float imp_early = jce_physics_constraint_applied_impulse(w, j);
    TEST_ASSERT_TRUE(isfinite(imp_early));
    /* The joint is actively resisting -> a non-zero reaction impulse. */
    TEST_ASSERT_TRUE(imp_early > 0.0f);

    for (int i = 0; i < FRAMES; ++i) {
        jce_physics_body_apply_force(w, a, push_a);
        jce_physics_body_apply_force(w, b, push_b);
        jce_physics_step(w, DT);
    }

    float imp_late = jce_physics_constraint_applied_impulse(w, j);
    TEST_ASSERT_TRUE(isfinite(imp_late));
    TEST_ASSERT_TRUE(imp_late > 0.0f);

    /* The runtime monitor breaks when applied_impulse > break_force * dt.
     * With a tiny break_force the threshold is small and the steady reaction
     * impulse to a 200 N pull comfortably exceeds it — i.e. a joint authored
     * with this break_force WOULD snap.  (UNIT CHECK: impulse vs force*dt.) */
    const float break_force = 1.0f;                 /* N */
    const float threshold   = break_force * DT;     /* N·s */
    TEST_ASSERT_TRUE(imp_late > threshold);

    /* Invalid handle returns 0 (no crash). */
    JceConstraintHandle bogus = JCE_CONSTRAINT_INVALID;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_physics_constraint_applied_impulse(w, bogus));

    jce_physics_constraint_destroy(w, j);
    /* After destroy the slot is dead -> query returns 0. */
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_physics_constraint_applied_impulse(w, j));

    jce_physics_destroy(w);
}

/* Two identical runs of the limited-axis scenario produce identical separations
 * and applied impulses; all quantities stay finite. */
static void test_determinism(void)
{
    enum { FRAMES = 90 };
    const float L = 0.4f;

    float x_sep[2];
    float imp[2];

    for (int run = 0; run < 2; ++run) {
        JcePhysicsWorld *w = make_world();
        TEST_ASSERT_NOT_NULL(w);

        JceBodyHandle a = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
        JceBodyHandle b = make_box(w, jce_v3(0.0f, 0.0f, 0.0f));
        JceConstraintHandle j = make_joint(w, a, b, LIMITED, LOCKED, LOCKED, L);
        TEST_ASSERT_TRUE(jce_constraint_valid(j));

        const jce_vec3 push_a = jce_v3(-18.0f, 0.0f, 0.0f);
        const jce_vec3 push_b = jce_v3( 18.0f, 0.0f, 0.0f);

        for (int i = 0; i < FRAMES; ++i) {
            jce_physics_body_apply_force(w, a, push_a);
            jce_physics_body_apply_force(w, b, push_b);
            jce_physics_step(w, DT);
        }

        x_sep[run] = body_x(w, b) - body_x(w, a);
        imp[run]   = jce_physics_constraint_applied_impulse(w, j);

        TEST_ASSERT_TRUE(isfinite(x_sep[run]));
        TEST_ASSERT_TRUE(isfinite(imp[run]));
        TEST_ASSERT_TRUE(finite3(jce_physics_body_get_velocity(w, a)));
        TEST_ASSERT_TRUE(finite3(jce_physics_body_get_velocity(w, b)));

        jce_physics_constraint_destroy(w, j);
        jce_physics_destroy(w);
    }

    /* Bit-for-bit identical input on one build -> identical output. */
    TEST_ASSERT_EQUAL_FLOAT(x_sep[0], x_sep[1]);
    TEST_ASSERT_EQUAL_FLOAT(imp[0],   imp[1]);
}


/* ── angular LIMITED ──────────────────────────────────────────────────────
 *
 * THE FILE HAD NO ANGULAR LIMIT CASE AT ALL.  Every LIMITED axis above is
 * linear; ang_motion appeared only as LOCKED and angular_limit_deg only as
 * 0.0.  So the header's promise of "INDEPENDENT motion authoring per linear
 * and angular axis (Locked / Limited / Free)" was covered for two of the
 * three states on one of the two kinds of axis.
 *
 * Same shape as the linear case above, for the same reason: TWO joints, one
 * with the axis FREE and one LIMITED, under an identical sustained torque.
 * A single joint cannot tell "the limit held it" from "nothing pushed it",
 * and the FREE twin is what makes the comparison mean something.
 *
 * The angle measured is RELATIVE between the two bodies, which is what a
 * joint limit constrains -- a pair that tumbles together as one rigid unit
 * has each body far from its start orientation and the JOINT unmoved.
 */
static float rel_angle_deg(JcePhysicsWorld *w, JceBodyHandle a, JceBodyHandle b)
{
    jce_vec3 pa, pb;
    jce_quat qa, qb;
    jce_physics_body_get_transform(w, a, &pa, &qa);
    jce_physics_body_get_transform(w, b, &pb, &qb);
    (void)pa; (void)pb;
    const jce_quat inv_a = { -qa.x, -qa.y, -qa.z, qa.w };   /* unit => inverse */
    const jce_quat rel   = jce_q_multiply(inv_a, qb);
    float qw = fabsf(rel.w);
    if (qw > 1.0f) qw = 1.0f;
    return 2.0f * acosf(qw) * (180.0f / 3.14159265358979323846f);
}

static void test_angular_limited_axis_caps_the_twist(void)
{
    const float LIM_DEG = 20.0f;

    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* FREE twin. */
    JceBodyHandle fa = make_box(w, jce_v3(-2.0f, 0.0f, 0.0f));
    JceBodyHandle fb = make_box(w, jce_v3(-2.0f, 0.0f, 0.0f));
    /* LIMITED twin. */
    JceBodyHandle la = make_box(w, jce_v3(2.0f, 0.0f, 0.0f));
    JceBodyHandle lb = make_box(w, jce_v3(2.0f, 0.0f, 0.0f));

    JceConfigurableJointDesc jd;
    memset(&jd, 0, sizeof jd);
    jd.anchor_a = jce_v3(0.0f, 0.0f, 0.0f);
    jd.anchor_b = jce_v3(0.0f, 0.0f, 0.0f);
    jd.lin_motion[0] = jd.lin_motion[1] = jd.lin_motion[2] = LOCKED;
    jd.disable_collision = true;

    jd.body_a = fa; jd.body_b = fb;
    jd.ang_motion[0] = FREE; jd.ang_motion[1] = FREE; jd.ang_motion[2] = FREE;
    JceConstraintHandle fj = jce_physics_configurable_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(fj));

    jd.body_a = la; jd.body_b = lb;
    jd.ang_motion[0] = LIMITED; jd.ang_motion[1] = LIMITED; jd.ang_motion[2] = LIMITED;
    jd.angular_limit_deg[0] = LIM_DEG;
    jd.angular_limit_deg[1] = LIM_DEG;
    jd.angular_limit_deg[2] = LIM_DEG;
    JceConstraintHandle lj = jce_physics_configurable_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(lj));

    float free_worst = 0.0f, lim_worst = 0.0f;
    for (int i = 0; i < 300; ++i) {
        /* All three axes, so the result does not depend on which one Bullet
         * happens to treat as its twist. */
        const jce_vec3 t = jce_v3(3.0f, 3.0f, 3.0f);
        jce_physics_body_apply_torque(w, fb, t);
        jce_physics_body_apply_torque(w, lb, t);
        jce_physics_step(w, DT);
        const float f = rel_angle_deg(w, fa, fb);
        const float l = rel_angle_deg(w, la, lb);
        if (f > free_worst) free_worst = f;
        if (l > lim_worst)  lim_worst  = l;
    }
    printf("  angular: free %.1f deg, limited %.1f deg (limit %.1f)\n",
           (double)free_worst, (double)lim_worst, (double)LIM_DEG);
    jce_physics_destroy(w);

    /* THE CONTROL: the FREE pair must actually twist, or nothing below means
     * anything. */
    TEST_ASSERT_TRUE_MESSAGE(free_worst > LIM_DEG * 2.0f,
        "the FREE angular pair barely twisted under a sustained torque, so "
        "this case cannot tell a working limit from a joint nothing moved");

    /* Solver slack: an iterative solver overshoots a limit under sustained
     * torque, so the bound is generous.  What is being asserted is that a
     * bound EXISTS -- an unenforced limit lands near the free number. */
    TEST_ASSERT_TRUE_MESSAGE(lim_worst < LIM_DEG * 3.0f + 10.0f,
        "a LIMITED angular axis did not cap the relative twist -- the "
        "authored angular_limit_deg is not reaching the solver, so every "
        "consumer of it (ragdoll joints, authored ConfigurableJoints) has an "
        "angular range that does nothing");
    TEST_ASSERT_TRUE_MESSAGE(lim_worst < free_worst * 0.75f,
        "the LIMITED pair twisted about as far as the FREE pair under an "
        "identical torque, which is what an unenforced limit looks like");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_per_axis_lock_vs_free);
    RUN_TEST(test_limited_axis);
    RUN_TEST(test_break_applied_impulse);
    RUN_TEST(test_determinism);
    RUN_TEST(test_angular_limited_axis_caps_the_twist);
    return UNITY_END();
}
