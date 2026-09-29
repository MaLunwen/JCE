/*
 * test_jce_physics2d_joint.c — Headless self-test for the 2D-joint physics
 * primitive the runtime's Joint2D last-mile is built on.
 *
 * The runtime glue (rt_spawn_joint2d in jce_runtime.c) is correct-by-
 * construction: in the post-spawn joint pass it resolves body_a (the jointed
 * entity's 2D body) + body_b (connected_body's 2D body, or the world) and calls
 * jce_physics2d_joint_create from the authored JceJoint2DComponent, then tracks
 * the handle so teardown destroys it before the 2D world dies.  This test
 * exercises that PUBLIC 2D-physics joint API directly against a real Box2D world
 * (mirrors test_jce_configurable_joint), proving the behaviour the runtime
 * relies on:
 *
 * Coverage:
 *   1. distance-holds-length — two dynamic bodies + a rigid DISTANCE joint of
 *      rest length L; separating impulses are applied each step; after ~120
 *      steps the centre separation stays ~L (the rigid distance constraint
 *      holds despite the pull).
 *   2. hinge-limit            — a HINGE (revolute) joint with limits enabled
 *      [lower,upper]; a sustained torque drives the body past the upper limit;
 *      after settling the body's angle does not meaningfully exceed the upper
 *      limit (the limit is doing work) and stays finite.
 *   3. spring-settles         — a SPRING (distance + spring) displaced past its
 *      rest length L then released under no external force; over time the
 *      separation relaxes back toward L (the spring restores rest length).
 *   4. destroy + invalid safety — destroy on a valid handle then again on the
 *      same handle is safe; destroy on INVALID is a no-op; create with an
 *      invalid body_a returns INVALID; create on a NULL world returns INVALID.
 *
 * UNITS (mirror the engine API): anchors are LOCAL to each body; angles are
 * radians; frequency is Hz.  The runtime converts the component's deg/Hz to
 * these before calling create.
 *
 * Tolerances are generous: Box2D is deterministic for identical input on one
 * build but not bit-exact in absolute magnitudes across machines.
 *
 * Linked against jce_core + jce_physics (jce_physics2d.c compiles into the
 * jce_physics layer; mirrors test_jce_configurable_joint).
 */

#include "unity.h"

#include <jce/middleware/physics/jce_physics2d.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── shared constants ──────────────────────────────────────────────────────── */

static const float DT       = 1.0f / 60.0f;
static const float BOX_HALF = 0.5f;
static const float BOX_MASS = 1.0f;

/* ── helpers ───────────────────────────────────────────────────────────────── */

/* Zero-gravity world so the only motion is what we apply / what the joint
 * constrains (isolates the joint behaviour). */
static JcePhysics2D *make_world(void)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x  = 0.0f;
    wd.gravity.y  = 0.0f;
    wd.max_bodies = 64u;
    return jce_physics2d_create(&wd);
}

static JceBodyHandle make_box(JcePhysics2D *w, jce_vec2 pos)
{
    JceBody2DDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type            = JCE_BODY_DYNAMIC;
    bd.shape           = JCE_SHAPE2D_BOX;
    bd.position        = pos;
    bd.angle           = 0.0f;
    bd.half_extents.x  = BOX_HALF;
    bd.half_extents.y  = BOX_HALF;
    bd.mass            = BOX_MASS;
    bd.friction        = 0.5f;
    bd.restitution     = 0.0f;
    bd.linear_damping  = 0.0f;   /* no damping so the joint behaviour is clean */
    bd.angular_damping = 0.0f;
    bd.fixed_rotation  = false;
    return jce_physics2d_body_create(w, &bd);
}

static float body_x(JcePhysics2D *w, JceBodyHandle b)
{
    jce_vec2 p; float a;
    jce_physics2d_body_get_transform(w, b, &p, &a);
    return p.x;
}

static float body_angle(JcePhysics2D *w, JceBodyHandle b)
{
    jce_vec2 p; float a;
    jce_physics2d_body_get_transform(w, b, &p, &a);
    return a;
}

