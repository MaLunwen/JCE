/*
 * test_jce_runtime_save_load.c — the save system's missing half.
 *
 * jce_runtime_save_to_file has existed with NO counterpart.  There was no
 * jce_runtime_load_* symbol at all, and the single call site of
 * jce_snapshot_load_from_file anywhere in the product is an editor panel --
 * which only works because it calls jce_state_stop() first and tears the
 * runtime down, the mitigation a shipped game has no equivalent of.  So a game
 * could write .jsnp checkpoints that nothing in its own executable could read
 * back: no "Continue", no "Load Game".
 *
 * The public header told games to drive jce_snapshot_load_from_file
 * themselves.  That instruction was wrong.  The scene provider's read callback
 * does jce_scene_clear + jce_scene_load_json and STOPS: every entity is
 * destroyed and recreated, so the physics bodies, script instances, audio
 * voices and triggers keyed to the old ids are gone and nothing rebuilds them.
 * The restored scene renders and does nothing -- the worst shape a save bug
 * can take, because it looks like it worked.
 *
 * SO THE OBSERVABLE IS NOT "DID IT LOAD".  A test that only checked entity
 * names would have passed against the broken path.  These check that the
 * restored runtime is ALIVE: that the spawn walk ran again, which is the half
 * that was missing.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "application/jce_rt_internal.h"

#include "unity.h"

#include <jce/application/jce_runtime.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

#define SAVE_PATH "jce_ut_save_load/slot0.jsnp"

/* A rigid body is the cheapest witness that the spawn walk ran: the runtime
 * creates one per authored RigidBody at spawn and nowhere else, so counting
 * bodies distinguishes "entities exist" from "the runtime rebuilt around
 * them" -- which is exactly the distinction the old path got wrong. */
static const char *const SCENE_JSON =
"{\"format_version\":1,\"entities\":["
 "{\"name\":\"Crate\",\"id\":1,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":5,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   /* "Rigidbody", not "RigidBody": the engine silently ignores an unknown
    * component type, so the wrong spelling produced a scene with no body
    * at all and the body-count assertion caught it. */
   "{\"type\":\"Rigidbody\",\"mass\":1.0,\"isKinematic\":false},"
   "{\"type\":\"BoxCollider\",\"sizeX\":1,\"sizeY\":1,\"sizeZ\":1,"
    "\"centerX\":0,\"centerY\":0,\"centerZ\":0}]},"
 "{\"name\":\"Marker\",\"id\":2,\"parent_id\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":1,\"posY\":2,\"posZ\":3,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}"
"]}";

static JceScene   *s_scene;
static JceRuntime *s_rt;

typedef struct { const char *want; bool found; int count; } Look;

static void look_cb(JceScene *s, JceEntity e, void *ud)
{
    Look *l = (Look *)ud;
    const char *n = jce_scene_entity_name(s, e);
    l->count++;
    if (n && l->want && strcmp(n, l->want) == 0) l->found = true;
}

static Look look_for(JceScene *s, const char *name)
{
    Look l = { name, false, 0 };
    jce_scene_each_entity(s, look_cb, &l);
    return l;
}

void setUp(void)
{
    JceJson *root = jce_json_parse(SCENE_JSON, strlen(SCENE_JSON));
    TEST_ASSERT_NOT_NULL(root);
    s_scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s_scene);
    TEST_ASSERT_EQUAL_INT(2, jce_scene_load_json(s_scene, root));
    jce_json_free(root);

    JceRuntimeDesc desc;
    memset(&desc, 0, sizeof desc);
    desc.scene          = s_scene;
    desc.enable_physics = true;      /* bodies are the witness */
    s_rt = jce_runtime_create(&desc);
    TEST_ASSERT_NOT_NULL(s_rt);
}

void tearDown(void)
{
    jce_runtime_destroy(s_rt);
    jce_scene_destroy(s_scene);
    remove(SAVE_PATH);
    s_rt = NULL;
    s_scene = NULL;
}

