/* test_jce_prefab_tree_format.c
 *
 * Characterization tests for REF-001 (dedup audit): the engine prefab loader
 * must accept BOTH on-disk .prefab.json shapes.
 *
 * Two writers exist for one asset class:
 *   - FLAT  — jce_prefab_save_subtree() -> {contract, scene:{version, entities:[...]}}
 *   - TREE  — the editor's Save-as-Prefab  -> {contract, prefab:{version, root:{...children...}}}
 *
 * Before REF-001 the engine's resolve_entities() accepted only scene.entities /
 * root.entities, so jce_prefab_instantiate_file() on an editor-authored prefab
 * returned SILENTLY EMPTY — no entities, no error.  Nothing caught it because
 * every existing prefab test round-trips the flat writer against the flat
 * reader.  These tests pin both shapes so the incompatibility cannot return.
 */

#include <jce/middleware/scene/jce_prefab.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/resource/jce_scene_contract.h>

#include "unity.h"

#include <stdio.h>
#include <string.h>

#define TREE_PREFAB "jce_prefab_tree_form.prefab.json"
#define FLAT_PREFAB "jce_prefab_flat_form.prefab.json"

/* The exact envelope the editor's build_prefab_json_root() emits: a
 * {contract, prefab:{version, root}} document whose nodes nest through
 * "children" and carry their components in a "components" array. */
static const char *const kTreePrefabJson =
"{\n"
"  \"" JCE_SCENE_CONTRACT_KEY "\": {\n"
"    \"" JCE_SCENE_CONTRACT_NAME_KEY "\": \"" JCE_SCENE_CONTRACT_NAME "\",\n"
"    \"" JCE_SCENE_CONTRACT_MAJOR_KEY "\": 1,\n"
"    \"" JCE_SCENE_CONTRACT_MINOR_KEY "\": 0\n"
"  },\n"
"  \"prefab\": {\n"
"    \"" JCE_SCENE_VERSION_KEY "\": 1,\n"
"    \"root\": {\n"
"      \"name\": \"Enemy\",\n"
"      \"enabled\": true,\n"
"      \"tagColor\": 0,\n"
"      \"components\": [\n"
"        { \"type\": \"Transform\",\n"
"          \"posX\": 1.0, \"posY\": 2.0, \"posZ\": 3.0,\n"
"          \"rotX\": 0.0, \"rotY\": 0.0, \"rotZ\": 0.0,\n"
"          \"scaleX\": 1.0, \"scaleY\": 1.0, \"scaleZ\": 1.0 }\n"
"      ],\n"
"      \"children\": [\n"
"        {\n"
"          \"name\": \"Weapon\",\n"
"          \"enabled\": true,\n"
"          \"tagColor\": 0,\n"
"          \"components\": [\n"
"            { \"type\": \"Transform\",\n"
"              \"posX\": 4.0, \"posY\": 5.0, \"posZ\": 6.0,\n"
"              \"rotX\": 0.0, \"rotY\": 0.0, \"rotZ\": 0.0,\n"
"              \"scaleX\": 2.0, \"scaleY\": 2.0, \"scaleZ\": 2.0 }\n"
"          ],\n"
"          \"children\": []\n"
"        }\n"
"      ]\n"
"    }\n"
"  }\n"
"}\n";

static bool write_text_file(const char *path, const char *text)
{
    FILE *f = fopen(path, "wb");
    if (!f) return false;
    size_t n = strlen(text);
    bool ok = fwrite(text, 1, n, f) == n;
    fclose(f);
    return ok;
}

/* Collect every entity in the scene so we can locate children by parent. */
typedef struct {
    JceEntity ents[64];
    int       count;
} EntityList;

static void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    EntityList *l = (EntityList *)ud;
    (void)s;
    if (l->count < (int)(sizeof l->ents / sizeof l->ents[0]))
        l->ents[l->count++] = e;
}

static JceEntity find_child_of(JceScene *s, JceEntity parent)
{
    EntityList l;
    memset(&l, 0, sizeof l);
    jce_scene_each_entity(s, collect_cb, &l);
    for (int i = 0; i < l.count; ++i) {
        if (l.ents[i] != parent && jce_scene_get_parent(s, l.ents[i]) == parent)
            return l.ents[i];
    }
    return 0;
}

void setUp(void) {}
void tearDown(void)
{
    remove(TREE_PREFAB);
    remove(FLAT_PREFAB);
}

/* ── The regression this whole file exists for ────────────────────────
 * An editor-authored (tree-shaped) prefab must instantiate its entities.
 * Pre-REF-001 this produced 0 entities and returned a falsy root. */
