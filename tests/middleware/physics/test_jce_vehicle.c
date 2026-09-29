/*
 * test_jce_vehicle.c — Headless self-test for the btRaycastVehicle wrapper.
 *
 * Exercises the PUBLIC engine vehicle API (jce/middleware/physics/jce_physics.h)
 * against a real Bullet world.  The vehicle is registered as a btActionInterface
 * inside addVehicle(), so jce_physics_step() (-> stepSimulation) auto-updates it
 * each fixed sub-step; this test never touches Bullet internals.
 *
 * Coverage:
 *   1. rest-on-ground   — a 4-wheel vehicle dropped onto a static ground box
 *                         settles (does not fall through, speed ≈ 0).
 *   2. throttle-forward — full throttle from rest builds forward speed and
 *                         advances the chassis along +Z.
 *   3. brake-stops      — braking from cruising speed bleeds the speed off.
 *   4. wheel-transform  — get_wheel_transform returns finite hub positions
 *                         roughly under the chassis for all 4 wheels.
 *   5. invalid-handle   — set_input / get_speed / get_chassis_transform on
 *                         JCE_VEHICLE_INVALID and on a destroyed handle are
 *                         NULL/no-op safe (no crash).
 *   6. determinism      — two identical worlds fed the same input script reach
 *                         the same chassis position within a tight epsilon.
 *
 * Tolerances are generous: Bullet is deterministic for identical input on one
 * build but not bit-exact in absolute magnitudes across machines.
 *
 * Linked against jce_core + jce_physics (mirrors test_jce_cloth / fracture).
 */

#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <stdint.h>
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* ── shared geometry constants ─────────────────────────────────────────────── */

/* Sedan-ish chassis. */
static const float CHASSIS_HX = 0.9f;
static const float CHASSIS_HY = 0.5f;
static const float CHASSIS_HZ = 2.2f;
static const float CHASSIS_MASS = 1500.0f;
static const float MAX_ENGINE = 4000.0f;
static const float MAX_BRAKE  = 100.0f;
static const float MAX_STEER  = 0.5f;   /* radians */

static const float WHEEL_RADIUS  = 0.4f;
static const float SUSPENSION_REST = 0.6f;

static const float DT = 1.0f / 60.0f;

/* ── helpers ────────────────────────────────────────────────────────────────── */

static JcePhysicsWorld *make_world(void)
{
    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity        = jce_v3(0.0f, -9.81f, 0.0f);
    wd.max_bodies     = 256u;
    wd.fixed_timestep = DT;
    wd.max_sub_steps  = 4;
    JcePhysicsWorld *w = jce_physics_create(&wd);
    return w;
}

/* Large static ground box centred at y=0 with its top surface at y=0. */
static void add_ground(JcePhysicsWorld *w)
{
    JceBodyDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type         = JCE_BODY_STATIC;
    bd.shape        = JCE_SHAPE_BOX;
    bd.position     = jce_v3(0.0f, -1.0f, 0.0f);   /* top face at y = 0 */
    bd.rotation     = jce_q_identity();
    bd.half_extents = jce_v3(100.0f, 1.0f, 100.0f);
    bd.mass         = 0.0f;
    bd.friction     = 1.0f;
    (void)jce_physics_body_create(w, &bd);
}

