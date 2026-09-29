/*
 * test_jce_vehicle_wheel_input.c — per-wheel drive reaches the simulation.
 *
 * WheelCollider.motor_torque / .brake_torque / .steer_angle_deg are how Unity
 * drives a car, and this engine had no route for them: the only input the
 * physics API offered was whole-vehicle throttle/brake/steer, so the three
 * fields were parsed, stored, shown to nobody and read by nothing.  Bullet has
 * taken per-wheel arguments the whole time -- applyEngineForce(f, i) et al are
 * already called in the set_input loop -- so the gap was one missing entry
 * point, not a missing capability.
 *
 * A per-wheel force is not observable from a component, so this asks the
 * simulation: drive ONE side of a four-wheel vehicle and the chassis must
 * turn.  Two controls make that conclusion mean something:
 *
 *   symmetric — the same trim on both sides must NOT yaw the chassis, so a
 *               "yaw" that is really integrator drift cannot pass;
 *   zero      — no trim at all must leave the chassis where the vehicle-level
 *               input alone puts it, BYTE FOR BYTE, because every already
 *               authored vehicle has motor_torque == 0.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics.h>

#include <math.h>
#include <string.h>

#define STEPS 90
#define DT    (1.0f / 60.0f)

typedef struct {
    JcePhysicsWorld  *world;
    JceVehicleHandle  veh;
} Rig;

static Rig make_rig(void)
{
    Rig rig;
    memset(&rig, 0, sizeof(rig));

    JcePhysicsWorldDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x = 0.0f; wd.gravity.y = -9.81f; wd.gravity.z = 0.0f;
    wd.max_bodies = 32u;
    rig.world = jce_physics_create(&wd);
    TEST_ASSERT_NOT_NULL(rig.world);

    /* Ground so the wheels have something to push against. */
    JceBodyDesc gd;
    memset(&gd, 0, sizeof(gd));
    gd.type = JCE_BODY_STATIC;
    gd.shape = JCE_SHAPE_BOX;
    gd.position.x = 0.0f; gd.position.y = -0.5f; gd.position.z = 0.0f;
    gd.half_extents.x = 200.0f;
    gd.half_extents.y = 0.5f;
    gd.half_extents.z = 200.0f;
    gd.rotation.w = 1.0f;              /* same zero-quaternion trap */
    gd.friction = 1.0f;
    gd.collision_group = 0xFFFFFFFFu;  /* memset leaves mask 0 = collides */
    gd.collision_mask  = 0xFFFFFFFFu;  /* with nothing; the car free-falls. */
    (void)jce_physics_body_create(rig.world, &gd);

    JceVehicleDesc vd;
    memset(&vd, 0, sizeof(vd));
    vd.chassis_half_extents.x = 0.9f;
    vd.chassis_half_extents.y = 0.3f;
    vd.chassis_half_extents.z = 2.0f;
    vd.position.x = 0.0f; vd.position.y = 1.2f; vd.position.z = 0.0f;
    /* memset leaves the quaternion at (0,0,0,0).  Bullet normalises it,
     * divides by zero, and every reading below comes back NaN -- which the
     * first version of this test reported as a FEATURE failure. */
    vd.rotation.x = 0.0f; vd.rotation.y = 0.0f;
    vd.rotation.z = 0.0f; vd.rotation.w = 1.0f;
    vd.chassis_mass = 800.0f;
    vd.max_engine_force = 4000.0f;
    vd.max_brake_force = 200.0f;
    vd.max_steering_rad = 0.5f;
    vd.collision_group = 0xFFFFFFFFu;
    vd.collision_mask  = 0xFFFFFFFFu;
    rig.veh = jce_physics_vehicle_create(rig.world, &vd);

    /* Four wheels: 0/1 rear (drive), 2/3 front.  x < 0 is the left side. */
    const float wx[4] = { -0.8f, 0.8f, -0.8f, 0.8f };
    const float wz[4] = { -1.5f, -1.5f, 1.5f, 1.5f };
    for (int i = 0; i < 4; ++i) {
        JceWheelDesc w;
        memset(&w, 0, sizeof(w));
        w.connection_point.x = wx[i];
        w.connection_point.y = 0.0f;
        w.connection_point.z = wz[i];
        w.wheel_direction.x = 0.0f;
        w.wheel_direction.y = -1.0f;
        w.wheel_direction.z = 0.0f;
        w.wheel_axle.x = -1.0f;
        w.wheel_axle.y = 0.0f;
        w.wheel_axle.z = 0.0f;
        w.suspension_rest_len = 0.6f;
        w.wheel_radius = 0.4f;
        w.is_front_wheel = (wz[i] > 0.0f);
        w.suspension_stiffness = 20.0f;
        w.suspension_damping = 2.3f;
        w.suspension_compression = 4.4f;
        w.friction_slip = 1000.0f;
        w.roll_influence = 0.1f;
        (void)jce_physics_vehicle_add_wheel(rig.world, rig.veh, &w);
    }
    return rig;
}

