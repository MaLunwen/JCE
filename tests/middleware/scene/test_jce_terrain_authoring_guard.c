/*
 * test_jce_terrain_authoring_guard.c
 *
 * The authoring entry points must REFUSE a terrain with no resident grid.
 *
 * jce_terrain.h states the contract in one line -- "authoring (sculpt /
 * import / export) requires a monolithic terrain" -- and import, export,
 * erosion, thermal and the sky-occlusion bake all enforced it. The brush and
 * save entry points did not, and the shape of the miss is worth stating
 * because it is not the obvious one: a tiled or procedural terrain sets `w`
 * and `h` to the FULL extent, so every "is this grid big enough" check passes.
 * Only `heights`/`splat` are NULL. A guard written as `w < 2` -- which is what
 * apply_erosion opens with -- therefore does not protect anything here.
 *
 * The editor reaches these: a `"procedural": true` meta loads through the Load
 * button (jce_terrain.c: terrain_from_meta_json -> create_procedural), and the
 * next brush stroke used to dereference NULL. capture_snapshot already
 * returned false for tiled, so the undo push silently no-opped first and the
 * crash was not even preceded by a snapshot.
 *
 * These tests fail by CRASHING rather than by asserting, which is the only
 * honest way to test a null dereference: if a guard is removed, the process
 * dies here instead of in somebody's editor.
 */

#include <jce/middleware/scene/jce_terrain.h>
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_prefab.h>

#include <stdlib.h>
#include <string.h>

#include "unity.h"

void setUp(void)    {}
void tearDown(void) {}

#define TW    65
#define TH    65
#define WORLD 128.0f
#define MAXH  40.0f
#define MATERIAL_PREFAB "jce_terrain_material_roundtrip.prefab.json"

/* A terrain with no resident height/splat grid. Procedural is the one an
 * editor user can actually reach; tiled needs a load callback. */
static JceTerrain *make_gridless(void)
{
    JceTerrain *t = jce_terrain_create_procedural(TW, TH, WORLD, WORLD,
                                                  MAXH, 8, 4, 1u, 0.05f);
    if (!t) return NULL;
    if (jce_terrain_heights(t)) {   /* resident after all: not our case */
        jce_terrain_free(t);
        return NULL;
    }
    return t;
}

static void test_sculpt_refuses_a_gridless_terrain(void)
{
    JceTerrain *t = make_gridless();
    if (!t) { TEST_IGNORE_MESSAGE("no gridless terrain available"); return; }

    /* Every mode, because the switch is inside the loop that dereferences. */
    jce_terrain_sculpt_apply(t, JCE_TERRAIN_SCULPT_RAISE,
                             WORLD * 0.5f, WORLD * 0.5f, 16.0f, 1.0f, 0.1f);
    jce_terrain_sculpt_apply(t, JCE_TERRAIN_SCULPT_LOWER,
                             WORLD * 0.5f, WORLD * 0.5f, 16.0f, 1.0f, 0.1f);
    jce_terrain_sculpt_apply(t, JCE_TERRAIN_SCULPT_SMOOTH,
                             WORLD * 0.5f, WORLD * 0.5f, 16.0f, 1.0f, 0.1f);
    jce_terrain_sculpt_apply(t, JCE_TERRAIN_SCULPT_FLATTEN,
                             WORLD * 0.5f, WORLD * 0.5f, 16.0f, 1.0f, 0.1f);

    JceTerrainBrushDesc brush = jce_terrain_brush_desc_default();
    brush.hardness = 1.0f;
    jce_terrain_sculpt_apply_brush(t, JCE_TERRAIN_SCULPT_RAISE,
                                   &brush, WORLD * 0.5f, WORLD * 0.5f,
                                   16.0f, 1.0f, 0.1f);

    /* Still gridless, still alive. */
    TEST_ASSERT_NULL(jce_terrain_heights(t));
    jce_terrain_free(t);
}

