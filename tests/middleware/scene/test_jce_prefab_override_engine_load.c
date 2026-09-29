/*
 * test_jce_prefab_override_engine_load.c
 *
 * The ENGINE's scene loader must understand the prefab-instance shape the
 * EDITOR writes.  This is the shipped-game half of a two-sided contract, and
 * until 2026-09-01 only one side existed.
 *
 * WHAT THE EDITOR WRITES.  When it saves a scene containing a prefab instance
 * whose source file loads (editor/src/io/jce_editor_scene_serial.cpp), it emits
 * an entity node carrying:
 *
 *     "prefabInstance": true,
 *     "prefabPath":     "...prefab.json",
 *     "overrides":      ["Transform"],          <- names of differing rows
 *     "components":     [ {Transform...} ],     <- ONLY those rows
 *     "children":       []                      <- deliberately EMPTY
 *
 * The children are omitted on purpose: "an instance root's children come
 * ENTIRELY from the source on load".  That is a correct and compact encoding --
 * as long as whoever loads it knows to instantiate the source first.
 *
 * WHAT THE ENGINE DID.  `grep -c '"overrides"'` over jce_scene_serial.c and
 * jce_scene_components_json.c returned 0.  The engine read prefabInstance and
 * prefabPath only to populate JceEditorMeta, and built the entity from the
 * `components` array exactly as written.  So in a SHIPPED game a prefab
 * instance became one bare entity holding only the overridden rows, with every
 * other component and every child silently absent: a twenty-node enemy prefab
 * loaded as a single invisible Transform.
 *
 * Nothing in the build noticed, because the editor loads its own format
 * correctly -- the only configuration in which the scene looks wrong is the one
 * nobody opens during authoring.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 * The tracked contract is tools/lint/check_prefab_override_parity.py.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_scene_serial.h>

#include <stdio.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define PREFAB_PATH "test_prefab_override_src.prefab.json"

/* A prefab whose root carries TWO components and owns ONE child.  Both facts
 * matter: the root's second component proves the non-overridden rows survive,
 * and the child proves the subtree the editor deliberately does not serialize
 * is rebuilt. */
static const char *k_prefab_json =
"{\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
 "\"scene\":{\"version\":1,\"entities\":["
   "{\"id\":1,\"name\":\"Crate\",\"parentId\":0,\"components\":["
     "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
     "{\"type\":\"MeshRenderer\",\"meshShape\":0,\"visible\":true}"
   "]},"
   "{\"id\":2,\"name\":\"CrateLid\",\"parentId\":1,\"components\":["
     "{\"type\":\"Transform\",\"posX\":0,\"posY\":1,\"posZ\":0,\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}"
   "]}"
 "]}}";

/* A scene in EXACTLY the shape the editor emits for an instance of it: the
 * Transform overridden to (5,0,0), no MeshRenderer, no children. */
static const char *k_scene_json =
"{\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
 "\"scene\":{\"version\":1,\"entities\":["
   "{\"id\":10,\"name\":\"Crate\",\"parentId\":0,"
     "\"prefabInstance\":true,\"prefabPath\":\"" PREFAB_PATH "\","
     "\"overrides\":[\"Transform\"],"
     "\"components\":[{\"type\":\"Transform\",\"posX\":5,\"posY\":0,\"posZ\":0,\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}"
 "]}}";

static void write_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    TEST_ASSERT_NOT_NULL_MESSAGE(f, "cannot create the prefab fixture");
    fwrite(text, 1, strlen(text), f);
    fclose(f);
}

static JceEntity find_named(JceScene *s, const char *name);

static void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s;
    struct { const char *want; JceEntity found; } *c = ud;
    const char *n = jce_scene_entity_registered_name(s, e);
    if (n && strcmp(n, c->want) == 0 && c->found == 0) c->found = e;
}

static JceEntity find_named(JceScene *s, const char *name)
{
    struct { const char *want; JceEntity found; } c = { name, 0 };
    jce_scene_each_entity(s, collect_cb, &c);
    return c.found;
}