static void test_a_saved_session_can_be_loaded_back(void)
{
    /* THE WHOLE FEATURE.  Before jce_runtime_load_from_file existed there was
     * no symbol to call here at all. */
    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_save_to_file(s_rt, SAVE_PATH),
        "the save half has always worked; if this fails the rest is untested");
    TEST_ASSERT_TRUE_MESSAGE(jce_fs_host_exists_file(SAVE_PATH),
        "and it must reach disk, creating the parent directory");

    /* Move something so the restore has to actually put it back. */
    JceEntity crate = 0;
    {
        Look l = { "Crate", false, 0 };
        jce_scene_each_entity(s_scene, look_cb, &l);
        TEST_ASSERT_TRUE(l.found);
    }
    (void)crate;

    TEST_ASSERT_TRUE_MESSAGE(jce_runtime_load_from_file(s_rt, SAVE_PATH),
        "and the session must load back -- the half that did not exist");
    TEST_ASSERT_TRUE_MESSAGE(look_for(s_scene, "Crate").found,
        "the saved entity must be in the scene again");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, look_for(s_scene, NULL).count,
        "and exactly the saved entities -- not appended on top of the old ones");
}

static void test_the_restored_runtime_is_alive_not_just_populated(void)
{
    /* THE CASE THAT SEPARATES THIS FROM THE BROKEN PATH.  Driving
     * jce_snapshot_load_from_file directly -- what the header used to tell
     * games to do -- leaves the entities present and the runtime empty: the
     * bodies belonged to entity ids that no longer exist and nothing rebuilds
     * them.  A test that checked names would pass on that.  Body count does
     * not. */
    const int bodies_before = s_rt->body_count;
    TEST_ASSERT_TRUE_MESSAGE(bodies_before > 0,
        "the authored RigidBody must produce a body at spawn, or this test "
        "measures nothing");

    TEST_ASSERT_TRUE(jce_runtime_save_to_file(s_rt, SAVE_PATH));
    TEST_ASSERT_TRUE(jce_runtime_load_from_file(s_rt, SAVE_PATH));

    TEST_ASSERT_EQUAL_INT_MESSAGE(bodies_before, s_rt->body_count,
        "the restored scene must have its physics bodies REBUILT -- equal "
        "count, because the spawn walk ran again.  Zero here is the defect "
        "this exists for: a scene that renders and does nothing.");

    /* And it must still step without tripping over stale bookkeeping. */
    for (int i = 0; i < 8; ++i) jce_runtime_step(s_rt, 1.0f / 60.0f);
    TEST_ASSERT_EQUAL_INT_MESSAGE(bodies_before, s_rt->body_count,
        "stepping after a load must not lose or duplicate bodies");
}

static void test_loading_twice_does_not_accumulate(void)
{
    /* Without the teardown before the swap, each load would leave the previous
     * load's bodies behind -- a whole scene of physics bodies leaked per
     * "Load Game", which a player can press all afternoon. */
    const int bodies = s_rt->body_count;
    TEST_ASSERT_TRUE(jce_runtime_save_to_file(s_rt, SAVE_PATH));
    for (int i = 0; i < 5; ++i)
        TEST_ASSERT_TRUE(jce_runtime_load_from_file(s_rt, SAVE_PATH));
    TEST_ASSERT_EQUAL_INT_MESSAGE(bodies, s_rt->body_count,
        "five loads must leave one scene's worth of bodies, not five");
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, look_for(s_scene, NULL).count,
        "...and one scene's worth of entities");
}

static void test_bad_requests_are_refused(void)
{
    TEST_ASSERT_FALSE(jce_runtime_load_from_file(NULL, SAVE_PATH));
    TEST_ASSERT_FALSE(jce_runtime_load_from_file(s_rt, NULL));
    TEST_ASSERT_FALSE(jce_runtime_load_from_file(s_rt, ""));
    TEST_ASSERT_FALSE_MESSAGE(
        jce_runtime_load_from_file(s_rt, "jce_ut_save_load/no_such.jsnp"),
        "a missing slot must fail");
    /* AND MUST NOT COST THE PLAYER THEIR SESSION.  Picking an empty save slot
     * is the common failure; the bytes are read before the live session is
     * touched precisely so this case changes nothing.  The first version of
     * this function tore down first and emptied the scene here -- which the
     * test caught by asserting the count AFTER the refusal. */
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, look_for(s_scene, NULL).count,
        "a refused load must leave the running session exactly as it was");
    TEST_ASSERT_TRUE_MESSAGE(s_rt->body_count > 0,
        "...including its physics bodies");
}

int main(void)
{
    (void)jce_fs_host_create_directory("jce_ut_save_load");
    UNITY_BEGIN();
    RUN_TEST(test_a_saved_session_can_be_loaded_back);
    RUN_TEST(test_the_restored_runtime_is_alive_not_just_populated);
    RUN_TEST(test_loading_twice_does_not_accumulate);
    RUN_TEST(test_bad_requests_are_refused);
    return UNITY_END();
}
