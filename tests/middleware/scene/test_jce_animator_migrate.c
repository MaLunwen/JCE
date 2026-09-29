/*
 * test_jce_animator_migrate.c — the retired Animator becomes a working
 * SkeletalAnimator on load.
 *
 * JceAnimatorComponent was a strict subset of JceSkeletalAnimatorComponent
 * (clip_name/speed/loop/playing against clip_names[8] + active_clip + blend
 * trees + state machines) and had NO runtime consumer -- only the serialiser.
 * The editor carried a component-wide unwired badge, so it was honest and
 * inert: a component in the Add Component menu that does nothing.
 *
 * It could not be wired as it stood: it has no skeleton binding and the
 * runtime needs one (jce_runtime.c loads skeleton_path as a glTF).  The same
 * glTF supplies the mesh, so MeshRenderer.mesh_path IS the rig -- which is
 * why migrating makes the authored values PLAY rather than merely survive.
 *
 * Asserted:
 *   1. an Animator next to a MeshRenderer becomes a SkeletalAnimator whose
 *      clip/speed/loop/playing are carried over AND whose skeleton_path is
 *      the mesh;
 *   2. with no MeshRenderer the values still migrate (skeleton left empty --
 *      dropping them would lose authored data, and the inspector can fill the
 *      rig in);
 *   3. an explicitly authored SkeletalAnimator WINS -- the migration must not
 *      overwrite one, whichever order the two appear in the file.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>

#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *kScene =
"{\"contract\":\"jce.scene/1\",\"scene\":{\"version\":1,\"entities\":["
 "{\"id\":1,\"name\":\"Rigged\",\"parentId\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"MeshRenderer\",\"meshPath\":\"models/hero.gltf\",\"visible\":true},"
   "{\"type\":\"Animator\",\"clipName\":\"Walk\",\"speed\":2.5,"
    "\"loop\":true,\"playing\":true},"
   "{\"type\":\"EditorMeta\",\"name\":\"Rigged\"}]},"
 "{\"id\":2,\"name\":\"NoMesh\",\"parentId\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"Animator\",\"clipName\":\"Idle\",\"speed\":1.0,"
    "\"loop\":false,\"playing\":false},"
   "{\"type\":\"EditorMeta\",\"name\":\"NoMesh\"}]},"
 "{\"id\":3,\"name\":\"Both\",\"parentId\":0,\"components\":["
   "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
    "\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
   "{\"type\":\"SkeletalAnimator\",\"skeletonPath\":\"models/explicit.gltf\","
    "\"clipNames\":[\"Explicit\"],\"activeClip\":0,"
    "\"speed\":1.0,\"loop\":true,\"playing\":true},"
   "{\"type\":\"Animator\",\"clipName\":\"ShouldLose\",\"speed\":9.0,"
    "\"loop\":true,\"playing\":true},"
   "{\"type\":\"EditorMeta\",\"name\":\"Both\"}]}"
"]}}";

static JceScene *g_scene;

/* The loader REMAPS file ids onto fresh handles, so the probe entities are
 * found by their EditorMeta name.  Not by their clip: test 1 asserts what
 * the clip is, and identifying by it would make that assertion circular. */
typedef struct { const char *want; JceEntity found; } FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *user)
{
    FindCtx *fc = (FindCtx *)user;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    if (m && strcmp(m->name, fc->want) == 0) fc->found = e;
}

static JceEntity ent(const char *name)
{
    FindCtx fc; fc.want = name; fc.found = 0;
    jce_scene_each_entity(g_scene, find_cb, &fc);
    TEST_ASSERT_TRUE_MESSAGE(fc.found != 0,
                             "probe entity not found in the loaded scene");
    return fc.found;
}

static void test_migrates_with_rig_from_the_mesh(void)
{
    JceEntity e = ent("Rigged");
    TEST_ASSERT_FALSE_MESSAGE(jce_scene_has_animator(g_scene, e),
        "the retired Animator must not survive the load");
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_has_skeletal_animator(g_scene, e),
        "an authored Animator must become a SkeletalAnimator");

    JceSkeletalAnimatorComponent *sa =
        jce_scene_get_skeletal_animator(g_scene, e);
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_EQUAL_STRING("Walk", sa->clip_names[0]);
    TEST_ASSERT_EQUAL_INT(1, sa->clip_count);
    TEST_ASSERT_EQUAL_INT(0, sa->active_clip);
    TEST_ASSERT_EQUAL_FLOAT(2.5f, sa->speed);
    TEST_ASSERT_TRUE(sa->loop);
    TEST_ASSERT_TRUE(sa->playing);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("models/hero.gltf", sa->skeleton_path,
        "the rig must come from the MeshRenderer on the same entity -- "
        "without it the migrated component still cannot play");
}

static void test_migrates_without_a_mesh(void)
{
    JceEntity e = ent("NoMesh");
    TEST_ASSERT_TRUE(jce_scene_has_skeletal_animator(g_scene, e));
    JceSkeletalAnimatorComponent *sa =
        jce_scene_get_skeletal_animator(g_scene, e);
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_EQUAL_STRING("Idle", sa->clip_names[0]);
    TEST_ASSERT_FALSE(sa->loop);
    TEST_ASSERT_FALSE(sa->playing);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("", sa->skeleton_path,
        "no MeshRenderer means no rig to guess at");
}

static void test_explicit_skeletal_animator_wins(void)
{
    JceEntity e = ent("Both");
    JceSkeletalAnimatorComponent *sa =
        jce_scene_get_skeletal_animator(g_scene, e);
    TEST_ASSERT_NOT_NULL(sa);
    TEST_ASSERT_EQUAL_STRING_MESSAGE("Explicit", sa->clip_names[0],
        "an explicitly authored SkeletalAnimator must not be overwritten by "
        "the migration, whichever order the two appear in the file");
    TEST_ASSERT_EQUAL_STRING("models/explicit.gltf", sa->skeleton_path);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, sa->speed);
}

void setUp(void) {}
void tearDown(void) {}

int main(void)
{
    g_scene = jce_scene_create();
    if (!g_scene) { printf("scene_create failed\n"); return 1; }
    JceJson *root = jce_json_parse(kScene, strlen(kScene));
    if (!root) { printf("json parse failed\n"); return 1; }
    if (jce_scene_load_json(g_scene, root) < 0) {
        printf("scene load failed\n");
        return 1;
    }
    jce_json_free(root);
    UNITY_BEGIN();
    RUN_TEST(test_migrates_with_rig_from_the_mesh);
    RUN_TEST(test_migrates_without_a_mesh);
    RUN_TEST(test_explicit_skeletal_animator_wins);
    int rc = UNITY_END();
    jce_scene_destroy(g_scene);
    return rc;
}