/* Build a 4-wheel vehicle with the chassis centre at (0, start_y, 0). */
static JceVehicleHandle make_vehicle(JcePhysicsWorld *w, float start_y)
{
    JceVehicleDesc vd;
    memset(&vd, 0, sizeof(vd));
    vd.position             = jce_v3(0.0f, start_y, 0.0f);
    vd.rotation             = jce_q_identity();
    vd.chassis_half_extents = jce_v3(CHASSIS_HX, CHASSIS_HY, CHASSIS_HZ);
    vd.chassis_mass         = CHASSIS_MASS;
    vd.max_engine_force     = MAX_ENGINE;
    vd.max_brake_force      = MAX_BRAKE;
    vd.max_steering_rad     = MAX_STEER;
    vd.collision_group      = JCE_COLLISION_DEFAULT_GROUP;
    vd.collision_mask       = JCE_COLLISION_ALL_MASK;

    JceVehicleHandle veh = jce_physics_vehicle_create(w, &vd);
    if (!jce_vehicle_valid(veh)) {
        return veh;
    }

    /* Connection points at the four chassis corners, just inside the box. */
    const float cx = CHASSIS_HX;
    const float cz = CHASSIS_HZ - 0.4f;       /* axles inboard of the bumpers */
    const float cy = -CHASSIS_HY + 0.1f;      /* hubs near the chassis bottom */

    /* FL, FR (front, +Z), RL, RR (rear, -Z). */
    const float corners[4][3] = {
        { -cx, cy,  cz },   /* front-left  */
        {  cx, cy,  cz },   /* front-right */
        { -cx, cy, -cz },   /* rear-left   */
        {  cx, cy, -cz },   /* rear-right  */
    };
    const bool is_front[4] = { true, true, false, false };

    for (int i = 0; i < 4; ++i) {
        JceWheelDesc wd;
        memset(&wd, 0, sizeof(wd));
        wd.connection_point    = jce_v3(corners[i][0], corners[i][1], corners[i][2]);
        wd.wheel_direction     = jce_v3(0.0f, -1.0f, 0.0f);
        wd.wheel_axle          = jce_v3(-1.0f, 0.0f, 0.0f);
        wd.suspension_rest_len = SUSPENSION_REST;
        wd.wheel_radius        = WHEEL_RADIUS;
        wd.is_front_wheel      = is_front[i];
        wd.suspension_stiffness   = 20.0f;
        wd.suspension_damping     = 2.3f;
        wd.suspension_compression = 4.4f;
        wd.friction_slip          = 1000.0f;
        wd.roll_influence         = 0.1f;
        uint32_t idx = jce_physics_vehicle_add_wheel(w, veh, &wd);
        TEST_ASSERT_NOT_EQUAL_UINT32(UINT32_MAX, idx);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i, idx);
    }
    return veh;
}

static void step_n(JcePhysicsWorld *w, int frames)
{
    for (int i = 0; i < frames; ++i) {
        jce_physics_step(w, DT);
    }
}

static int finite3(jce_vec3 v)
{
    return isfinite(v.x) && isfinite(v.y) && isfinite(v.z);
}

/* ── tests ────────────────────────────────────────────────────────────────── */

static void test_rest_on_ground(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    add_ground(w);

    JceVehicleHandle veh = make_vehicle(w, 2.0f);
    TEST_ASSERT_TRUE(jce_vehicle_valid(veh));

    /* Zero input — just let it settle on its suspension. */
    jce_physics_vehicle_set_input(w, veh, 0.0f, 0.0f, 0.0f);
    step_n(w, 120);

    jce_vec3 pos = jce_v3(0, 0, 0);
    jce_quat rot = jce_q_identity();
    jce_physics_vehicle_get_chassis_transform(w, veh, &pos, &rot);
    TEST_ASSERT_TRUE(finite3(pos));

    /* Did NOT fall through the ground (chassis centre stays above y = 0). */
    TEST_ASSERT_TRUE_MESSAGE(pos.y > 0.0f, "vehicle fell through the ground");

    /* At rest, forward speed ≈ 0. */
    float speed = jce_physics_vehicle_get_speed(w, veh);
    TEST_ASSERT_TRUE_MESSAGE(fabsf(speed) < 0.5f, "resting vehicle is moving");

    jce_physics_vehicle_destroy(w, veh);
    jce_physics_destroy(w);
}

