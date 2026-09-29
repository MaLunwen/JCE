/*
 * test_jce_static_collider_body.c — a collider alone must be solid.
 *
 * rt_spawn_entity_body bailed before it looked at any collider:
 *
 *     if (!rb || !jce_scene_component_enabled(..., RIGIDBODY)) return false;
 *
 * So "BoxCollider, no RigidBody" -- the way a floor or a wall is authored in
 * every engine, and exactly what the editor's own Add Component > Box Collider
 * leaves you with -- produced NO COLLISION BODY AT ALL.  The player fell
 * through the world while the viewport drew the green collider gizmo over the
 * hole, with no diagnostic anywhere.
 *
 * MeshCollider and CompoundCollider never had this: rt_spawn_cooked_body
 * handles a missing RigidBody with `else { id.type = JCE_BODY_STATIC; }`.  The
 * three PRIMITIVE colliders were the exception, and they are what a level is
 * built out of.
 *
 * THE OBSERVABLE IS rt->body_count AND WHERE A FALLING BOX ENDS UP.  A body
 * existing is not the claim; the claim is that something lands on it.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "application/jce_rt_internal.h"

#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

static JceScene   *s_scene;
static JceRuntime *s_rt;

void setUp(void)    { s_scene = NULL; s_rt = NULL; }
void tearDown(void)
{
    if (s_rt)    jce_runtime_destroy(s_rt);
    if (s_scene) jce_scene_destroy(s_scene);
    s_rt = NULL; s_scene = NULL;
}

static void boot(const char *json)
{
    JceJson *root = jce_json_parse(json, strlen(json));
    TEST_ASSERT_NOT_NULL_MESSAGE(root, "the fixture must parse");
    s_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s_scene);
    TEST_ASSERT_TRUE(jce_scene_load_json(s_scene, root) > 0);
    jce_json_free(root);

    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s_scene;
    d.enable_physics = true;
    s_rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(s_rt);
}

/* Iterate the scene's LIVE entities.  The first version of this walked ids
 * 1..4095 and asked each for its name, which segfaults the moment it reaches an
 * id the scene never issued -- entity ids are not a dense range to be scanned. */
typedef struct { const char *want; JceEntity found; } NameLook;

static void name_cb(JceScene *sc, JceEntity e, void *ud)
{
    NameLook *l = (NameLook *)ud;
    const char *n = jce_scene_entity_name(sc, e);
    if (n && strcmp(n, l->want) == 0) l->found = e;
}

static JceEntity by_name(const char *want)
{
    NameLook l = { want, 0 };
    jce_scene_each_entity(s_scene, name_cb, &l);
    return l.found;
}

/* A BoxCollider floor with NO Rigidbody, and a dynamic crate above it. */
static const char *const FLOOR_AND_CRATE =
"{\"format_version\":1,\"entities\":["
 "{\"name\":\"Floor\",\"id\":1,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"BoxCollider\",\"sizeX\":40,\"sizeY\":1,\"sizeZ\":40,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0}]},"
 "{\"name\":\"Crate\",\"id\":2,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":6,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"Rigidbody\",\"mass\":1.0,\"isKinematic\":false},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0}]}"
"]}";

static void test_a_collider_without_a_rigidbody_gets_a_body(void)
{
    boot(FLOOR_AND_CRATE);
    /* ONE STEP FIRST, and that is a finding in itself.  A small STATIC
     * box/sphere/capsule is deferred at spawn (rt->dd[], big-world lazy
     * spawn) and created by the per-frame pass.  That pass used to give up
     * entirely when no player position was available -- so a scene with no
     * character kept none of its static collision, and this fix would have
     * moved the bug rather than closed it.  It fails open now, which is what
     * this single step exercises. */
    jce_runtime_step(s_rt, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_rt->body_count,
        "BOTH entities must have a body: the crate from its Rigidbody, and the "
        "floor from its BoxCollider alone.  One here is the defect -- the "
        "floor is not solid and nothing says so.");
}

static void test_something_actually_lands_on_it(void)
{
    /* The body existing is not the claim.  Step long enough for a crate
     * dropped from 6 m to fall the ~5 m to the floor's surface and settle;
     * without a floor body it keeps going. */
    boot(FLOOR_AND_CRATE);
    const JceEntity crate = by_name("Crate");
    TEST_ASSERT_TRUE_MESSAGE(crate != 0, "the fixture entity must exist");

    for (int i = 0; i < 240; ++i) jce_runtime_step(s_rt, 1.0f / 60.0f);

    const JceTransform *t = jce_scene_get_transform(s_scene, crate);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_TRUE_MESSAGE(t->position.y > -1.0f,
        "the crate must come to rest ON the floor.  Falling far below it is "
        "the defect: a level whose floor is drawn and not there.");
    TEST_ASSERT_TRUE_MESSAGE(t->position.y < 5.0f,
        "...and it must actually have fallen, or this test would pass on a "
        "crate that never moved");
}

static void test_a_disabled_rigidbody_leaves_the_collider_solid(void)
{
    /* A disabled Rigidbody means "no dynamics", not "dissolve the floor" --
     * the collider beside it is still enabled.  Silently removing the body
     * here is the same defect wearing a different hat. */
    boot(FLOOR_AND_CRATE);
    const JceEntity floor = by_name("Floor");
    TEST_ASSERT_TRUE(floor != 0);
    jce_scene_set_component_enabled(s_scene, floor,
                                    JCE_COMP_FLAG_RIGIDBODY, false);
    /* Re-spawn to re-run the walk with the component disabled. */
    jce_runtime_destroy(s_rt);
    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s_scene;
    d.enable_physics = true;
    s_rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(s_rt);
    jce_runtime_step(s_rt, 1.0f / 60.0f);   /* deferred spawn, see above */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, s_rt->body_count,
        "the floor keeps its static body");
}

static void test_an_entity_with_neither_gets_nothing(void)
{
    /* The early return still has to hold, or every transform in the scene
     * would sprout a placeholder body. */
    static const char *const BARE =
    "{\"format_version\":1,\"entities\":["
     "{\"name\":\"Empty\",\"id\":1,\"parent_id\":0,\"components\":["
       "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
        "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,"
        "\"scaleZ\":1}]}]}";
    boot(BARE);
    jce_runtime_step(s_rt, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(0, s_rt->body_count,
        "a bare Transform must produce no body at all");
}

static void test_a_disabled_collider_and_no_rigidbody_gets_nothing(void)
{
    boot(FLOOR_AND_CRATE);
    const JceEntity floor = by_name("Floor");
    TEST_ASSERT_TRUE(floor != 0);
    jce_scene_set_component_enabled(s_scene, floor,
                                    JCE_COMP_FLAG_BOX_COLLIDER, false);
    jce_runtime_destroy(s_rt);
    JceRuntimeDesc d;
    memset(&d, 0, sizeof d);
    d.scene          = s_scene;
    d.enable_physics = true;
    s_rt = jce_runtime_create(&d);
    TEST_ASSERT_NOT_NULL(s_rt);
    jce_runtime_step(s_rt, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(1, s_rt->body_count,
        "with its only collider disabled and no Rigidbody, the floor has "
        "nothing to make a body from -- just the crate remains");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_collider_without_a_rigidbody_gets_a_body);
    RUN_TEST(test_something_actually_lands_on_it);
    RUN_TEST(test_a_disabled_rigidbody_leaves_the_collider_solid);
    RUN_TEST(test_an_entity_with_neither_gets_nothing);
    RUN_TEST(test_a_disabled_collider_and_no_rigidbody_gets_nothing);
    return UNITY_END();
}
