/*
 * test_jce_runtime_vehicle_bridge.c — the SCENE-to-physics vehicle bridge.
 *
 * WHY THIS EXISTS.  tests/middleware/physics/test_jce_vehicle.c covers the
 * public vehicle API -- jce_physics_vehicle_create / _add_wheel / _set_input
 * -- and nothing covered the path a designer actually uses: a
 * JceVehicleComponent on an entity with JceWheelColliderComponent children,
 * turned into a Bullet raycast vehicle by rt_build_vehicle in
 * engine/src/application/jce_rt_physics.c.  Nothing in the suite named
 * wheel_collider or JceVehicleComponent at all.
 *
 * That gap had a cost.  The bridge read the wheel's radius, centre and
 * suspension and IGNORED forward_friction and sideways_friction, passing a
 * hardcoded friction_slip of 1000 instead.  A designer could tune wheel grip
 * and the vehicle handled identically, with no error -- the same "no symptom"
 * shape as the light cookies: an ignored value behaves exactly like a value
 * that happens to equal the default.
 *
 * The authored numbers are MULTIPLIERS on that slip, not replacements: their
 * default is 1.0 and the engine's is 1000, three orders apart, so wiring them
 * as absolutes would have collapsed the grip of every vehicle in every
 * existing scene.  1.0 x 1000 = 1000 keeps them all handling exactly as
 * before, which is why case 1 runs at the default and case 2 asserts a tuned
 * value now bites.
 */

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include <math.h>
#include <stdio.h>   /* snprintf */
#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceEntity add_entity(JceScene *s, const char *name,
                            float x, float y, float z)
{
    JceEntity    e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    t.position.x = x; t.position.y = y; t.position.z = z;
    jce_scene_set_transform(s, e, &t);
    return e;
}

/* A large static box whose top surface sits at y = 0. */
static void add_ground(JceScene *s)
{
    JceEntity               e = add_entity(s, "ground", 0.0f, -5.0f, 0.0f);
    JceBoxColliderComponent box;
    JceRigidBodyComponent   rb;

    memset(&box, 0, sizeof box);
    box.size[0] = 400.0f;      /* full extents, centred on the entity */
    box.size[1] =  10.0f;
    box.size[2] = 400.0f;
    jce_scene_set_box_collider(s, e, &box);

    memset(&rb, 0, sizeof rb);
    rb.mass = 0.0f;            /* static */
    jce_scene_set_rigidbody(s, e, &rb);
}

/* Chassis plus four wheel children, each with the given grip. */
static JceEntity add_vehicle(JceScene *s, float grip)
{
    static const float WX[4] = { -0.9f,  0.9f, -0.9f,  0.9f };
    static const float WZ[4] = {  1.4f,  1.4f, -1.4f, -1.4f };
    JceEntity               chassis = add_entity(s, "chassis", 0.0f, 1.2f, 0.0f);
    JceVehicleComponent     vc;
    JceBoxColliderComponent box;
    int i;

    memset(&box, 0, sizeof box);
    box.size[0] = 1.8f;
    box.size[1] = 1.0f;
    box.size[2] = 4.4f;
    jce_scene_set_box_collider(s, chassis, &box);

    memset(&vc, 0, sizeof vc);
    vc.enabled      = true;      /* designer toggle; default OFF is inert */
    vc.chassis_mass = 1200.0f;
    vc.input_mode   = JCE_VEHICLE_INPUT_PLAYER;
    jce_scene_set_vehicle(s, chassis, &vc);

    /* Unique names, and NOT cosmetically: four siblings all called "wheel"
     * made jce_scene_set_parent ACCESS_VIOLATE on the SECOND one while this
     * test was being written.  Filed separately; the name matters here. */
    for (i = 0; i < 4; ++i) {
        JceEntity                 w;
        JceWheelColliderComponent wc;
        char wname[16];
        snprintf(wname, sizeof wname, "wheel%d", i);
        w = add_entity(s, wname, WX[i], -0.4f, WZ[i]);
        jce_scene_set_parent(s, w, chassis);
        memset(&wc, 0, sizeof wc);
        wc.radius              = 0.4f;
        wc.suspension_distance = 0.3f;
        wc.suspension_spring   = 35000.0f;
        wc.suspension_damper   = 4500.0f;
        wc.mass                = 20.0f;
        /* Equal on purpose: the bridge WARNs when they differ, because the
         * raycast vehicle has one friction slip per wheel and cannot honour
         * both.  This test is about the value arriving, not about that WARN. */
        wc.forward_friction    = grip;
        wc.sideways_friction   = grip;
        jce_scene_set_wheel_collider(s, w, &wc);
    }
    return chassis;
}

/* Settle, then drive a full-lock turn.  Returns where the chassis ended up. */
static float corner_drift(float grip)
{
    JceScene       *s = jce_scene_create();
    JceRuntimeDesc  rd;
    JceRuntime     *rt;
    JceEntity       chassis;
    JceTransform   *tf;
    float           x;
    int             i;

    TEST_ASSERT_NOT_NULL(s);
    add_ground(s);
    chassis = add_vehicle(s, grip);

    memset(&rd, 0, sizeof rd);
    rd.scene = s;
    rd.enable_physics = true;
    rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    for (i = 0; i < 60; ++i) jce_runtime_step(rt, 1.0f / 60.0f);

    tf = jce_scene_get_transform(s, chassis);
    TEST_ASSERT_NOT_NULL(tf);
    TEST_ASSERT_TRUE_MESSAGE(tf->position.y > 0.0f,
        "the chassis fell through the ground - the scene-to-physics bridge "
        "built no wheels at all");

    {
        JceRuntimeInput in;
        memset(&in, 0, sizeof in);
        in.walk_z     = 1.0f;   /* throttle */
        in.walk_x     = 1.0f;   /* full steer */
        in.speed_mult = 1.0f;
        for (i = 0; i < 240; ++i) {
            jce_runtime_set_input(rt, &in);
            jce_runtime_step(rt, 1.0f / 60.0f);
        }
    }

    tf = jce_scene_get_transform(s, chassis);
    x  = tf ? tf->position.x : 0.0f;

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
    return x;
}

/* (1) The bridge builds a vehicle out of scene components, at the DEFAULT
 * grip -- which is the backward-compatibility case: 1.0 x 1000 is exactly the
 * hardcoded slip the bridge used before authored grip was wired. */
static void test_bridge_builds_a_vehicle_that_rests_on_the_ground(void)
{
    (void)corner_drift(1.0f);   /* asserts rest-on-ground internally */
}

/* (2) Authored grip reaches the solver: a wheel with a thousandth of the
 * sideways friction cannot hold a full-lock turn and ends up elsewhere. */
static void test_authored_sideways_friction_changes_the_turn(void)
{
    const float gripped = corner_drift(1.0f);
    const float slippy  = corner_drift(0.001f);

    TEST_ASSERT_TRUE_MESSAGE(fabsf(gripped - slippy) > 0.05f,
        "a wheel authored with a thousandth of the sideways grip took the "
        "same line as one at the default - forward_friction and "
        "sideways_friction are reaching nothing, which is what they did "
        "before this was wired");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_bridge_builds_a_vehicle_that_rests_on_the_ground);
    RUN_TEST(test_authored_sideways_friction_changes_the_turn);
    return UNITY_END();
}