static void test_throttle_drives_forward(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    add_ground(w);

    JceVehicleHandle veh = make_vehicle(w, 2.0f);
    TEST_ASSERT_TRUE(jce_vehicle_valid(veh));

    /* Settle first so the wheels are in contact. */
    jce_physics_vehicle_set_input(w, veh, 0.0f, 0.0f, 0.0f);
    step_n(w, 60);

    jce_vec3 p0 = jce_v3(0, 0, 0);
    jce_quat r0 = jce_q_identity();
    jce_physics_vehicle_get_chassis_transform(w, veh, &p0, &r0);

    /* Full throttle, no brake, straight. */
    jce_physics_vehicle_set_input(w, veh, 1.0f, 0.0f, 0.0f);
    step_n(w, 180);

    float speed = jce_physics_vehicle_get_speed(w, veh);
    TEST_ASSERT_TRUE_MESSAGE(speed > 1.5f, "throttle did not build forward speed");

    jce_vec3 p1 = jce_v3(0, 0, 0);
    jce_quat r1 = jce_q_identity();
    jce_physics_vehicle_get_chassis_transform(w, veh, &p1, &r1);
    TEST_ASSERT_TRUE(finite3(p1));

    /* Advanced along +Z (forward). */
    TEST_ASSERT_TRUE_MESSAGE(p1.z > p0.z + 0.5f, "chassis did not advance forward");

    jce_physics_vehicle_destroy(w, veh);
    jce_physics_destroy(w);
}

static void test_brake_stops(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    add_ground(w);

    JceVehicleHandle veh = make_vehicle(w, 2.0f);
    TEST_ASSERT_TRUE(jce_vehicle_valid(veh));

    jce_physics_vehicle_set_input(w, veh, 0.0f, 0.0f, 0.0f);
    step_n(w, 60);

    /* Accelerate to a cruising speed. */
    jce_physics_vehicle_set_input(w, veh, 1.0f, 0.0f, 0.0f);
    step_n(w, 180);
    float cruise = jce_physics_vehicle_get_speed(w, veh);
    TEST_ASSERT_TRUE_MESSAGE(cruise > 1.5f, "could not reach cruising speed");

    /* Release throttle and brake hard. */
    jce_physics_vehicle_set_input(w, veh, 0.0f, 1.0f, 0.0f);
    step_n(w, 180);
    float braked = jce_physics_vehicle_get_speed(w, veh);

    /* Braked speed must be well below cruise (and small in absolute terms). */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(braked) < 0.5f * cruise || fabsf(braked) < 1.0f,
                             "brake did not slow the vehicle");

    jce_physics_vehicle_destroy(w, veh);
    jce_physics_destroy(w);
}

static void test_wheel_transforms_sane(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    add_ground(w);

    JceVehicleHandle veh = make_vehicle(w, 2.0f);
    TEST_ASSERT_TRUE(jce_vehicle_valid(veh));

    /* Let suspension resolve once so the hubs are placed. */
    jce_physics_vehicle_set_input(w, veh, 0.0f, 0.0f, 0.0f);
    step_n(w, 30);

    jce_vec3 chassis = jce_v3(0, 0, 0);
    jce_quat crot = jce_q_identity();
    jce_physics_vehicle_get_chassis_transform(w, veh, &chassis, &crot);

    for (uint32_t i = 0; i < 4u; ++i) {
        jce_vec3 wp = jce_v3(0, 0, 0);
        jce_quat wr = jce_q_identity();
        jce_physics_vehicle_get_wheel_transform(w, veh, i, &wp, &wr);

        TEST_ASSERT_TRUE_MESSAGE(finite3(wp), "wheel hub position not finite");

        /* Hub sits within a few metres of the chassis on every axis. */
        TEST_ASSERT_TRUE(fabsf(wp.x - chassis.x) < 4.0f);
        TEST_ASSERT_TRUE(fabsf(wp.y - chassis.y) < 4.0f);
        TEST_ASSERT_TRUE(fabsf(wp.z - chassis.z) < 4.0f);
    }

    jce_physics_vehicle_destroy(w, veh);
    jce_physics_destroy(w);
}