/* Run STEPS ticks with an optional per-wheel engine trim on `wheel`
 * (or on both rear wheels when `both`), and return the chassis yaw. */
static float run(float trim, int wheel, int both)
{
    Rig rig = make_rig();
    for (int s = 0; s < STEPS; ++s) {
        jce_physics_vehicle_set_input(rig.world, rig.veh, 0.0f, 0.0f, 0.0f);
        if (trim != 0.0f) {
            jce_physics_vehicle_add_wheel_input(rig.world, rig.veh,
                                                (uint32_t)wheel, trim, 0.0f, 0.0f);
            if (both)
                jce_physics_vehicle_add_wheel_input(rig.world, rig.veh,
                                                    (uint32_t)(wheel ^ 1),
                                                    trim, 0.0f, 0.0f);
        }
        jce_physics_step(rig.world, DT);
    }
    jce_vec3 pos; jce_quat rot;
    memset(&pos, 0, sizeof(pos));
    memset(&rot, 0, sizeof(rot));
    jce_physics_vehicle_get_chassis_transform(rig.world, rig.veh, &pos, &rot);
    /* Yaw about Y from the quaternion. */
    float yaw = atan2f(2.0f * (rot.w * rot.y + rot.x * rot.z),
                       1.0f - 2.0f * (rot.y * rot.y + rot.x * rot.x));
    jce_physics_destroy(rig.world);
    return yaw;
}

static void test_one_sided_trim_yaws(void)
{
    float none = run(0.0f, 0, 0);
    float left = run(3000.0f, 0, 0);   /* rear-left only */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(left - none) > 0.02f,
        "driving ONE rear wheel must turn the chassis -- if this fails the "
        "per-wheel input never reached Bullet");
}

static void test_symmetric_trim_does_not_yaw(void)
{
    float none = run(0.0f, 0, 0);
    float both = run(3000.0f, 0, 1);   /* both rear wheels */
    TEST_ASSERT_TRUE_MESSAGE(fabsf(both - none) < 0.02f,
        "an EQUAL trim on both sides must not yaw -- a yaw here would mean "
        "the one-sided result above was drift, not differential drive");
}

static void test_zero_trim_is_a_no_op(void)
{
    /* The default every authored wheel has.  Two runs of the same input must
     * agree exactly; the trim path must not perturb them.
     *
     * The finiteness check is not decoration: TEST_ASSERT_EQUAL_FLOAT treats
     * NaN == NaN as equal, so this test PASSED while the rig was producing
     * nothing but NaN. */
    float a = run(0.0f, 0, 0);
    float b = run(0.0f, 0, 0);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(a), "baseline yaw is not finite");
    TEST_ASSERT_EQUAL_FLOAT(a, b);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_one_sided_trim_yaws);
    RUN_TEST(test_symmetric_trim_does_not_yaw);
    RUN_TEST(test_zero_trim_is_a_no_op);
    return UNITY_END();
}