static void test_splat_paint_refuses_a_gridless_terrain(void)
{
    JceTerrain *t = make_gridless();
    if (!t) { TEST_IGNORE_MESSAGE("no gridless terrain available"); return; }

    for (int layer = 0; layer < 4; ++layer)
        jce_terrain_splat_paint(t, layer, WORLD * 0.5f, WORLD * 0.5f,
                                16.0f, 1.0f, 0.1f);

    JceTerrainBrushDesc brush = jce_terrain_brush_desc_default();
    jce_terrain_splat_paint_brush(t, 1, &brush,
                                  WORLD * 0.5f, WORLD * 0.5f,
                                  16.0f, 1.0f, 0.1f);

    TEST_ASSERT_NULL(jce_terrain_splat(t));
    jce_terrain_free(t);
}

static JceTerrain *make_small_resident(void)
{
    return jce_terrain_create(9, 9, 8.0f, 8.0f, 40.0f, 4);
}

static void test_custom_mask_rotation_changes_the_stamp_direction(void)
{
    const float mask[2] = { 0.0f, 1.0f };
    JceTerrainBrushDesc brush = jce_terrain_brush_desc_default();
    brush.mask = mask;
    brush.mask_width = 2;
    brush.mask_height = 1;
    brush.hardness = 1.0f;

    JceTerrain *t = make_small_resident();
    TEST_ASSERT_NOT_NULL(t);
    jce_terrain_sculpt_apply_brush(t, JCE_TERRAIN_SCULPT_RAISE,
                                   &brush, 4.0f, 4.0f,
                                   3.0f, 1.0f, 1.0f);
    const float *h = jce_terrain_heights(t);
    float left = h[4 * 9 + 3];
    float right = h[4 * 9 + 5];
    TEST_ASSERT_TRUE_MESSAGE(right > left,
                             "unrotated mask should be stronger on +X");
    jce_terrain_free(t);

    t = make_small_resident();
    TEST_ASSERT_NOT_NULL(t);
    brush.rotation_deg = 180.0f;
    jce_terrain_sculpt_apply_brush(t, JCE_TERRAIN_SCULPT_RAISE,
                                   &brush, 4.0f, 4.0f,
                                   3.0f, 1.0f, 1.0f);
    h = jce_terrain_heights(t);
    left = h[4 * 9 + 3];
    right = h[4 * 9 + 5];
    TEST_ASSERT_TRUE_MESSAGE(left > right,
                             "180-degree rotation should reverse the mask");
    jce_terrain_free(t);
}

static void test_flatten_can_lock_a_stroke_target(void)
{
    JceTerrain *t = make_small_resident();
    TEST_ASSERT_NOT_NULL(t);
    float *h = (float *)jce_terrain_heights(t);
    h[4 * 9 + 4] = 0.75f;

    JceTerrainBrushDesc brush = jce_terrain_brush_desc_default();
    brush.hardness = 1.0f;
    brush.use_flatten_target = true;
    brush.flatten_target_world = 10.0f;
    jce_terrain_sculpt_apply_brush(t, JCE_TERRAIN_SCULPT_FLATTEN,
                                   &brush, 4.0f, 4.0f,
                                   2.0f, 1.0f, 1.0f);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.25f, h[4 * 9 + 4]);
    jce_terrain_free(t);
}

static void test_save_refuses_a_gridless_terrain(void)
{
    JceTerrain *t = make_gridless();
    if (!t) { TEST_IGNORE_MESSAGE("no gridless terrain available"); return; }

    /* A path that would be writable if the call got that far -- the point is
     * that it must refuse BEFORE the memcpy, not that the write fails. */
    TEST_ASSERT_FALSE(jce_terrain_save_file(t, "gridless_guard_probe.terrain.json"));
    jce_terrain_free(t);
}

/* The counter-test: on a terrain that DOES have a grid, the same calls must
 * still work. A guard that refuses everything would pass the three above. */