static void test_invalid_handle_safe(void)
{
    JcePhysicsWorld *w = make_world();
    TEST_ASSERT_NOT_NULL(w);
    add_ground(w);

    JceVehicleHandle inv = JCE_VEHICLE_INVALID;

    /* No-op / NULL-safe on the invalid sentinel. */
    jce_physics_vehicle_set_input(w, inv, 1.0f, 0.0f, 0.0f);
    float s = jce_physics_vehicle_get_speed(w, inv);
    TEST_ASSERT_TRUE(isfinite(s));

    jce_vec3 pos = jce_v3(123.0f, 456.0f, 789.0f);
    jce_quat rot = jce_q_identity();
    jce_physics_vehicle_get_chassis_transform(w, inv, &pos, &rot);
    /* Must not crash; output left finite. */
    TEST_ASSERT_TRUE(finite3(pos));

    /* Same on a destroyed handle. */
    JceVehicleHandle veh = make_vehicle(w, 2.0f);
    TEST_ASSERT_TRUE(jce_vehicle_valid(veh));
    jce_physics_vehicle_destroy(w, veh);

    jce_physics_vehicle_set_input(w, veh, 1.0f, 0.0f, 0.0f);
    s = jce_physics_vehicle_get_speed(w, veh);
    TEST_ASSERT_TRUE(isfinite(s));
    jce_physics_vehicle_get_chassis_transform(w, veh, &pos, &rot);
    TEST_ASSERT_TRUE(finite3(pos));

    /* Double-destroy is safe. */
    jce_physics_vehicle_destroy(w, veh);

    jce_physics_destroy(w);
}

/* Run identical input scripts on two identical worlds and compare results. */
static void test_determinism(void)
{
    enum { FRAMES = 200 };

    JcePhysicsWorld *wa = make_world();
    JcePhysicsWorld *wb = make_world();
    TEST_ASSERT_NOT_NULL(wa);
    TEST_ASSERT_NOT_NULL(wb);
    add_ground(wa);
    add_ground(wb);

    JceVehicleHandle va = make_vehicle(wa, 2.0f);
    JceVehicleHandle vb = make_vehicle(wb, 2.0f);
    TEST_ASSERT_TRUE(jce_vehicle_valid(va));
    TEST_ASSERT_TRUE(jce_vehicle_valid(vb));

    for (int i = 0; i < FRAMES; ++i) {
        /* Deterministic input script: settle, then turn-and-go. */
        float throttle = (i > 30) ? 1.0f : 0.0f;
        float steer    = (i > 90) ? 0.3f : 0.0f;
        jce_physics_vehicle_set_input(wa, va, throttle, 0.0f, steer);
        jce_physics_vehicle_set_input(wb, vb, throttle, 0.0f, steer);
        jce_physics_step(wa, DT);
        jce_physics_step(wb, DT);
    }

    jce_vec3 pa = jce_v3(0, 0, 0), pb = jce_v3(0, 0, 0);
    jce_quat ra = jce_q_identity(), rb = jce_q_identity();
    jce_physics_vehicle_get_chassis_transform(wa, va, &pa, &ra);
    jce_physics_vehicle_get_chassis_transform(wb, vb, &pb, &rb);
    TEST_ASSERT_TRUE(finite3(pa));
    TEST_ASSERT_TRUE(finite3(pb));

    /* Same build, same input -> Bullet is deterministic. */
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, pa.x, pb.x);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, pa.y, pb.y);
    TEST_ASSERT_FLOAT_WITHIN(1e-3f, pa.z, pb.z);

    jce_physics_vehicle_destroy(wa, va);
    jce_physics_vehicle_destroy(wb, vb);
    jce_physics_destroy(wa);
    jce_physics_destroy(wb);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_rest_on_ground);
    RUN_TEST(test_throttle_drives_forward);
    RUN_TEST(test_brake_stops);
    RUN_TEST(test_wheel_transforms_sane);
    RUN_TEST(test_invalid_handle_safe);
    RUN_TEST(test_determinism);
    return UNITY_END();
}