/* Centre separation of two bodies (full XY distance). */
static float separation(JcePhysics2D *w, JceBodyHandle a, JceBodyHandle b)
{
    jce_vec2 pa, pb; float aa, ab;
    jce_physics2d_body_get_transform(w, a, &pa, &aa);
    jce_physics2d_body_get_transform(w, b, &pb, &ab);
    float dx = pb.x - pa.x;
    float dy = pb.y - pa.y;
    return sqrtf(dx * dx + dy * dy);
}

/* ── tests ─────────────────────────────────────────────────────────────────── */

/* A rigid DISTANCE joint of rest length L between two dynamic bodies holds the
 * centre separation ~L despite a sustained separating impulse each step. */
static void test_distance_holds_length(void)
{
    enum { FRAMES = 120 };
    const float L = 2.0f;

    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* Place the bodies exactly L apart along X so the joint starts satisfied. */
    JceBodyHandle a = make_box(w, (jce_vec2){ -L * 0.5f, 0.0f });
    JceBodyHandle b = make_box(w, (jce_vec2){  L * 0.5f, 0.0f });
    TEST_ASSERT_TRUE(jce_body_valid(a));
    TEST_ASSERT_TRUE(jce_body_valid(b));

    JcePhysics2DJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.kind     = JCE_PHYSICS2D_JOINT_DISTANCE;
    jd.body_a   = a;
    jd.body_b   = b;
    jd.anchor_a = (jce_vec2){ 0.0f, 0.0f };   /* body centres */
    jd.anchor_b = (jce_vec2){ 0.0f, 0.0f };
    jd.distance = L;

    JceConstraintHandle j = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    /* Pull the bodies apart along X every step. */
    for (int i = 0; i < FRAMES; ++i) {
        jce_physics2d_body_apply_impulse(w, a, (jce_vec2){ -2.0f, 0.0f });
        jce_physics2d_body_apply_impulse(w, b, (jce_vec2){  2.0f, 0.0f });
        jce_physics2d_step(w, DT);
    }

    float sep = separation(w, a, b);
    TEST_ASSERT_TRUE(isfinite(sep));
    /* The rigid distance joint keeps the separation ~L despite the pull. */
    TEST_ASSERT_FLOAT_WITHIN(0.25f, L, sep);

    jce_physics2d_joint_destroy(w, j);
    jce_physics2d_destroy(w);
}

/* A HINGE (revolute) joint with limits [lower,upper] caps the body's rotation:
 * a sustained torque past the upper limit leaves the body's angle ~<= upper. */
static void test_hinge_limit(void)
{
    enum { FRAMES = 240 };
    const float LOWER = -0.3f;   /* rad */
    const float UPPER =  0.6f;   /* rad */

    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* body_a is the rotating body; body_b is the world (a static pivot the
     * joint owns).  Pin at the body origin so it can only rotate about it. */
    JceBodyHandle a = make_box(w, (jce_vec2){ 0.0f, 0.0f });
    TEST_ASSERT_TRUE(jce_body_valid(a));

    JcePhysics2DJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.kind            = JCE_PHYSICS2D_JOINT_HINGE;
    jd.body_a          = a;
    jd.body_b          = JCE_BODY_INVALID;     /* world anchor (static ground) */
    jd.anchor_a        = (jce_vec2){ 0.0f, 0.0f };
    jd.anchor_b        = (jce_vec2){ 0.0f, 0.0f };  /* world pivot at origin */
    jd.use_limits      = true;
    jd.lower_angle_rad = LOWER;
    jd.upper_angle_rad = UPPER;

    JceConstraintHandle j = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    /* Drive rotation toward / past the upper limit with a sustained torque. */
    for (int i = 0; i < FRAMES; ++i) {
        jce_physics2d_body_apply_torque(w, a, 50.0f);
        jce_physics2d_step(w, DT);
    }

    float ang = body_angle(w, a);
    TEST_ASSERT_TRUE(isfinite(ang));
    /* The limit caps the angle: it does not meaningfully exceed UPPER (small
     * solver slack) and it actually travelled out toward the limit. */
    TEST_ASSERT_TRUE(ang <= UPPER + 0.1f);
    TEST_ASSERT_TRUE(ang > 0.5f * UPPER);

    jce_physics2d_joint_destroy(w, j);
    jce_physics2d_destroy(w);
}

