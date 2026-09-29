/*
 * test_jce_joint2d_fields.c — the 2D-joint physics primitives the runtime's
 * Joint2D last mile needs.
 *
 * Four JceJoint2DComponent fields were authored, serialised, shown in the
 * Inspector and read by nothing, because the PHYSICS layer had no way to
 * express them:
 *
 *   enable_collision          JcePhysics2DJointDesc had no collideConnected,
 *                             so jointed bodies always passed through each
 *                             other -- Box2D's default, never the author's
 *                             choice;
 *   break_force / break_torque  Box2D has no breakable joint and the reaction
 *                             force was not readable from this API at all, so
 *                             a 2D joint could not break however hard it was
 *                             pulled;
 *   auto_configure_distance   a loader-side computation with no reader.
 *
 * This test covers the two the physics layer now provides.  The third
 * (auto_configure_distance) is spawn-side arithmetic in rt_spawn_joint2d and
 * is not reachable from here without a runtime.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics2d.h>

#include <math.h>
#include <stdbool.h>
#include <stdio.h>
#include <string.h>

#define DT (1.0f / 60.0f)

static JcePhysics2D *make_world(float gy)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x  = 0.0f;
    wd.gravity.y  = gy;
    wd.max_bodies = 16u;
    return jce_physics2d_create(&wd);
}

static JceBodyHandle box(JcePhysics2D *w, float x, float y, float mass)
{
    JceBody2DDesc bd;
    memset(&bd, 0, sizeof(bd));
    bd.type = mass > 0.0f ? JCE_BODY_DYNAMIC : JCE_BODY_STATIC;
    bd.shape = JCE_SHAPE2D_BOX;
    bd.position.x = x; bd.position.y = y;
    bd.half_extents.x = 0.5f; bd.half_extents.y = 0.5f;
    bd.mass = mass;
    return jce_physics2d_body_create(w, &bd);
}

/* Two 1x1 dynamic boxes joined by a SPRING of rest length 0.2 -- shorter than
 * the 1.0 their faces need.  A rigid joint of length 0 would win outright over
 * contact (which is why the first cut of this test could not tell the two
 * cases apart); a spring lets the two forces actually compete, so contact
 * shows up as a larger equilibrium separation. */
static float overlap_separation(bool collide)
{
    JcePhysics2D *w = make_world(0.0f);
    TEST_ASSERT_NOT_NULL(w);
    JceBodyHandle a = box(w, 0.0f, 0.0f, 1.0f);
    JceBodyHandle b = box(w, 0.1f, 0.0f, 1.0f);

    JcePhysics2DJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.kind = JCE_PHYSICS2D_JOINT_SPRING;
    jd.body_a = a;
    jd.body_b = b;
    jd.distance = 0.2f;
    jd.frequency_hz = 2.0f;
    jd.damping_ratio = 0.8f;
    jd.collide_connected = collide;
    JceConstraintHandle h = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE_MESSAGE(jce_constraint_valid(h), "joint create failed");

    for (int i = 0; i < 120; ++i)
        jce_physics2d_step(w, DT);

    jce_vec2 pa, pb; float ang = 0.0f;
    memset(&pa, 0, sizeof(pa)); memset(&pb, 0, sizeof(pb));
    jce_physics2d_body_get_transform(w, a, &pa, &ang);
    jce_physics2d_body_get_transform(w, b, &pb, &ang);
    float dx = pb.x - pa.x, dy = pb.y - pa.y;
    float sep = sqrtf(dx * dx + dy * dy);
    jce_physics2d_destroy(w);
    return sep;
}

static void test_collide_connected(void)
{
    float off = overlap_separation(false);
    float on  = overlap_separation(true);
    printf("  [collide off=%.4f on=%.4f]\n", (double)off, (double)on);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(off) && isfinite(on),
                             "separation is not finite");
    /* Both directions, so a build that turned collision on for EVERY joint
     * fails as loudly as one that never turns it on. */
    TEST_ASSERT_TRUE_MESSAGE(off < 0.30f,
        "with collision OFF the pair must settle at the spring's rest length "
        "(0.2), i.e. overlapping -- a larger value means they collided anyway");
    TEST_ASSERT_TRUE_MESSAGE(on > 0.85f,
        "with collision ON contact must push the 1x1 pair to touching (~1.0) "
        "-- if this fails, collide_connected never reached Box2D");
}

/* A heavy box hanging off a rigid distance joint: the joint carries its
 * weight, so the constraint force is about m*g. */
static float hanging_force(float mass)
{
    JcePhysics2D *w = make_world(-9.81f);
    TEST_ASSERT_NOT_NULL(w);
    JceBodyHandle anchor = box(w, 0.0f, 0.0f, 0.0f);   /* static */
    JceBodyHandle load   = box(w, 0.0f, -2.0f, mass);

    JcePhysics2DJointDesc jd;
    memset(&jd, 0, sizeof(jd));
    jd.kind = JCE_PHYSICS2D_JOINT_DISTANCE;
    jd.body_a = anchor;
    jd.body_b = load;
    jd.distance = 2.0f;
    JceConstraintHandle h = jce_physics2d_joint_create(w, &jd);
    TEST_ASSERT_TRUE(jce_constraint_valid(h));

    for (int i = 0; i < 120; ++i)
        jce_physics2d_step(w, DT);

    float f = jce_physics2d_joint_get_force(w, h);
    jce_physics2d_destroy(w);
    return f;
}

static void test_constraint_force_is_readable_and_scales(void)
{
    float light = hanging_force(1.0f);
    float heavy = hanging_force(10.0f);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(light) && isfinite(heavy),
                             "constraint force is not finite");
    TEST_ASSERT_TRUE_MESSAGE(light > 1.0f,
        "a loaded joint must report a non-trivial constraint force -- 0 would "
        "mean break_force can never trigger");
    /* Ten times the mass, roughly ten times the force.  The point is not the
     * exact ratio but that the reading TRACKS the load: a constant would let
     * break_force fire on every joint or none. */
    TEST_ASSERT_TRUE_MESSAGE(heavy > light * 4.0f,
        "the constraint force must scale with the hanging load");
}

static void test_invalid_handles_report_zero(void)
{
    JcePhysics2D *w = make_world(0.0f);
    JceConstraintHandle bad = JCE_CONSTRAINT_INVALID;
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_physics2d_joint_get_force(w, bad));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_physics2d_joint_get_torque(w, bad));
    TEST_ASSERT_EQUAL_FLOAT(0.0f, jce_physics2d_joint_get_force(NULL, bad));
    jce_physics2d_destroy(w);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_collide_connected);
    RUN_TEST(test_constraint_force_is_readable_and_scales);
    RUN_TEST(test_invalid_handles_report_zero);
    return UNITY_END();
}
