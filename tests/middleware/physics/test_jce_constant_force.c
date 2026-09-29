/*
 * test_jce_constant_force.c — Headless self-test for the constant-force
 * physics primitive that the runtime's ConstantForce last-mile is built on.
 *
 * The runtime glue (rt_apply_constant_force in jce_runtime.c) is correct-by-
 * construction: each fixed tick, for every dynamic body whose entity carries an
 * ENABLED JceConstantForceComponent, it accumulates the authored force/torque
 * via jce_physics_body_apply_force / jce_physics_body_apply_torque BEFORE
 * jce_physics_step.  This test exercises those PUBLIC physics primitives
 * directly against a real Bullet world (mirrors test_jce_vehicle), proving the
 * behaviour the runtime relies on: a continuously-applied force accelerates a
 * dynamic body, and a continuously-applied torque spins it up.
 *
 * Coverage:
 *   1. force-accelerates    — a constant +X force applied each step from rest
 *                             (zero gravity) builds +X velocity and advances
 *                             the body along +X (a = F/m, monotone).
 *   2. torque-spins-up      — a constant torque about +Y applied each step from
 *                             rest builds +Y angular velocity.
 *   3. force-zero-is-noop   — no force applied -> a body in a zero-gravity world
 *                             stays put (proves the test rig isn't drifting and
 *                             that "component at defaults == no motion").
 *   4. static-body-ignored  — applying force to a STATIC body does not move it
 *                             (mirrors the runtime's dynamic-only gate).
 *
 * Tolerances are generous: Bullet is deterministic for identical input on one
 * build but not bit-exact in absolute magnitudes across machines.
 *
 * Linked against jce_core + jce_physics (mirrors test_jce_vehicle).
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
static const float BOX_MASS = 2.0f;

/* ── helpers ───────────────────────────────────────────────────────────────── */

/* Zero-gravity world so the only motion is what we apply (isolates the force). */
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

static JceBodyHandle make_box(JcePhysicsWorld *w, JceBodyType type)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type            = type;
    bd.shape           = JCE_SHAPE_BOX;
    bd.position        = jce_v3(0.0f, 0.0f, 0.0f);
    bd.rotation        = jce_q_identity();
    bd.half_extents    = jce_v3(BOX_HALF, BOX_HALF, BOX_HALF);
    bd.mass            = (type == JCE_BODY_DYNAMIC) ? BOX_MASS : 0.0f;
    bd.friction        = 0.5f;
    bd.linear_damping  = 0.0f;   /* no damping so acceleration is clean */
    bd.angular_damping = 0.0f;
    bd.collision_group = JCE_COLLISION_DEFAULT_GROUP;
    bd.collision_mask  = JCE_COLLISION_ALL_MASK;
    return jce_physics_body_create(w, &bd);
}

static int finite3(jce_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* ── tests ─────────────────────────────────────────────────────────────────── */

/* A constant +X force applied every step from rest accelerates the body in +X.
 * This is exactly what rt_apply_constant_force does per fixed tick. */
static void test_force_accelerates(void)
{
    enum { FRAMES = 120 };
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle b = make_box(w, JCE_BODY_DYNAMIC);
    TEST_ASSERT_TRUE(jce_physics_body_is_dynamic(w, b));

    const jce_vec3 force = jce_v3(20.0f, 0.0f, 0.0f);   /* world-space N */

    jce_vec3 prev_pos;
    jce_quat rot;
    jce_physics_body_get_transform(w, b, &prev_pos, &rot);

    for (int i = 0; i < FRAMES; ++i) {
        /* Runtime applies the force BEFORE the step. */
        jce_physics_body_apply_force(w, b, force);
        jce_physics_step(w, DT);
    }

    jce_vec3 vel = jce_physics_body_get_velocity(w, b);
    jce_vec3 pos;
    jce_physics_body_get_transform(w, b, &pos, &rot);

    TEST_ASSERT_TRUE(finite3(vel));
    TEST_ASSERT_TRUE(finite3(pos));

    /* Velocity built up in +X; the off-axes stayed put (zero gravity). */
    TEST_ASSERT_TRUE(vel.x > 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, vel.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, vel.z);

    /* Position advanced in +X past where it started. */
    TEST_ASSERT_TRUE(pos.x > prev_pos.x + 0.5f);

    /* Roughly a = F/m = 20/2 = 10 m/s^2 over ~2 s -> ~20 m/s (loose bounds,
     * Bullet sub-steps + clamping make this approximate). */
    TEST_ASSERT_TRUE(vel.x > 5.0f);
    TEST_ASSERT_TRUE(vel.x < 40.0f);

    jce_physics_destroy(w);
}

/* A constant torque about +Y applied every step from rest spins the body up. */
static void test_torque_spins_up(void)
{
    enum { FRAMES = 120 };
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle b = make_box(w, JCE_BODY_DYNAMIC);
    TEST_ASSERT_TRUE(jce_physics_body_is_dynamic(w, b));

    const jce_vec3 torque = jce_v3(0.0f, 5.0f, 0.0f);   /* world-space N·m */

    jce_vec3 ang0 = jce_physics_body_get_angular_velocity(w, b);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.0f, ang0.y);

    for (int i = 0; i < FRAMES; ++i) {
        jce_physics_body_apply_torque(w, b, torque);
        jce_physics_step(w, DT);
    }

    jce_vec3 ang = jce_physics_body_get_angular_velocity(w, b);
    TEST_ASSERT_TRUE(finite3(ang));

    /* Angular velocity built up about +Y. */
    TEST_ASSERT_TRUE(ang.y > 0.5f);

    jce_physics_destroy(w);
}

