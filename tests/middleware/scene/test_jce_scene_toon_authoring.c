/*
 * test_jce_scene_toon_authoring.c
 *
 * Per-character TOON authoring on MeshRenderer (stylized-slice §5.6).  Round-
 * trips the new toon flag + cel/rim/outline knobs field-for-field through the
 * REAL component JSON serializer (jce_scene_save_json -> jce_scene_load_json),
 * and asserts a MeshRenderer authored WITHOUT toon keys reloads with toon=false
 * (legacy scenes byte-identical: zero-default = off).
 *
 * ALSO PINS MeshRenderer.visible, which lives here because this is the suite
 * that round-trips a MeshRenderer through the REAL serializer.  It was the one
 * renderer component whose visibility crossed JSON in NEITHER direction:
 * parse_mesh_renderer hardcoded `mr.visible = true` and read no key, and
 * ser_mesh_renderer emitted none -- while GrassField, FoliageCluster, Water,
 * TerrainChunk, VegetationScatter and BillboardRenderer all round-trip theirs.
 * Two consumers lost silently: the editor could not SAVE a hidden mesh, and any
 * script's comp_get -> modify -> comp_set turned a hidden mesh back on, because
 * the key was absent from what comp_get handed it.
 */

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_json.h>

#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

typedef struct { const char *want; JceEntity found; } FindCtx;

static void find_cb(JceScene *s, JceEntity e, void *ud)
{
    FindCtx *ctx = (FindCtx *)ud;
    const char *nm = jce_scene_entity_registered_name(s, e);
    if (nm && strcmp(nm, ctx->want) == 0) ctx->found = e;
}

static JceEntity find_by_name(JceScene *s, const char *name)
{
    FindCtx ctx = { name, JCE_ENTITY_INVALID };
    jce_scene_each_entity(s, find_cb, &ctx);
    return ctx.found;
}

static void test_toon_round_trip(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    TEST_ASSERT_NOT_NULL(src);
    TEST_ASSERT_NOT_NULL(dst);

    JceEntity e = jce_scene_create_entity(src, "Hero");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    mr.mesh_path = "models/UAL1_Standard.glb";   /* interned on set */
    mr.base_color[0] = mr.base_color[1] = mr.base_color[2] = mr.base_color[3] = 1.0f;
    mr.roughness = 1.0f;
    mr.toon           = true;
    mr.toon_bands     = 3;
    mr.rim_power      = 4.0f;
    mr.rim_intensity  = 0.8f;
    mr.rim_color[0]   = 1.0f; mr.rim_color[1] = 0.9f; mr.rim_color[2] = 0.7f;
    mr.outline_width  = 0.02f;
    mr.outline_color[0] = 0.05f; mr.outline_color[1] = 0.04f; mr.outline_color[2] = 0.03f;
    jce_scene_set_mesh_renderer(src, e, &mr);

    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Hero");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    TEST_ASSERT_TRUE(jce_scene_has_mesh_renderer(dst, ne));
    JceMeshRenderer *out = jce_scene_get_mesh_renderer(dst, ne);
    TEST_ASSERT_NOT_NULL(out);

    TEST_ASSERT_TRUE(out->toon);
    TEST_ASSERT_EQUAL_INT(3, out->toon_bands);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 4.0f, out->rim_power);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.8f, out->rim_intensity);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 1.0f, out->rim_color[0]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.9f, out->rim_color[1]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.7f, out->rim_color[2]);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.02f, out->outline_width);
    TEST_ASSERT_FLOAT_WITHIN(1e-4f, 0.05f, out->outline_color[0]);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

