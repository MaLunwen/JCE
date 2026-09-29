/*
 * test_jce_collider2d_trigger.c — Collider2D.is_trigger reaches the shape.
 *
 * JceBody2DDesc had no sensor field at all, and attach_shape never touched
 * b2ShapeDef.isSensor, so "Is Trigger" -- authored, serialised, in the
 * Inspector -- produced a solid wall.  Nothing errored; a trigger just
 * behaved like a collider, which is the failure mode that looks like level
 * design rather than a bug.
 *
 * The probe is the thing a trigger is FOR: drop a dynamic box onto a static
 * one and ask whether it was stopped.
 *
 *   solid  -> the faller rests ON the platform      (y stays above it)
 *   sensor -> the faller passes THROUGH             (y ends well below)
 *
 * Both directions are asserted, so a build that made everything a sensor
 * fails just as loudly as one that made nothing a sensor.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/physics/jce_physics2d.h>

#include <math.h>
#include <stdbool.h>
#include <string.h>

#define STEPS 120
#define DT    (1.0f / 60.0f)

static JcePhysics2D *make_world(void)
{
    JcePhysics2DDesc wd;
    memset(&wd, 0, sizeof(wd));
    wd.gravity.x  = 0.0f;
    wd.gravity.y  = -9.81f;
    wd.max_bodies = 16u;
    return jce_physics2d_create(&wd);
}

/* Static platform at y = 0, half-extents 4 x 0.5.  `sensor` decides whether
 * it is solid.  Returns the faller's y after STEPS ticks. */
static float drop_onto(bool sensor)
{
    JcePhysics2D *w = make_world();
    TEST_ASSERT_NOT_NULL(w);

    JceBody2DDesc pd;
    memset(&pd, 0, sizeof(pd));
    pd.type = JCE_BODY_STATIC;
    pd.shape = JCE_SHAPE2D_BOX;
    pd.position.x = 0.0f; pd.position.y = 0.0f;
    pd.half_extents.x = 4.0f; pd.half_extents.y = 0.5f;
    pd.sensor = sensor;
    (void)jce_physics2d_body_create(w, &pd);

    JceBody2DDesc fd;
    memset(&fd, 0, sizeof(fd));
    fd.type = JCE_BODY_DYNAMIC;
    fd.shape = JCE_SHAPE2D_BOX;
    fd.position.x = 0.0f; fd.position.y = 3.0f;
    fd.half_extents.x = 0.25f; fd.half_extents.y = 0.25f;
    fd.mass = 1.0f;
    JceBodyHandle faller = jce_physics2d_body_create(w, &fd);

    for (int i = 0; i < STEPS; ++i)
        jce_physics2d_step(w, DT);

    jce_vec2 p; float ang = 0.0f;
    memset(&p, 0, sizeof(p));
    jce_physics2d_body_get_transform(w, faller, &p, &ang);
    jce_physics2d_destroy(w);
    return p.y;
}

static void test_solid_platform_stops_the_faller(void)
{
    float y = drop_onto(false);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(y), "solid drop produced a non-finite y");
    TEST_ASSERT_TRUE_MESSAGE(y > 0.4f,
        "a NON-trigger platform must stop the falling box");
}

static void test_trigger_platform_is_passed_through(void)
{
    float y = drop_onto(true);
    TEST_ASSERT_TRUE_MESSAGE(isfinite(y), "sensor drop produced a non-finite y");
    TEST_ASSERT_TRUE_MESSAGE(y < -2.0f,
        "a TRIGGER platform must not stop the falling box -- if this fails, "
        "is_trigger still has nowhere to go and the trigger is a wall");
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_solid_platform_stops_the_faller);
    RUN_TEST(test_trigger_platform_is_passed_through);
    return UNITY_END();
}