static void test_a_resident_terrain_is_still_editable(void)
{
    JceTerrain *t = jce_terrain_create(TW, TH, WORLD, WORLD, MAXH, 32);
    TEST_ASSERT_NOT_NULL(t);
    TEST_ASSERT_NOT_NULL(jce_terrain_heights(t));

    const float before = jce_terrain_heights(t)[(TH / 2) * TW + (TW / 2)];
    jce_terrain_sculpt_apply(t, JCE_TERRAIN_SCULPT_RAISE,
                             WORLD * 0.5f, WORLD * 0.5f, 32.0f, 1.0f, 1.0f);
    const float after = jce_terrain_heights(t)[(TH / 2) * TW + (TW / 2)];
    TEST_ASSERT_TRUE_MESSAGE(after > before,
                             "the guard also blocked a terrain that has a grid");

    jce_terrain_splat_paint(t, 1, WORLD * 0.5f, WORLD * 0.5f, 32.0f, 1.0f, 1.0f);
    TEST_ASSERT_NOT_NULL(jce_terrain_splat(t));

    jce_terrain_free(t);
}

static void test_terrain_material_layers_round_trip(void)
{
    JceScene *scene = jce_scene_create();
    TEST_ASSERT_NOT_NULL(scene);
    JceEntity entity = jce_scene_create_entity(scene, "terrain-material");
    JceTerrainComponent terrain;
    memset(&terrain, 0, sizeof terrain);
    strcpy(terrain.terrain_path, "world/test.terrain.json");
    strcpy(terrain.layer_albedo_path[2], "terrain/rock_albedo.png");
    strcpy(terrain.layer_normal_path[2], "terrain/rock_normal.png");
    strcpy(terrain.layer_mask_path[2], "terrain/rock_mask.png");
    terrain.layer_normal_scale[0] = 0.5f;
    terrain.layer_normal_scale[1] = 0.75f;
    terrain.layer_normal_scale[2] = 1.25f;
    terrain.layer_normal_scale[3] = 2.0f;
    terrain.height_blend = 0.6f;
    terrain.tile_scale = 12.0f;
    terrain.tint[0] = terrain.tint[1] = terrain.tint[2] = 1.0f;
    terrain.visible = true;
    terrain.splat_enabled = true;
    jce_scene_set_terrain(scene, entity, &terrain);

    TEST_ASSERT_TRUE(jce_prefab_save_subtree(scene, entity, MATERIAL_PREFAB));
    jce_scene_destroy(scene);

    scene = jce_scene_create();
    entity = jce_prefab_instantiate_file(scene, MATERIAL_PREFAB, NULL);
    TEST_ASSERT_NOT_EQUAL(0, entity);
    JceTerrainComponent *loaded = jce_scene_get_terrain(scene, entity);
    TEST_ASSERT_NOT_NULL(loaded);
    TEST_ASSERT_EQUAL_STRING("terrain/rock_albedo.png",
                             loaded->layer_albedo_path[2]);
    TEST_ASSERT_EQUAL_STRING("terrain/rock_normal.png",
                             loaded->layer_normal_path[2]);
    TEST_ASSERT_EQUAL_STRING("terrain/rock_mask.png",
                             loaded->layer_mask_path[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.25f,
                             loaded->layer_normal_scale[2]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.6f, loaded->height_blend);
    jce_scene_destroy(scene);
    remove(MATERIAL_PREFAB);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sculpt_refuses_a_gridless_terrain);
    RUN_TEST(test_splat_paint_refuses_a_gridless_terrain);
    RUN_TEST(test_save_refuses_a_gridless_terrain);
    RUN_TEST(test_a_resident_terrain_is_still_editable);
    RUN_TEST(test_custom_mask_rotation_changes_the_stamp_direction);
    RUN_TEST(test_flatten_can_lock_a_stroke_target);
    RUN_TEST(test_terrain_material_layers_round_trip);
    return UNITY_END();
}