/* ── The regression. ─────────────────────────────────────────────────── */
static void test_engine_rebuilds_the_instance_from_its_source(void)
{
    write_file(PREFAB_PATH, k_prefab_json);

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE_MESSAGE(
        jce_scene_serial_load(s, k_scene_json, strlen(k_scene_json)),
        "the scene did not load at all");

    JceEntity root = find_named(s, "Crate");
    TEST_ASSERT_TRUE_MESSAGE(root != 0, "the instance root is missing");

    /* 1. The OVERRIDDEN row wins. */
    JceTransform *t = jce_scene_get_transform(s, root);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "no Transform on the instance root");
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 5.0f, t->position.x,
        "the override did not win -- the source's Transform overwrote it");

    /* 2. The NON-overridden row comes from the source.  This is the assertion
     * that was false in a shipped build: the scene node carries no
     * MeshRenderer, so a loader that trusts `components` produces an entity
     * with none. */
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_has_mesh_renderer(s, root),
        "the instance has no MeshRenderer -- the engine built the entity from "
        "the override list alone and never instantiated the prefab source, so "
        "a shipped level renders nothing where the prefab was placed");

    /* 3. The CHILD comes from the source too.  The editor deliberately writes
     * an empty children array for an instance root. */
    JceEntity lid = find_named(s, "CrateLid");
    TEST_ASSERT_TRUE_MESSAGE(lid != 0,
        "the prefab's child was not rebuilt -- every descendant of every "
        "placed prefab is absent from the shipped game");
    TEST_ASSERT_EQUAL_UINT64_MESSAGE(root, jce_scene_get_parent(s, lid),
        "the rebuilt child is not parented to the instance root");

    jce_scene_destroy(s);
    remove(PREFAB_PATH);
}

/* ── A legacy full-snapshot node must still load byte-identically. ───── */
static void test_legacy_snapshot_node_is_untouched(void)
{
    /* Same instance markers, but NO "overrides" key -- every scene authored
     * before the editor's override path existed looks like this.  It must take
     * the unchanged full-snapshot route. */
    static const char *legacy =
    "{\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
     "\"scene\":{\"version\":1,\"entities\":["
       "{\"id\":10,\"name\":\"OldCrate\",\"parentId\":0,"
         "\"prefabInstance\":true,\"prefabPath\":\"does_not_exist.prefab.json\","
         "\"components\":[{\"type\":\"Transform\",\"posX\":7,\"posY\":0,\"posZ\":0,\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}"
     "]}}";

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE(jce_scene_serial_load(s, legacy, strlen(legacy)));

    JceEntity e = find_named(s, "OldCrate");
    TEST_ASSERT_TRUE_MESSAGE(e != 0, "the legacy node did not load");
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FLOAT_WITHIN_MESSAGE(0.001f, 7.0f, t->position.x,
        "a legacy full-snapshot node changed meaning");
    jce_scene_destroy(s);
}

/* ── A missing source must not lose the level. ───────────────────────── */
static void test_missing_source_keeps_what_the_node_carries(void)
{
    static const char *orphan =
    "{\"contract\":{\"name\":\"jce.scene\",\"major\":1,\"minor\":0},"
     "\"scene\":{\"version\":1,\"entities\":["
       "{\"id\":10,\"name\":\"Orphan\",\"parentId\":0,"
         "\"prefabInstance\":true,\"prefabPath\":\"missing.prefab.json\","
         "\"overrides\":[\"Transform\"],"
         "\"components\":[{\"type\":\"Transform\",\"posX\":9,\"posY\":0,\"posZ\":0,\"rotX\":0,\"rotY\":0,\"rotZ\":0,\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1}]}"
     "]}}";

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_TRUE_MESSAGE(jce_scene_serial_load(s, orphan, strlen(orphan)),
        "a prefab instance whose source is gone took the whole scene down");

    JceEntity e = find_named(s, "Orphan");
    TEST_ASSERT_TRUE_MESSAGE(e != 0,
        "the entity vanished because its prefab source could not be found -- "
        "a moved asset must degrade to what the node carries, not delete it");
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_FLOAT_WITHIN(0.001f, 9.0f, t->position.x);
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_engine_rebuilds_the_instance_from_its_source);
    RUN_TEST(test_legacy_snapshot_node_is_untouched);
    RUN_TEST(test_missing_source_keeps_what_the_node_carries);
    return UNITY_END();
}
