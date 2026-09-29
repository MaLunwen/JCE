/*
 * test_jce_material_override_mask.c — a per-entity tint survives a load.
 *
 * JceMeshRenderer carries a full inline PBR block, the renderer reads it, the
 * inspector exposes all six factors as live widgets, and the serialiser writes
 * every one of them.  Then BOTH loaders threw them away for any entity naming
 * a .mat.json: the editor's repair_paths_cb with no guard at all, and the
 * runtime's parse_mesh_renderer whenever the material resolved.
 *
 * So: assign Rock.mat.json to 200 rocks, tint three of them red, see red in
 * the viewport, save, reopen -- grey again.  No warning, and no diff to
 * explain it: the scene file still says baseColorR=1, it is simply overwritten
 * in memory at load.
 *
 * material_override_mask records which factors the AUTHOR set on this
 * renderer.  Zero -- what every scene written before this carries -- means the
 * material still wins outright.
 *
 * IN THE TREE: tests/ is tracked on this branch (gitignored on `main`).
 * Settle it with `git check-ignore -v <path>`, never from memory --
 * tools/lint/check_provenance_claims.py has what that cost.
 */
#include "unity.h"

#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_material_override.h>
/* jce_scene_serialize_entity_components lives HERE, not in jce_scene.h.
 * Without this include C assumes an int return and the 64-bit JceJson*
 * comes back truncated and sign-extended -- FFFFFFFF867C9DB0 -- which
 * segfaults on the first dereference.  MSVC says only C4013 at the
 * default warning level, which is the same silence
 * check_shipped_main_declarations.py exists for. */
#include <jce/middleware/scene/jce_scene_components_json.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>

#include <stdio.h>
#include <string.h>

#define MAT_DIR  "jce_ut_matovr"
#define MAT_PATH MAT_DIR "/rock.mat.json"

/* A material that is emphatically NOT the tint the entity authors. */
static const char *const MAT_JSON =
"{\"$schema\":\"jce.material.v1\",\"properties\":{"
 "\"baseColorFactor\":[0.5,0.5,0.5,1.0],"
 "\"metallicFactor\":0.25,\"roughnessFactor\":0.75,"
 "\"emissiveFactor\":[0.0,0.0,0.0],"
 "\"normalScale\":1.0,\"aoStrength\":1.0}}";

static char s_scene_json[2048];

/* One entity: a red tint, pointing at the grey material.  `mask` says which
 * factors it claims as its own. */
static const char *scene_with_mask(unsigned mask)
{
    snprintf(s_scene_json, sizeof s_scene_json,
        "{\"format_version\":1,\"entities\":["
         "{\"name\":\"Rock\",\"id\":1,\"parent_id\":0,\"components\":["
           "{\"type\":\"Transform\",\"posX\":0,\"posY\":0,\"posZ\":0,"
            "\"rotX\":0,\"rotY\":0,\"rotZ\":0,"
            "\"scaleX\":1,\"scaleY\":1,\"scaleZ\":1},"
           "{\"type\":\"MeshRenderer\",\"materialPath\":\"%s\","
            "\"baseColorR\":1.0,\"baseColorG\":0.0,\"baseColorB\":0.0,"
            "\"baseColorA\":1.0,\"metallic\":0.9,"
            "\"materialOverrides\":%u}]}]}",
        MAT_PATH, mask);
    return s_scene_json;
}

static JceScene *load(unsigned mask)
{
    const char *j = scene_with_mask(mask);
    JceJson *root = jce_json_parse(j, strlen(j));
    TEST_ASSERT_NOT_NULL(root);
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    TEST_ASSERT_EQUAL_INT(1, jce_scene_load_json(s, root));
    jce_json_free(root);
    return s;
}

typedef struct { JceEntity e; } Found;

static void first_cb(JceScene *s, JceEntity e, void *ud)
{
    (void)s;
    Found *f = (Found *)ud;
    if (f->e == 0) f->e = e;
}

static JceMeshRenderer *only_renderer(JceScene *s)
{
    Found f = { 0 };
    jce_scene_each_entity(s, first_cb, &f);
    TEST_ASSERT_TRUE(f.e != 0);
    return jce_scene_get_mesh_renderer(s, f.e);
}

void setUp(void) {}
void tearDown(void) {}

static void test_without_the_mask_the_material_still_wins(void)
{
    /* THE OLD BEHAVIOUR, PINNED.  Every scene written before this field
     * existed carries mask 0, and must load byte-identically: the material's
     * grey overwrites the entity's red.  If this ever changes, the feature has
     * silently repainted every existing project. */
    JceScene *s = load(0u);
    JceMeshRenderer *mr = only_renderer(s);
    TEST_ASSERT_NOT_NULL(mr);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.5f, mr->base_color[0],
        "mask 0 means the material is the source of truth, as before");
    TEST_ASSERT_EQUAL_FLOAT(0.25f, mr->metallic);
    jce_scene_destroy(s);
}

static void test_an_overridden_factor_survives(void)
{
    /* THE WHOLE FEATURE.  baseColor is claimed; metallic is not. */
    JceScene *s = load(JCE_MR_OVERRIDE_BASE_COLOR);
    JceMeshRenderer *mr = only_renderer(s);
    TEST_ASSERT_NOT_NULL(mr);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, mr->base_color[0],
        "the authored red must survive the load -- this is the tint that used "
        "to vanish with no warning");
    TEST_ASSERT_EQUAL_FLOAT(0.0f, mr->base_color[1]);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.25f, mr->metallic,
        "and a factor NOT claimed must still track the material, or the "
        "override would be all-or-nothing and the shared material pointless");
    jce_scene_destroy(s);
}