static void test_no_toon_keys_default_off(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    JceEntity e = jce_scene_create_entity(src, "Plain");
    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible = true;
    mr.mesh_path = "models/box.glb";   /* interned on set */
    jce_scene_set_mesh_renderer(src, e, &mr);

    JceJson *root = jce_scene_save_json(src);
    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Plain");
    JceMeshRenderer *out = jce_scene_get_mesh_renderer(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE(out->toon);

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* Find the first MeshRenderer component object anywhere under `node`.  The
 * scene envelope's shape is not this test's business -- it walks for a "type"
 * of "MeshRenderer" rather than hardcoding a path that a later envelope change
 * would silently turn into NULL. */
static JceJson *find_mesh_renderer_json(JceJson *node)
{
    if (!node) return NULL;
    if (jce_json_is_object(node)) {
        const char *ty = jce_json_get_string(node, "type", NULL);
        if (ty && strcmp(ty, "MeshRenderer") == 0) return node;
    }
    if (jce_json_is_object(node) || jce_json_is_array(node)) {
        JceJson *c = jce_json_first_child(node);
        for (; c; c = jce_json_next_sibling(c)) {
            JceJson *hit = find_mesh_renderer_json(c);
            if (hit) return hit;
        }
    }
    return NULL;
}

/* A HIDDEN MESH MUST STILL BE HIDDEN AFTER A SAVE AND A LOAD.
 *
 * Before this was fixed the assertion below read `visible == true` and there
 * was no test to notice: the serializer wrote no key and the parser answered
 * `true` unconditionally, so every hidden MeshRenderer came back on.  In the
 * editor that is a Save that discards what the user did; through a script's
 * comp_get/comp_set it is a mesh that re-appears because the round trip was
 * never given the field it was asked to preserve. */
static void test_a_hidden_mesh_renderer_survives_save_and_load(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    JceEntity e   = jce_scene_create_entity(src, "Hidden");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible   = false;                 /* the whole point */
    mr.mesh_path = "models/box.glb";
    jce_scene_set_mesh_renderer(src, e, &mr);

    /* THE KEY MUST BE IN THE DOCUMENT, asserted separately from the reload.
     * A parser that defaulted to false would make the reload pass with the
     * serializer still writing nothing -- silence comparing equal to
     * silence. */
    JceJson *root = jce_scene_save_json(src);
    TEST_ASSERT_NOT_NULL(root);
    JceJson *mrj = find_mesh_renderer_json(root);
    TEST_ASSERT_NOT_NULL_MESSAGE(mrj, "no MeshRenderer was serialized at all");
    TEST_ASSERT_TRUE_MESSAGE(jce_json_has(mrj, "visible"),
        "ser_mesh_renderer emitted no \"visible\" key -- the value cannot "
        "survive a save, and a script's comp_get cannot preserve what it is "
        "never given");
    TEST_ASSERT_FALSE(jce_json_get_bool(mrj, "visible", true));

    int loaded = jce_scene_load_json(dst, root);
    TEST_ASSERT_EQUAL_INT(1, loaded);
    jce_json_free(root);

    JceEntity ne = find_by_name(dst, "Hidden");
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, ne);
    JceMeshRenderer *out = jce_scene_get_mesh_renderer(dst, ne);
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_FALSE_MESSAGE(out->visible,
        "a MeshRenderer saved hidden reloaded VISIBLE -- parse_mesh_renderer "
        "is not reading the key");

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

/* AND A DOCUMENT WITH NO KEY STILL MEANS VISIBLE.
 *
 * Every scene, prefab and cooked asset in this repository predates the key --
 * git grep finds no "visible" on a MeshRenderer in any tracked .scene.json --
 * so the default is what keeps them rendering.  Built by REMOVING the key
 * from a real saved document rather than by hand-writing one, so the rest of
 * the component stays exactly what the serializer produces. */
static void test_a_mesh_renderer_with_no_visible_key_is_visible(void)
{
    JceScene *src = jce_scene_create();
    JceScene *dst = jce_scene_create();
    JceEntity e   = jce_scene_create_entity(src, "Legacy");

    JceMeshRenderer mr;
    jce_mesh_renderer_init(&mr);
    mr.visible   = true;
    mr.mesh_path = "models/box.glb";
    jce_scene_set_mesh_renderer(src, e, &mr);

    JceJson *root = jce_scene_save_json(src);
    JceJson *mrj  = find_mesh_renderer_json(root);
    TEST_ASSERT_NOT_NULL(mrj);
    jce_json_remove(mrj, "visible");
    TEST_ASSERT_FALSE_MESSAGE(jce_json_has(mrj, "visible"),
        "the key was not actually removed -- this case would then be testing "
        "the same thing as the one above");

    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(dst, root));
    jce_json_free(root);

    JceMeshRenderer *out = jce_scene_get_mesh_renderer(dst,
                                                       find_by_name(dst, "Legacy"));
    TEST_ASSERT_NOT_NULL(out);
    TEST_ASSERT_TRUE_MESSAGE(out->visible,
        "a legacy MeshRenderer with no \"visible\" key loaded HIDDEN -- every "
        "scene in the repository would go blank");

    jce_scene_destroy(src);
    jce_scene_destroy(dst);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_toon_round_trip);
    RUN_TEST(test_no_toon_keys_default_off);
    RUN_TEST(test_a_hidden_mesh_renderer_survives_save_and_load);
    RUN_TEST(test_a_mesh_renderer_with_no_visible_key_is_visible);
    return UNITY_END();
}
