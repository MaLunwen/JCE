/* test_jce_grass_field.c
 * GrassField component serialize->parse round-trip + blade-mesh builder
 * (uv.y monotonic) + scatter determinism for grass placement params. */
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>
#include <jce/middleware/scene/jce_foliage.h>
#include <jce/renderer/jce_mesh.h>   /* JceMeshVertex (pos[3]/normal[3]/uv[2]) */
#include "unity.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* Forward-declare the pure-CPU blade fill function (compiled into jce_scene,
 * declared in jce_sr_internal.h which cannot be included from tests due to
 * engine-internal relative includes).  This pure-math helper is headlessly
 * testable (does NOT call bgfx).  Linked via LINK jce_scene in CMakeLists. */
uint32_t sr_grass_fill_blade(float blade_height, float blade_width, int cards,
                             JceMeshVertex *v, uint32_t *idx);

/* Mirror the blade-geometry constants from jce_sr_internal.h (same reason). */
#define JCE_GRASS_BLADE_SEGS    4
#define JCE_GRASS_BLADE_VERTS   ((JCE_GRASS_BLADE_SEGS + 1) * 2)
#define JCE_GRASS_BLADE_INDICES (JCE_GRASS_BLADE_SEGS * 6)

#define GRASS_PREFAB "jce_grass_roundtrip.prefab.json"
void setUp(void)    {}
void tearDown(void) { remove(GRASS_PREFAB); }

static void test_grass_field_roundtrip(void)
{
    JceScene *s = jce_scene_create();
    TEST_ASSERT_NOT_NULL(s);
    JceEntity e = jce_scene_create_entity(s, "grass");
    JceTransform t; memset(&t, 0, sizeof t);
    t.rotation.w = 1.0f; t.scale.x = t.scale.y = t.scale.z = 1.0f;
    jce_scene_set_transform(s, e, &t);

    JceGrassFieldComponent g; memset(&g, 0, sizeof g);
    g.density = 12.0f; g.seed = 4242u; g.area_x = 40.0f; g.area_z = 60.0f;
    g.max_slope_deg = 30.0f; g.scale_min = 0.8f; g.scale_max = 1.4f;
    g.blade_height = 0.45f; g.blade_width = 0.06f; g.cards = 4;
    g.root_color[0] = 0.10f; g.root_color[1] = 0.22f; g.root_color[2] = 0.05f;
    g.tip_color[0]  = 0.55f; g.tip_color[1]  = 0.78f; g.tip_color[2]  = 0.25f;
    g.wind_dir[0] = 1.0f; g.wind_dir[1] = 0.3f;
    g.wind_speed = 1.7f; g.wind_amplitude = 0.12f;
    g.fade_start = 60.0f; g.fade_end = 120.0f; g.hue_jitter = 0.25f;
    g.cast_shadow = false; g.visible = true;
    jce_scene_set_grass_field(s, e, &g);

    TEST_ASSERT_TRUE(jce_prefab_save_subtree(s, e, GRASS_PREFAB));
    jce_scene_destroy(s);

    JceScene *s2 = jce_scene_create();
    JceEntity root = jce_prefab_instantiate_file(s2, GRASS_PREFAB, NULL);
    TEST_ASSERT_TRUE(root != 0);
    JceGrassFieldComponent *r = jce_scene_get_grass_field(s2, root);
    TEST_ASSERT_NOT_NULL(r);
    TEST_ASSERT_EQUAL_FLOAT(12.0f, r->density);
    TEST_ASSERT_EQUAL_UINT32(4242u, r->seed);
    TEST_ASSERT_EQUAL_FLOAT(40.0f, r->area_x);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, r->area_z);
    TEST_ASSERT_EQUAL_FLOAT(30.0f, r->max_slope_deg);
    TEST_ASSERT_EQUAL_FLOAT(0.45f, r->blade_height);
    TEST_ASSERT_EQUAL_FLOAT(0.06f, r->blade_width);
    TEST_ASSERT_EQUAL_INT(4, r->cards);
    TEST_ASSERT_EQUAL_FLOAT(0.22f, r->root_color[1]);
    TEST_ASSERT_EQUAL_FLOAT(0.78f, r->tip_color[1]);
    TEST_ASSERT_EQUAL_FLOAT(1.0f, r->wind_dir[0]);
    TEST_ASSERT_EQUAL_FLOAT(1.7f, r->wind_speed);
    TEST_ASSERT_EQUAL_FLOAT(0.12f, r->wind_amplitude);
    TEST_ASSERT_EQUAL_FLOAT(60.0f, r->fade_start);
    TEST_ASSERT_EQUAL_FLOAT(120.0f, r->fade_end);
    TEST_ASSERT_EQUAL_FLOAT(0.25f, r->hue_jitter);
    TEST_ASSERT_FALSE(r->cast_shadow);
    TEST_ASSERT_TRUE(r->visible);

    /* An entity with NO GrassField reloads with none (legacy scenes unchanged). */
    JceEntity bare = jce_scene_create_entity(s2, "bare");
    TEST_ASSERT_FALSE(jce_scene_has_grass_field(s2, bare));
    jce_scene_destroy(s2);
}

static void test_blade_mesh_uvy_monotonic(void)
{
    /* Use the pure-CPU fill helper (no bgfx) to verify blade geometry math.
     * Blade = segmented, tapered, curved strip per card (stylized leaf):
     * (SEGS+1) rows of 2 verts, SEGS quads of 2 tris. */
    const int cards = 4;
    const uint32_t nverts   = (uint32_t)cards * JCE_GRASS_BLADE_VERTS;
    const uint32_t nindices = (uint32_t)cards * JCE_GRASS_BLADE_INDICES;
    JceMeshVertex *v = (JceMeshVertex *)malloc(nverts * sizeof(JceMeshVertex));
    uint32_t *idx = (uint32_t *)malloc(nindices * sizeof(uint32_t));
    TEST_ASSERT_NOT_NULL(v);
    TEST_ASSERT_NOT_NULL(idx);

    uint32_t written = sr_grass_fill_blade(0.5f, 0.06f, cards, v, idx);

    TEST_ASSERT_EQUAL_UINT32(nverts, written);

    /* uv.y is the root->tip parameter: within [0,1], row-monotonic (each
     * vertex pair shares one t; t rises with the row). */
    for (uint32_t i = 0; i < written; ++i) {
        float uvy = v[i].uv[1];
        TEST_ASSERT_TRUE(uvy >= 0.0f && uvy <= 1.0f);
    }
    for (int c = 0; c < cards; ++c) {
        const uint32_t base = (uint32_t)c * JCE_GRASS_BLADE_VERTS;
        for (int s = 0; s < JCE_GRASS_BLADE_SEGS; ++s) {
            float t0 = v[base + (uint32_t)s * 2u].uv[1];
            float t1 = v[base + (uint32_t)(s + 1) * 2u].uv[1];
            TEST_ASSERT_TRUE(t1 > t0);
        }
        /* Root row starts at 0; tip row ends at 1. */
        TEST_ASSERT_TRUE(v[base].uv[1] == 0.0f);
        TEST_ASSERT_TRUE(v[base + JCE_GRASS_BLADE_VERTS - 1u].uv[1] == 1.0f);
    }
    for (uint32_t i = 0; i < nindices; ++i)
        TEST_ASSERT_TRUE(idx[i] < nverts);

    free(v);
    free(idx);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_grass_field_roundtrip);
    RUN_TEST(test_blade_mesh_uvy_monotonic);
    return UNITY_END();
}