static void test_each_bit_is_independent(void)
{
    JceScene *s = load(JCE_MR_OVERRIDE_METALLIC);
    JceMeshRenderer *mr = only_renderer(s);
    TEST_ASSERT_NOT_NULL(mr);
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.9f, mr->metallic,
        "metallic is claimed and must survive");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.5f, mr->base_color[0],
        "...while baseColor, unclaimed, still comes from the material");
    jce_scene_destroy(s);
}

static void test_the_mask_round_trips(void)
{
    /* An override the file cannot carry is an override that dies on save. */
    JceScene *s = load(JCE_MR_OVERRIDE_BASE_COLOR | JCE_MR_OVERRIDE_METALLIC);
    Found f = { 0 };
    jce_scene_each_entity(s, first_cb, &f);
    JceJson *comps = jce_scene_serialize_entity_components(s, f.e);
    TEST_ASSERT_NOT_NULL(comps);
    bool seen = false;
    for (JceJson *c = jce_json_first_child(comps); c;
         c = jce_json_next_sibling(c)) {
        const char *t = jce_json_get_string(c, "type", "");
        if (t && strcmp(t, "MeshRenderer") == 0) {
            const double m = jce_json_number_value(
                jce_json_get(c, "materialOverrides"), -1.0);
            TEST_ASSERT_EQUAL_INT_MESSAGE(
                (int)(JCE_MR_OVERRIDE_BASE_COLOR | JCE_MR_OVERRIDE_METALLIC),
                (int)m, "the mask must serialise, or it dies on the next save");
            seen = true;
        }
    }
    TEST_ASSERT_TRUE_MESSAGE(seen, "the MeshRenderer must serialise at all");
    jce_json_free(comps);
    jce_scene_destroy(s);
}

static void test_the_helper_skips_exactly_what_is_claimed(void)
{
    /* THE SHARED RULE, DIRECT.  Three call sites reach this function -- the
     * runtime loader, the editor's path-repair pass and the inspector's
     * material load/reload -- and only the first is reachable from a unit
     * test, so the rule is asserted here rather than through one caller. */
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    mr.base_color[0] = 1.0f; mr.base_color[1] = 0.0f;
    mr.base_color[2] = 0.0f; mr.base_color[3] = 1.0f;
    mr.metallic = 0.9f;
    mr.roughness = 0.1f;
    mr.material_override_mask = JCE_MR_OVERRIDE_BASE_COLOR;

    JcePbrMaterial mat = jce_pbr_material_default();
    mat.base_color_factor[0] = 0.5f;
    mat.metallic_factor  = 0.25f;
    mat.roughness_factor = 0.75f;

    jce_mesh_renderer_apply_material_pbr(&mr, &mat);

    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(1.0f, mr.base_color[0],
        "the claimed factor must be left alone");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.25f, mr.metallic,
        "an unclaimed factor must take the material's value");
    TEST_ASSERT_EQUAL_FLOAT_MESSAGE(0.75f, mr.roughness,
        "...every unclaimed factor, not just the first");

    /* Neither pointer may be walked when NULL: the editor calls this on a
     * component it has just looked up, and a lookup can fail. */
    jce_mesh_renderer_apply_material_pbr(NULL, &mat);
    jce_mesh_renderer_apply_material_pbr(&mr, NULL);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, mr.base_color[0]);
}

static void test_the_override_count_is_the_number_the_panel_shows(void)
{
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof mr);
    TEST_ASSERT_EQUAL_INT(0, jce_mesh_renderer_override_count(&mr));
    mr.material_override_mask = JCE_MR_OVERRIDE_BASE_COLOR |
                                JCE_MR_OVERRIDE_AO_STRENGTH;
    TEST_ASSERT_EQUAL_INT(2, jce_mesh_renderer_override_count(&mr));
    mr.material_override_mask = JCE_MR_OVERRIDE_ALL;
    TEST_ASSERT_EQUAL_INT_MESSAGE(6, jce_mesh_renderer_override_count(&mr),
        "JCE_MR_OVERRIDE_ALL must name every bit that exists, or a renderer "
        "that claims everything still silently takes something");
    TEST_ASSERT_EQUAL_INT(0, jce_mesh_renderer_override_count(NULL));
}

int main(void)
{
    (void)jce_fs_host_create_directory(MAT_DIR);
    FILE *f = fopen(MAT_PATH, "wb");
    if (!f) { fprintf(stderr, "cannot write the material fixture\n"); return 2; }
    fwrite(MAT_JSON, 1, strlen(MAT_JSON), f);
    fclose(f);

    UNITY_BEGIN();
    RUN_TEST(test_without_the_mask_the_material_still_wins);
    RUN_TEST(test_an_overridden_factor_survives);
    RUN_TEST(test_each_bit_is_independent);
    RUN_TEST(test_the_mask_round_trips);
    RUN_TEST(test_the_helper_skips_exactly_what_is_claimed);
    RUN_TEST(test_the_override_count_is_the_number_the_panel_shows);
    const int rc = UNITY_END();
    remove(MAT_PATH);
    return rc;
}
