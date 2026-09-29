/*
 * test_jce_entity_scoped_restore.c — the mechanism an entity-scoped undo
 * record is built on.
 *
 * The editor captured undo by serialising the WHOLE SCENE, twice per edit:
 * once to push the record, once to compare strings and decide whether anything
 * changed.  Measured here (one component per entity):
 *
 *     entities   save_ms   json_MB   load_ms
 *          100       1.7      0.05       1.4
 *        1,000      23.7      0.52      14.7
 *       10,000     159.8      5.19     159.8
 *       50,000     715.2     26.05    1109.2
 *
 * so a slider release on a 50k-entity scene cost ~1.4 s and a 26 MB string
 * comparison, and the undo itself CLEARED the scene -- every entity handle
 * reissued, the selection dropped, both occlusion cullers reset.
 *
 * An inspector edit changes one entity, and both halves of the primitive
 * needed to say so were already public.  This asserts the algorithm the
 * editor's scoped record now runs, through those same public calls:
 *
 *     capture  = serialize_entity_components + the component SET
 *     restore  = remove what the set lacks, then parse the components back
 *
 * The SET is the half that is easy to leave out: jce_scene_parse_entity_json
 * only ADDS and overwrites, so without it an undo cannot remove a component
 * the edit added -- it restores every old value and silently leaves the new
 * component behind.
 *
 * WHAT THIS DOES NOT COVER: the editor-side plumbing (which entity is in
 * scope, when the record is pushed).  jce_editor_history.cpp cannot be linked
 * without most of the editor, so that half is covered by the gates and by use,
 * not by this file.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

void setUp(void) {}
void tearDown(void) {}

/* The capture half. */
static char *capture_entity(JceScene *s, JceEntity e,
                            int *out_ids, int *out_n)
{
    JceJson *comps = jce_scene_serialize_entity_components(s, e);
    TEST_ASSERT_NOT_NULL(comps);
    JceJson *obj = jce_json_object();
    TEST_ASSERT_NOT_NULL(obj);
    jce_json_set_child(obj, "components", comps);
    char *text = jce_json_print(obj, false);
    jce_json_free(obj);

    *out_n = 0;
    const int n = jce_component_count();
    for (int cid = 0; cid < n; ++cid)
        if (jce_scene_has_comp(s, e, cid))
            out_ids[(*out_n)++] = cid;
    return text;
}

/* The restore half. */
static void restore_entity(JceScene *s, JceEntity e,
                           const char *text, const int *ids, int n)
{
    const int total = jce_component_count();
    for (int cid = 0; cid < total; ++cid) {
        if (!jce_scene_has_comp(s, e, cid)) continue;
        int in_record = 0;
        for (int i = 0; i < n; ++i) if (ids[i] == cid) { in_record = 1; break; }
        if (!in_record) jce_scene_remove_comp(s, e, cid);
    }
    JceJson *obj = jce_json_parse(text, strlen(text));
    TEST_ASSERT_NOT_NULL(obj);
    jce_scene_parse_entity_json(s, e, obj);
    jce_json_free(obj);
}

static void test_values_come_back(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "probe");

    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.visible = true;
    mr.base_color[0] = 1.0f; mr.base_color[1] = 0.0f;
    mr.base_color[2] = 0.0f; mr.base_color[3] = 1.0f;
    mr.roughness = 0.25f;
    jce_scene_set_mesh_renderer(s, e, &mr);

    int ids[256], n = 0;
    char *rec = capture_entity(s, e, ids, &n);
    TEST_ASSERT_NOT_NULL(rec);

    /* The edit. */
    JceMeshRenderer *live = jce_scene_get_mesh_renderer(s, e);
    TEST_ASSERT_NOT_NULL(live);
    live->base_color[0] = 0.0f;
    live->base_color[2] = 1.0f;
    live->roughness = 0.9f;

    restore_entity(s, e, rec, ids, n);

    const JceMeshRenderer *back = jce_scene_get_mesh_renderer(s, e);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, back->base_color[0],
        "the value the record holds must come back");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, back->base_color[2]);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, back->roughness);

    jce_json_free_string(rec);
    jce_scene_destroy(s);
}

