/*
 * test_jce_zeroed_component_serialises.c — a zeroed component through the
 * public setter crashed jce_scene_serial_save.
 *
 *     JceMeshRenderer mr = {0};
 *     mr.visible = true;
 *     jce_scene_set_mesh_renderer(s, e, &mr);
 *     jce_scene_serial_save(s, &len);        <-- segfault
 *
 * Seven of JceMeshRenderer's fields are INTERNED strings: `const char *` into
 * the scene's pool.  Every path inside the engine interns them, so "never
 * NULL" was assumed by every consumer and enforced by none -- and `{0}` is
 * exactly what the public API's shape invites, what a script binding produces,
 * and what a user project writes.  The serialiser assumed it twice: strlen on
 * the pointer, and `mr->albedo_tex[0]` guards that dereference before they can
 * decide.
 *
 * FOUND BY A MEASUREMENT, not by review: a harness written to time undo
 * snapshots segfaulted on its first 100-entity scene.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_json.h>
#include <jce/resource/jce_scene_serial.h>

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

static void test_a_zeroed_mesh_renderer_survives_a_save(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "zeroed");
    JceTransform t;
    memset(&t, 0, sizeof t);
    t.scale = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(s, e, &t);

    /* THE CALL THAT USED TO CRASH.  Nothing here is unusual -- it is the
     * smallest thing a caller can write that puts a renderer on an entity. */
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.visible = true;
    jce_scene_set_mesh_renderer(s, e, &mr);

    size_t len = 0;
    char *json = jce_scene_serial_save(s, &len);
    TEST_ASSERT_NOT_NULL_MESSAGE(json,
        "serialising a scene with a zeroed MeshRenderer must not fail");
    TEST_ASSERT_TRUE(len > 0);
    jce_json_free_string(json);
    jce_scene_destroy(s);
}

static void test_the_stored_component_has_no_null_strings(void)
{
    /* The invariant itself, read back through the getter: the fix is not "the
     * serialiser copes" but "the component never holds NULL", which is what
     * every OTHER consumer -- the draw path, the editor, the bindings --
     * assumes as well.  A serialiser-only guard would leave the same NULL
     * reachable from a draw call. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "zeroed");

    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    jce_scene_set_mesh_renderer(s, e, &mr);

    const JceMeshRenderer *got = jce_scene_get_mesh_renderer(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_NOT_NULL_MESSAGE(got->mesh_path,     "mesh_path");
    TEST_ASSERT_NOT_NULL_MESSAGE(got->material_path, "material_path");
    TEST_ASSERT_NOT_NULL_MESSAGE(got->albedo_tex,    "albedo_tex");
    TEST_ASSERT_NOT_NULL_MESSAGE(got->mr_tex,        "mr_tex");
    TEST_ASSERT_NOT_NULL_MESSAGE(got->normal_tex,    "normal_tex");
    TEST_ASSERT_NOT_NULL_MESSAGE(got->ao_tex,        "ao_tex");
    TEST_ASSERT_NOT_NULL_MESSAGE(got->emissive_tex,  "emissive_tex");
    /* Empty, not merely non-NULL: a caller that set nothing must read back as
     * "no mesh", which is what `path[0]` tests all over the engine expect. */
    TEST_ASSERT_EQUAL_STRING("", got->mesh_path);
    TEST_ASSERT_EQUAL_STRING("", got->albedo_tex);

    jce_scene_destroy(s);
}

static void test_an_authored_path_is_left_alone(void)
{
    /* The normalisation must not touch what the caller did set -- otherwise
     * it would quietly blank every mesh in every scene. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "authored");

    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.mesh_path  = jce_scene_intern(s, "Models/rock.glb");
    mr.albedo_tex = jce_scene_intern(s, "Textures/rock.png");
    jce_scene_set_mesh_renderer(s, e, &mr);

    const JceMeshRenderer *got = jce_scene_get_mesh_renderer(s, e);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_STRING("Models/rock.glb",   got->mesh_path);
    TEST_ASSERT_EQUAL_STRING("Textures/rock.png", got->albedo_tex);
    TEST_ASSERT_EQUAL_STRING("", got->material_path);

    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_a_zeroed_mesh_renderer_survives_a_save);
    RUN_TEST(test_the_stored_component_has_no_null_strings);
    RUN_TEST(test_an_authored_path_is_left_alone);
    return UNITY_END();
}