/* No force applied -> a body in a zero-gravity world does not drift. */
static void test_force_zero_is_noop(void)
{
    enum { FRAMES = 120 };
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle b = make_box(w, JCE_BODY_DYNAMIC);
    TEST_ASSERT_TRUE(jce_physics_body_is_dynamic(w, b));

    jce_vec3 start;
    jce_quat rot;
    jce_physics_body_get_transform(w, b, &start, &rot);

    for (int i = 0; i < FRAMES; ++i)
        jce_physics_step(w, DT);

    jce_vec3 pos;
    jce_physics_body_get_transform(w, b, &pos, &rot);
    jce_vec3 vel = jce_physics_body_get_velocity(w, b);

    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.x, pos.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.y, pos.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.z, pos.z);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, vel.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, vel.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, 0.0f, vel.z);

    jce_physics_destroy(w);
}

/* Applying force to a STATIC body does not move it (the runtime gates on
 * jce_physics_body_is_dynamic, so this never happens in practice — but the
 * primitive must be safe regardless). */
static void test_static_body_ignored(void)
{
    enum { FRAMES = 60 };
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle b = make_box(w, JCE_BODY_STATIC);
    TEST_ASSERT_FALSE(jce_physics_body_is_dynamic(w, b));

    jce_vec3 start;
    jce_quat rot;
    jce_physics_body_get_transform(w, b, &start, &rot);

    const jce_vec3 force = jce_v3(50.0f, 0.0f, 0.0f);
    for (int i = 0; i < FRAMES; ++i) {
        jce_physics_body_apply_force(w, b, force);
        jce_physics_step(w, DT);
    }

    jce_vec3 pos;
    jce_physics_body_get_transform(w, b, &pos, &rot);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.x, pos.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.y, pos.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.z, pos.z);

    jce_physics_destroy(w);
}

/* Sim-LOD physics gating (large-world #3): jce_physics_body_set_active(false)
 * sleeps a body so the solver does not integrate it (gravity is ignored — it
 * freezes in place); set_active(true) wakes it and the fall resumes.  This is
 * exactly the primitive the runtime's far-tier physics gating relies on. */
static void test_set_active_sleeps_and_wakes(void)
{
    enum { FRAMES = 90 };
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity        = jce_v3(0.0f, -9.81f, 0.0f);   /* real gravity this time */
    wd.max_bodies     = 8u;
    wd.fixed_timestep = DT;
    wd.max_sub_steps  = 4;
    JcePhysicsWorld *w = jce_physics_create(&wd);
    TEST_ASSERT_NOT_NULL(w);

    JceBodyHandle b = make_box(w, JCE_BODY_DYNAMIC);
    jce_vec3 start, pos; jce_quat rot;
    jce_physics_body_get_transform(w, b, &start, &rot);

    /* Slept before the first step: gravity must NOT move it. */
    jce_physics_body_set_active(w, b, false);
    for (int i = 0; i < FRAMES; ++i) jce_physics_step(w, DT);
    jce_physics_body_get_transform(w, b, &pos, &rot);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, start.y, pos.y);   /* frozen while slept */

    /* Woken: the body now falls under gravity. */
    jce_physics_body_set_active(w, b, true);
    for (int i = 0; i < FRAMES; ++i) jce_physics_step(w, DT);
    jce_physics_body_get_transform(w, b, &pos, &rot);
    TEST_ASSERT_TRUE(pos.y < start.y - 0.5f);          /* fell after wake */

    jce_physics_destroy(w);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_force_accelerates);
    RUN_TEST(test_torque_spins_up);
    RUN_TEST(test_force_zero_is_noop);
    RUN_TEST(test_static_body_ignored);
    RUN_TEST(test_set_active_sleeps_and_wakes);
    return UNITY_END();
}
