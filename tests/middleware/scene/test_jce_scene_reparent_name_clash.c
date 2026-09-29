/*
 * test_jce_scene_reparent_name_clash.c
 *
 * Same-named siblings must be able to share a parent, and reparenting must
 * never take the process down.
 *
 * jce_scene_create_entity uniquifies a name already taken IN THE SCOPE IT
 * CREATES INTO, because flecs's name index is unique per scope and
 * ecs_set_name on a taken name ABORTS THE PROCESS -- jce_scene.c:1176 says so
 * in its own comment.  Reparenting moves an entity into a DIFFERENT scope and
 * nothing re-checks the name there, which is why this file exists.
 *
 * WHAT IS ESTABLISHED, and what is not.  On 2026-09-01,
 * tests/application/test_jce_runtime_vehicle_bridge.c created four wheel
 * entities all asking for the name "wheel" and parented them to one chassis:
 * the process died on the SECOND jce_scene_set_parent, before it printed
 * anything.  Renaming them wheel0..wheel3 made it pass.  Both directions were
 * reproduced twice, so the OBSERVATION is solid.
 *
 * THE MECHANISM IS NOT.  Five reproductions were built here, each adding one
 * more thing the vehicle test had, and ALL FIVE PASS:
 *
 *   1. two same-named children under one parent;
 *   2. four of them;
 *   3. each carrying a WheelCollider set after the reparent;
 *   4. a parent carrying a BoxCollider and an enabled VehicleComponent;
 *   5. a static ground entity built first; and linking the FULL engine
 *      rather than jce_core/jce_scene/jce_resource.
 *
 * So this file does not reproduce the crash and is not a regression test for
 * it.  It is a GUARD on the behaviour the crash made me check: same-named
 * siblings sharing a parent must work, both must remain addressable, and the
 * AUTHORED name must survive whatever flecs had to store.  If someone ever
 * "fixes" naming by refusing the second reparent, this goes red.
 *
 * The negative results are listed so the next person does not rebuild them.
 */

#include <jce/middleware/scene/jce_scene.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

static JceEntity mk(JceScene *s, const char *name)
{
    JceEntity    e = jce_scene_create_entity(s, name);
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);
    return e;
}

/* Two children asking for the SAME name, then both parented to one entity. */
static void test_same_named_siblings_can_share_a_parent(void)
{
    JceScene *s = jce_scene_create();
    JceEntity parent, a, b;

    TEST_ASSERT_NOT_NULL(s);
    {   /* the bridge builds a static ground first */
        JceEntity g = mk(s, "ground");
        JceBoxColliderComponent gb;
        JceRigidBodyComponent   rb;
        memset(&gb, 0, sizeof gb);
        gb.size[0] = 400.0f; gb.size[1] = 10.0f; gb.size[2] = 400.0f;
        jce_scene_set_box_collider(s, g, &gb);
        memset(&rb, 0, sizeof rb);
        jce_scene_set_rigidbody(s, g, &rb);
    }
    parent = mk(s, "chassis");
    {   /* the bridge's chassis carries these; try them here */
        JceVehicleComponent vc;
        JceBoxColliderComponent box;
        memset(&box, 0, sizeof box);
        box.size[0] = 1.8f; box.size[1] = 1.0f; box.size[2] = 4.4f;
        jce_scene_set_box_collider(s, parent, &box);
        memset(&vc, 0, sizeof vc);
        vc.enabled = true;
        vc.chassis_mass = 1200.0f;
        jce_scene_set_vehicle(s, parent, &vc);
    }
    a      = mk(s, "wheel");
    b      = mk(s, "wheel");
    {   /* four, and each carrying a component after reparent -- the exact
         * shape of the vehicle bridge, which DOES abort here. */
        JceEntity c = mk(s, "wheel"), d = mk(s, "wheel");
        JceWheelColliderComponent wc;
        memset(&wc, 0, sizeof wc);
        wc.radius = 0.4f;
        jce_scene_set_parent(s, c, parent);
        jce_scene_set_wheel_collider(s, c, &wc);
        jce_scene_set_parent(s, d, parent);
        jce_scene_set_wheel_collider(s, d, &wc);
    }

    TEST_ASSERT_NOT_EQUAL_MESSAGE(a, b, "the two children are the same entity");

    {
        JceWheelColliderComponent wc;
        memset(&wc, 0, sizeof wc);
        wc.radius = 0.4f;
        jce_scene_set_parent(s, a, parent);
        jce_scene_set_wheel_collider(s, a, &wc);
        /* The line that took the process down in the vehicle bridge. */
        jce_scene_set_parent(s, b, parent);
        jce_scene_set_wheel_collider(s, b, &wc);
    }

    TEST_ASSERT_EQUAL_MESSAGE(parent, jce_scene_get_parent(s, a),
                              "first child is not under the parent");
    TEST_ASSERT_EQUAL_MESSAGE(parent, jce_scene_get_parent(s, b),
                              "second child is not under the parent - it was "
                              "refused rather than uniquified");

    /* Both must still be addressable, and the authored name is what the user
     * asked for even when flecs had to store something else. */
    TEST_ASSERT_EQUAL_STRING("wheel", jce_scene_entity_name(s, a));
    TEST_ASSERT_EQUAL_STRING("wheel", jce_scene_entity_name(s, b));

    jce_scene_destroy(s);
}

/* The same shape one level deeper: a child that already has a parent moves to
 * another parent that already holds its name. */
static void test_moving_into_a_scope_that_holds_the_name(void)
{
    JceScene *s = jce_scene_create();
    JceEntity p1, p2, a, b;

    TEST_ASSERT_NOT_NULL(s);
    p1 = mk(s, "left");
    p2 = mk(s, "right");
    a  = mk(s, "wheel");
    b  = mk(s, "wheel");

    jce_scene_set_parent(s, a, p1);
    jce_scene_set_parent(s, b, p2);
    /* Now move b into p1, where a already lives under the same asked-for
     * name. */
    jce_scene_set_parent(s, b, p1);

    TEST_ASSERT_EQUAL_MESSAGE(p1, jce_scene_get_parent(s, b),
                              "the move into an occupied name scope was "
                              "refused");
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_same_named_siblings_can_share_a_parent);
    RUN_TEST(test_moving_into_a_scope_that_holds_the_name);
    return UNITY_END();
}
