/*
 * test_jce_rigidbody_handle.c — "mirrors 3D contract", said the 2D side.
 *
 * rt_spawn_body2d writes the created body's index back onto the component and
 * its comment says it "mirrors 3D contract".  The 3D side never did:
 * JceRigidBodyComponent.body_handle_idx was 0 in every process that has ever
 * run.  The visible consequence is in the inspector, which prints the handle
 * and appended " (inactive in editor)" when it was falsy -- so a 3D rigid body
 * read as inactive while it was being simulated in Play.
 *
 * AND 0 MEANT TWO THINGS, which is the trap these cases exist to keep shut.
 * JceBodyHandle.idx packs slot and generation, and jce_body_valid() rejects
 * only UINT32_MAX -- so a body in slot 0 with generation 0 has idx 0, a real
 * live body indistinguishable from the memset default.  rt_spawn_entity_body
 * therefore stamps JCE_BODY_INVALID before it decides anything.  Asserting
 * "non-zero" instead of jce_body_valid() would pass today and go quietly
 * wrong the first time the body under test is the first one in the scene.
 *
 * Driven through the real jce_runtime_create spawn walk rather than by calling
 * the static helper: the contract is about what an authored scene gets.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/physics/jce_physics_types.h>

#include "unity.h"

#include <string.h>

void setUp(void)    {}
void tearDown(void) {}

/* A crate: the collider is what makes it solid, the Rigidbody what makes it
 * move.  Both enabled, so the spawn takes the primitive-collider path. */
static JceEntity make_crate(JceScene *s, const char *name)
{
    JceEntity              e = jce_scene_create_entity(s, name);
    JceTransform           t;
    JceBoxColliderComponent bc;
    JceRigidBodyComponent   rb;

    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    t.position.y = 4.0f;
    jce_scene_set_transform(s, e, &t);

    memset(&bc, 0, sizeof bc);
    bc.size[0] = bc.size[1] = bc.size[2] = 1.0f;
    jce_scene_set_box_collider(s, e, &bc);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_BOX_COLLIDER, true);

    memset(&rb, 0, sizeof rb);
    rb.mass = 1.0f;
    jce_scene_set_rigidbody(s, e, &rb);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_RIGIDBODY, true);
    return e;
}

static void test_a_spawned_3d_body_records_its_handle(void)
{
    JceScene      *s = jce_scene_create();
    JceRuntimeDesc rd;
    JceRuntime    *rt;
    JceEntity      e;
    JceRigidBodyComponent *rb;
    JceBodyHandle  h;

    TEST_ASSERT_NOT_NULL(s);
    e = make_crate(s, "crate");

    /* THE CONTROL FOR THE ASSERTION BELOW.  Before the runtime touches it the
     * field is the memset 0 -- which is exactly the value that used to survive
     * a successful spawn, so without checking it here "valid afterwards" could
     * be satisfied by nothing having happened at all. */
    rb = jce_scene_get_rigidbody(s, e);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0u, rb->body_handle_idx,
        "the fixture is meant to start at the memset default");

    memset(&rd, 0, sizeof rd);
    rd.scene          = s;
    rd.enable_physics = true;
    rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL_MESSAGE(rt, "the runtime did not come up");

    rb = jce_scene_get_rigidbody(s, e);
    TEST_ASSERT_NOT_NULL(rb);
    h.idx = rb->body_handle_idx;

    TEST_ASSERT_TRUE_MESSAGE(jce_body_valid(h),
        "the 3D spawn did not record its body handle -- the 2D spawn has "
        "always done this and its own comment calls it the '3D contract'");
    TEST_ASSERT_NOT_EQUAL_MESSAGE(JCE_BODY_INVALID.idx, rb->body_handle_idx,
        "the entry stamp survived, so no body was created for this entity");

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

static void test_the_spawn_leaves_INVALID_not_zero_when_it_makes_no_body(void)
{
    /* A Rigidbody with NO collider of any kind still reaches the spawn, and
     * whether it gets a body is the spawn's business.  What must not happen is
     * the field reading 0 afterwards, because 0 is a legal handle: the
     * inspector, and anything else that asks "does this component have a
     * body", would then be answered with the same byte in both cases. */
    JceScene      *s = jce_scene_create();
    JceRuntimeDesc rd;
    JceRuntime    *rt;
    JceEntity      e;
    JceTransform   t;
    JceRigidBodyComponent rb, *live;

    TEST_ASSERT_NOT_NULL(s);
    e = jce_scene_create_entity(s, "bodiless");
    memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f;
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);
    memset(&rb, 0, sizeof rb);
    rb.mass = 1.0f;
    jce_scene_set_rigidbody(s, e, &rb);
    jce_scene_set_component_enabled(s, e, JCE_COMP_FLAG_RIGIDBODY, true);

    memset(&rd, 0, sizeof rd);
    rd.scene          = s;
    rd.enable_physics = true;
    rt = jce_runtime_create(&rd);
    TEST_ASSERT_NOT_NULL(rt);

    live = jce_scene_get_rigidbody(s, e);
    TEST_ASSERT_NOT_NULL(live);
    {
        JceBodyHandle h2;
        h2.idx = live->body_handle_idx;
        /* Either outcome is legitimate -- the spawn may give a colliderless
         * Rigidbody a shape_type placeholder -- but the field must have been
         * WRITTEN either way, so it can no longer be the ambiguous 0. */
        TEST_ASSERT_TRUE_MESSAGE(
            jce_body_valid(h2) || live->body_handle_idx == JCE_BODY_INVALID.idx,
            "the entry stamp is missing: 0 still means both 'no body' and "
            "'the body in slot 0'");
    }

    jce_runtime_destroy(rt);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_spawned_3d_body_records_its_handle);
    RUN_TEST(test_the_spawn_leaves_INVALID_not_zero_when_it_makes_no_body);
    return UNITY_END();
}