static void test_a_component_the_edit_ADDED_is_removed(void)
{
    /* THE HALF THAT IS EASY TO LEAVE OUT.  parse_entity_json only adds and
     * overwrites, so a restore that replays the JSON alone would put every
     * old value back and leave the new component sitting there -- an undo
     * that looks like it worked. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "probe");

    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.visible = true;
    jce_scene_set_mesh_renderer(s, e, &mr);

    int ids[256], n = 0;
    char *rec = capture_entity(s, e, ids, &n);
    TEST_ASSERT_NOT_NULL(rec);

    /* The edit adds a component. */
    JcePointLight pl;
    memset(&pl, 0, sizeof pl);
    pl.intensity = 2.0f;
    pl.radius = 10.0f;
    jce_scene_set_point_light(s, e, &pl);
    TEST_ASSERT_TRUE(jce_scene_has_point_light(s, e));

    restore_entity(s, e, rec, ids, n);

    TEST_ASSERT_FALSE_MESSAGE(jce_scene_has_point_light(s, e),
        "undo must REMOVE a component the edit added, which needs the "
        "component SET and not just the values");
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_has_mesh_renderer(s, e),
        "...and must not remove the ones that were there all along");

    jce_json_free_string(rec);
    jce_scene_destroy(s);
}

static void test_a_component_the_edit_REMOVED_comes_back(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "probe");

    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.visible = true;
    mr.roughness = 0.375f;
    jce_scene_set_mesh_renderer(s, e, &mr);

    int ids[256], n = 0;
    char *rec = capture_entity(s, e, ids, &n);
    TEST_ASSERT_NOT_NULL(rec);

    jce_scene_remove_mesh_renderer(s, e);
    TEST_ASSERT_FALSE(jce_scene_has_mesh_renderer(s, e));

    restore_entity(s, e, rec, ids, n);

    TEST_ASSERT_TRUE_MESSAGE(jce_scene_has_mesh_renderer(s, e),
        "undo must bring back a component the edit removed");
    const JceMeshRenderer *back = jce_scene_get_mesh_renderer(s, e);
    TEST_ASSERT_NOT_NULL(back);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.375f, back->roughness,
        "...with its values, not with defaults");

    jce_json_free_string(rec);
    jce_scene_destroy(s);
}

static void test_other_entities_are_untouched(void)
{
    /* The whole point: a scoped restore touches ONE entity.  The full-scene
     * path clears the scene, so this was not even a question before -- and
     * that is exactly what made every undo reissue every handle. */
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity a = jce_scene_create_entity(s, "a");
    JceEntity b = jce_scene_create_entity(s, "b");

    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.visible = true;
    mr.roughness = 0.1f;
    jce_scene_set_mesh_renderer(s, a, &mr);
    mr.roughness = 0.8f;
    jce_scene_set_mesh_renderer(s, b, &mr);

    int ids[256], n = 0;
    char *rec = capture_entity(s, a, ids, &n);
    TEST_ASSERT_NOT_NULL(rec);

    jce_scene_get_mesh_renderer(s, a)->roughness = 0.5f;
    jce_scene_get_mesh_renderer(s, b)->roughness = 0.5f;

    restore_entity(s, a, rec, ids, n);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.1f,
        jce_scene_get_mesh_renderer(s, a)->roughness,
        "the scoped entity is restored");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.5f,
        jce_scene_get_mesh_renderer(s, b)->roughness,
        "and every other entity is left exactly as it is -- including its "
        "handle, which a full-scene restore reissues");
    TEST_ASSERT_TRUE(jce_scene_entity_alive(s, b));

    jce_json_free_string(rec);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_values_come_back);
    RUN_TEST(test_a_component_the_edit_ADDED_is_removed);
    RUN_TEST(test_a_component_the_edit_REMOVED_comes_back);
    RUN_TEST(test_other_entities_are_untouched);
    return UNITY_END();
}