/* A SPRING (distance + spring) displaced past its rest length L then released
 * under no external force relaxes back toward L over time. */
static void test_spring_settles(void)
{
    enum { FRAMES = 600 };
    const float L = 2.0f;

    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    /* Start the bodies STRETCHED to 1.5*L so the spring must pull them back. */
    const float start = 1.5f * L;
    JceBodyHandle a = make_box(w, (jce_vec2){ -start * 0.5f, 0.0f });
    JceBodyHandle b = make_box(w, (jce_vec2){  start * 0.5f, 0.0f });
    TEST_ASSERT_TRUE(jce_body_valid(a));
    TEST_ASSERT_TRUE(jce_body_valid(b));

    JcePhysics2DJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.kind          = JCE_PHYSICS2D_JOINT_SPRING;
    jd.body_a        = a;
    jd.body_b        = b;
    jd.anchor_a      = (jce_vec2){ 0.0f, 0.0f };
    jd.anchor_b      = (jce_vec2){ 0.0f, 0.0f };
    jd.distance      = L;
    jd.frequency_hz  = 2.0f;     /* soft-ish spring */
    jd.damping_ratio = 0.5f;     /* damped so it settles (does not ring forever) */

    JceConstraintHandle j = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    const float sep0 = separation(w, a, b);
    /* Sanity: it really started stretched well past the rest length. */
    TEST_ASSERT_TRUE(sep0 > L + 0.5f);

    for (int i = 0; i < FRAMES; ++i)
        jce_physics2d_step(w, DT);

    const float sep1 = separation(w, a, b);
    TEST_ASSERT_TRUE(isfinite(sep1));
    /* The spring restored the rest length: the final separation is much closer
     * to L than the stretched start, and within a modest band of L. */
    TEST_ASSERT_TRUE(fabsf(sep1 - L) < fabsf(sep0 - L));
    TEST_ASSERT_FLOAT_WITHIN(0.5f, L, sep1);

    jce_physics2d_joint_destroy(w, j);
    jce_physics2d_destroy(w);
}

/* Destroy / invalid-handle safety: destroy on a valid then on the same (now
 * dead) handle is safe; destroy on INVALID is a no-op; create with an invalid
 * body_a or a NULL world returns INVALID. */
static void test_destroy_and_invalid_safety(void)
{
    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle a = make_box(w, (jce_vec2){ -1.0f, 0.0f });
    JceBodyHandle b = make_box(w, (jce_vec2){  1.0f, 0.0f });

    JcePhysics2DJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.kind     = JCE_PHYSICS2D_JOINT_DISTANCE;
    jd.body_a   = a;
    jd.body_b   = b;
    jd.distance = 2.0f;

    JceConstraintHandle j = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(j));

    /* Destroy the valid joint, then destroy the SAME handle again (idempotent),
     * then destroy INVALID — none may crash. */
    jce_physics2d_joint_destroy(w, j);
    jce_physics2d_joint_destroy(w, j);
    jce_physics2d_joint_destroy(w, JCE_CONSTRAINT_INVALID);
    jce_physics2d_joint_destroy(NULL, j);   /* NULL world is a no-op */

    /* Create with an invalid body_a -> INVALID. */
    JcePhysics2DJointDesc bad = jd;
    bad.body_a = JCE_BODY_INVALID;
    bad.body_b = b;
    TEST_ASSERT_FALSE(jce_constraint_valid(jce_physics2d_joint_create(w, &bad)));

    /* Create on a NULL world / NULL desc -> INVALID. */
    TEST_ASSERT_FALSE(jce_constraint_valid(jce_physics2d_joint_create(NULL, &jd)));
    TEST_ASSERT_FALSE(jce_constraint_valid(jce_physics2d_joint_create(w, NULL)));

    /* The world is still usable after all that (a fresh joint still creates). */
    JceConstraintHandle j2 = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(j2));
    jce_physics2d_joint_destroy(w, j2);

    jce_physics2d_destroy(w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_distance_holds_length);
    RUN_TEST(test_hinge_limit);
    RUN_TEST(test_spring_settles);
    RUN_TEST(test_destroy_and_invalid_safety);
    return UNITY_END();
}
