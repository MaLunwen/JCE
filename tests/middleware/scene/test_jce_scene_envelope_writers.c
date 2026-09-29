/*
 * test_jce_scene_envelope_writers.c — the shared entity-envelope writers
 * (audit entity-envelope-dual-writers).
 *
 * "disabledComponents" and "layer" used to be emitted by THREE writers with
 * hand-copied logic: the engine's flat scene writer, the engine's EditorMeta
 * writer, and the editor's tree writer (prefabs, clipboard, Play snapshots).
 * The editor copy carried a comment asserting it "matches the engine's
 * main-scene writer" — a claim maintained by hand, which is precisely how two
 * copies drift while everyone believes they agree.
 *
 * The risk in de-duplicating them is the opposite of the risk in leaving
 * them: a helper that emits even slightly differently silently CHANGES the
 * on-disk scene format.  So these tests are about exact output, including the
 * cases where the correct output is nothing at all.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_component_registry.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <stdbool.h>
#include <string.h>

static JceScene *g_s;

void setUp(void)    { g_s = jce_scene_create(); }
void tearDown(void) { if (g_s) { jce_scene_destroy(g_s); g_s = NULL; } }

/* Default entity: nothing disabled, layer 0.  BOTH writers must emit
 * absolutely nothing — an empty array or a "layer": 0 would change every
 * existing scene file the first time it is re-saved. */
static void test_default_entity_emits_no_envelope_keys(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "plain");
    JceJson *o = jce_json_object();
    TEST_ASSERT_NOT_NULL(o);

    jce_scene_write_disabled_components(o, g_s, e);
    jce_scene_write_entity_layer(o, g_s, e);

    TEST_ASSERT_FALSE_MESSAGE(jce_json_has(o, "disabledComponents"),
        "an entity with nothing disabled emitted the key anyway — every "
        "existing scene file would change on its next save");
    TEST_ASSERT_FALSE_MESSAGE(jce_json_has(o, "layer"),
        "layer 0 was emitted — same problem");

    jce_json_free(o);
}

static void test_disabled_component_is_emitted_by_canonical_name(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "withmesh");

    const int comp = jce_component_find("MeshRenderer");
    TEST_ASSERT_NOT_EQUAL_INT(JCE_COMP_ID_INVALID, comp);

    /* Give the entity the component, then disable it. */
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    jce_scene_set_mesh_renderer(g_s, e, &mr);
    jce_scene_set_comp_enabled(g_s, e, comp, false);
    TEST_ASSERT_FALSE(jce_scene_comp_enabled(g_s, e, comp));

    JceJson *o = jce_json_object();
    jce_scene_write_disabled_components(o, g_s, e);

    JceJson *arr = jce_json_get(o, "disabledComponents");
    TEST_ASSERT_NOT_NULL_MESSAGE(arr, "disabled component was not emitted");
    TEST_ASSERT_TRUE(jce_json_is_array(arr));

    /* Canonical NAME, not the legacy 64-bit flag: that space is exhausted, and
       components past it silently came back ENABLED through every round trip. */
    bool found = false;
    const int n = jce_json_array_size(arr);
    for (int i = 0; i < n; ++i) {
        const char *v = jce_json_string_value(jce_json_array_at(arr, i), "");
        if (v && strcmp(v, "MeshRenderer") == 0) { found = true; break; }
    }
    TEST_ASSERT_TRUE_MESSAGE(found,
        "disabledComponents did not contain the canonical component name");

    jce_json_free(o);
}

/* Re-enabling must take the key back OUT, not leave an empty array behind. */
static void test_reenabling_removes_the_key_entirely(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "toggle");
    const int comp = jce_component_find("MeshRenderer");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    jce_scene_set_mesh_renderer(g_s, e, &mr);

    jce_scene_set_comp_enabled(g_s, e, comp, false);
    JceJson *a = jce_json_object();
    jce_scene_write_disabled_components(a, g_s, e);
    TEST_ASSERT_TRUE(jce_json_has(a, "disabledComponents"));
    jce_json_free(a);

    jce_scene_set_comp_enabled(g_s, e, comp, true);
    JceJson *b = jce_json_object();
    jce_scene_write_disabled_components(b, g_s, e);
    TEST_ASSERT_FALSE_MESSAGE(jce_json_has(b, "disabledComponents"),
        "re-enabling left the key behind (probably as an empty array)");
    jce_json_free(b);
}

static void test_non_default_layer_is_emitted(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "layered");
    jce_scene_set_entity_layer(g_s, e, 3);

    JceJson *o = jce_json_object();
    jce_scene_write_entity_layer(o, g_s, e);

    TEST_ASSERT_TRUE_MESSAGE(jce_json_has(o, "layer"),
        "a non-default layer was dropped — it would silently reset to 0 on "
        "every prefab save, clipboard copy and Play snapshot");
    TEST_ASSERT_EQUAL_INT(3, jce_json_get_int(o, "layer", -1));
    jce_json_free(o);
}

/* Degenerate arguments must be no-ops, not crashes: both helpers are called
   from two different serialisers with independently-derived arguments. */
static void test_writers_tolerate_null_arguments(void)
{
    JceEntity e = jce_scene_create_entity(g_s, "x");
    JceJson *o = jce_json_object();

    jce_scene_write_disabled_components(NULL, g_s, e);
    jce_scene_write_entity_layer(NULL, g_s, e);
    jce_scene_write_disabled_components(o, NULL, e);
    jce_scene_write_entity_layer(o, NULL, e);

    TEST_ASSERT_FALSE(jce_json_has(o, "disabledComponents"));
    TEST_ASSERT_FALSE(jce_json_has(o, "layer"));
    jce_json_free(o);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_entity_emits_no_envelope_keys);
    RUN_TEST(test_disabled_component_is_emitted_by_canonical_name);
    RUN_TEST(test_reenabling_removes_the_key_entirely);
    RUN_TEST(test_non_default_layer_is_emitted);
    RUN_TEST(test_writers_tolerate_null_arguments);
    return UNITY_END();
}