static void test_tree_form_instantiates_entities(void)
{
    TEST_ASSERT_TRUE(write_text_file(TREE_PREFAB, kTreePrefabJson));

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity root = jce_prefab_instantiate_file(s, TREE_PREFAB, NULL);
    TEST_ASSERT_TRUE_MESSAGE(root != 0,
        "editor tree-form prefab instantiated nothing (REF-001 regression)");

    EntityList l;
    memset(&l, 0, sizeof l);
    jce_scene_each_entity(s, collect_cb, &l);
    TEST_ASSERT_EQUAL_INT_MESSAGE(2, l.count,
        "tree prefab must create exactly root + 1 child");

    jce_scene_destroy(s);
}

/* Components must be applied through the SAME registry path as the flat
 * form — not a second parser that silently drops fields. */
static void test_tree_form_applies_components(void)
{
    TEST_ASSERT_TRUE(write_text_file(TREE_PREFAB, kTreePrefabJson));

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity root = jce_prefab_instantiate_file(s, TREE_PREFAB, NULL);
    TEST_ASSERT_TRUE(root != 0);

    JceTransform *t = jce_scene_get_transform(s, root);
    TEST_ASSERT_NOT_NULL_MESSAGE(t, "root node lost its Transform component");
    TEST_ASSERT_EQUAL_FLOAT(1.0f, t->position.x);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, t->position.y);
    TEST_ASSERT_EQUAL_FLOAT(3.0f, t->position.z);

    jce_scene_destroy(s);
}

/* Nesting is structural: the child must actually be parented to the root,
 * because tree nodes carry no parentId to fall back on. */
static void test_tree_form_establishes_parenting(void)
{
    TEST_ASSERT_TRUE(write_text_file(TREE_PREFAB, kTreePrefabJson));

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity root = jce_prefab_instantiate_file(s, TREE_PREFAB, NULL);
    TEST_ASSERT_TRUE(root != 0);

    JceEntity child = find_child_of(s, root);
    TEST_ASSERT_TRUE_MESSAGE(child != 0,
        "child node was not parented to the prefab root");

    JceTransform *ct = jce_scene_get_transform(s, child);
    TEST_ASSERT_NOT_NULL(ct);
    TEST_ASSERT_EQUAL_FLOAT(4.0f, ct->position.x);
    TEST_ASSERT_EQUAL_FLOAT(5.0f, ct->position.y);
    TEST_ASSERT_EQUAL_FLOAT(6.0f, ct->position.z);
    TEST_ASSERT_EQUAL_FLOAT(2.0f, ct->scale.x);

    jce_scene_destroy(s);
}

/* Backwards compatibility: the pre-existing flat writer/reader pair must
 * keep working byte-for-byte as before. */
static void test_flat_form_still_roundtrips(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);

    JceEntity e = jce_scene_create_entity(s, "FlatRoot");
    TEST_ASSERT_TRUE(e != 0);
    JceTransform *t = jce_scene_get_transform(s, e);
    TEST_ASSERT_NOT_NULL(t);
    t->position = jce_v3(7.0f, 8.0f, 9.0f);

    TEST_ASSERT_TRUE(jce_prefab_save_subtree(s, e, FLAT_PREFAB));
    jce_scene_destroy(s);

    JceScene *s2 = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s2);
    JceEntity root = jce_prefab_instantiate_file(s2, FLAT_PREFAB, NULL);
    TEST_ASSERT_TRUE_MESSAGE(root != 0, "flat-form prefab regressed");

    JceTransform *rt = jce_scene_get_transform(s2, root);
    TEST_ASSERT_NOT_NULL(rt);
    TEST_ASSERT_EQUAL_FLOAT(7.0f, rt->position.x);
    TEST_ASSERT_EQUAL_FLOAT(8.0f, rt->position.y);
    TEST_ASSERT_EQUAL_FLOAT(9.0f, rt->position.z);

    jce_scene_destroy(s2);
}

/* A document that is neither shape must fail cleanly, not crash. */
static void test_unrecognised_document_fails_cleanly(void)
{
    TEST_ASSERT_TRUE(write_text_file(TREE_PREFAB,
        "{ \"contract\": { \"name\": \"jce.scene\", \"major\": 1, \"minor\": 0 },"
        "  \"somethingElse\": { \"nope\": true } }\n"));

    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity root = jce_prefab_instantiate_file(s, TREE_PREFAB, NULL);
    TEST_ASSERT_TRUE_MESSAGE(root == 0,
        "an unrecognised prefab document must not instantiate anything");
    jce_scene_destroy(s);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_tree_form_instantiates_entities);
    RUN_TEST(test_tree_form_applies_components);
    RUN_TEST(test_tree_form_establishes_parenting);
    RUN_TEST(test_flat_form_still_roundtrips);
    RUN_TEST(test_unrecognised_document_fails_cleanly);
    return UNITY_END();
}
