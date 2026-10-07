#include "unity.h"
#include <jce/api_scene.h>
#include "middleware/scene/jce_sr_nodraw.h"
#include "middleware/scene/jce_component_registry_internal.h"
#include <stdlib.h>

void setUp(void) {}
void tearDown(void) {}

static void test_component_membership_changes_invalidate_structural_cache(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "component");
    JceRigidBodyComponent body = {0};
    uint64_t generation = jce_scene_get_structural_epoch(scene);
    jce_scene_set_rigidbody(scene, entity, &body);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > generation);
    generation = jce_scene_get_structural_epoch(scene);
    jce_scene_set_rigidbody(scene, entity, &body);
    TEST_ASSERT_EQUAL_UINT64(generation, jce_scene_get_structural_epoch(scene));
    jce_scene_remove_rigidbody(scene, entity);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > generation);
    generation = jce_scene_get_structural_epoch(scene);
    jce_scene_remove_rigidbody(scene, entity);
    TEST_ASSERT_EQUAL_UINT64(generation, jce_scene_get_structural_epoch(scene));
    jce_scene_destroy(scene);
}

static void test_world_component_membership_and_motion_have_distinct_epochs(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "world");
    JceTransform transform = *jce_scene_get_transform(scene, entity);
    uint64_t generation = jce_scene_get_structural_epoch(scene);
    transform.position.x = 5;
    jce_scene_set_transform(scene, entity, &transform);
    TEST_ASSERT_EQUAL_UINT64(generation, jce_scene_get_structural_epoch(scene));
    jce_scene_remove_transform(scene, entity);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > generation);
    generation = jce_scene_get_structural_epoch(scene);
    jce_scene_set_transform(scene, entity, &transform);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > generation);
    JcePivotComponent pivot = {0};
    generation = jce_scene_get_structural_epoch(scene);
    jce_scene_set_pivot(scene, entity, &pivot);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > generation);
    jce_scene_destroy(scene);
}

static void test_mesh_membership_and_field_edits_invalidate_correct_keys(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "mesh");
    JceMeshRenderer mesh = {0};
    uint64_t structure = jce_scene_get_structural_epoch(scene);
    jce_scene_set_mesh_renderer(scene, entity, &mesh);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > structure);
    structure = jce_scene_get_structural_epoch(scene);
    uint64_t fields = jce_scene_get_cull_data_gen(scene);
    mesh.shadow_cast_off = true;
    jce_scene_set_mesh_renderer(scene, entity, &mesh);
    TEST_ASSERT_EQUAL_UINT64(structure, jce_scene_get_structural_epoch(scene));
    TEST_ASSERT_TRUE(jce_scene_get_cull_data_gen(scene) > fields);
    jce_scene_remove_mesh_renderer(scene, entity);
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > structure);
    jce_scene_destroy(scene);
}

static void test_no_draw_presence_admission_survives_specialized_mutations(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "presence");
    TEST_ASSERT_TRUE(sr_entity_has_no_draw(scene, entity));
    TEST_ASSERT_FALSE(sr_nodraw_flags(JCE_COMP_FLAG_TRANSFORM, true));
    TEST_ASSERT_FALSE(sr_nodraw_flags(JCE_COMP_FLAG_MESH_RENDERER, false));
    const char *types[] = {"MeshRenderer", "Terrain", "Tilemap", "VegetationScatter",
        "LineRenderer", "TrailRenderer", "Water", "FoliageCluster", "GrassField",
        "SpriteRenderer", "BillboardRenderer", "VideoPlayer", "SkeletalAnimator",
        "SpriteAnimator", "LODGroup"};
    for (unsigned i = 0; i < sizeof(types) / sizeof(types[0]); ++i) {
        const int id = jce_component_find(types[i]);
        TEST_ASSERT_TRUE(id >= 0);
        const JceComponentDesc *desc = jce_component_desc(id);
        TEST_ASSERT_NOT_NULL(desc);
        TEST_ASSERT_TRUE(desc->struct_size > 0 && desc->set && desc->remove);
        void *value = calloc(1, desc->struct_size);
        TEST_ASSERT_NOT_NULL(value);
        const uint64_t epoch = jce_scene_get_structural_epoch(scene);
        TEST_ASSERT_TRUE(jce_scene_set_comp(scene, entity, id, value));
        TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > epoch);
        TEST_ASSERT_FALSE(sr_entity_has_no_draw(scene, entity));
        jce_scene_set_comp_enabled(scene, entity, id, false);
        TEST_ASSERT_FALSE(sr_entity_has_no_draw(scene, entity));
        const uint64_t added = jce_scene_get_structural_epoch(scene);
        jce_scene_remove_comp(scene, entity, id);
        TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > added);
        TEST_ASSERT_TRUE(sr_entity_has_no_draw(scene, entity));
        free(value);
    }
    jce_scene_destroy(scene);
}

static bool late_custom_presence(const JceScene *scene, JceEntity entity)
{
    return jce_scene_has_transform(scene, entity);
}

static void late_custom_remove(JceScene *scene, JceEntity entity)
{
    (void)scene;
    (void)entity;
}

static void test_late_registry_append_invalidates_no_draw_admission(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity entity = jce_scene_create_entity(scene, "late");
    int snapshot = jce_component_count();
    TEST_ASSERT_TRUE(sr_entity_has_no_draw(scene, entity));
    TEST_ASSERT_TRUE(sr_nodraw_cache_current(snapshot, snapshot));
    JceComponentDesc desc = {0};
    desc.name = "LateCustomRenderPresence";
    desc.has = late_custom_presence;
    desc.remove = late_custom_remove;
    TEST_ASSERT_TRUE(jce_component_register(&desc) >= 0);
    TEST_ASSERT_FALSE(sr_nodraw_cache_current(snapshot, jce_component_count()));
    TEST_ASSERT_FALSE(sr_entity_has_no_draw(scene, entity));
    const uint64_t before = jce_scene_get_structural_epoch(scene);
    TEST_ASSERT_TRUE(sr_nodraw_refresh_registry(scene, &snapshot, jce_component_count()));
    TEST_ASSERT_TRUE(jce_scene_get_structural_epoch(scene) > before);
    TEST_ASSERT_TRUE(sr_nodraw_cache_current(snapshot, jce_component_count()));
    TEST_ASSERT_FALSE(sr_entity_has_no_draw(scene, entity));
    const uint64_t refreshed = jce_scene_get_structural_epoch(scene);
    TEST_ASSERT_FALSE(sr_nodraw_refresh_registry(scene, &snapshot, jce_component_count()));
    TEST_ASSERT_FALSE(sr_nodraw_refresh_registry(NULL, &snapshot, snapshot + 1));
    TEST_ASSERT_EQUAL_UINT64(refreshed, jce_scene_get_structural_epoch(scene));
    jce_scene_destroy(scene);
}

static void test_dispatch_registry_admission_is_monotonic(void)
{
    bool valid = sr_nodraw_dispatch_current(true, 0, 0);
    TEST_ASSERT_TRUE(valid);
    /* An earlier OTHER dispatch may register before the next cached NONE. */
    valid = sr_nodraw_dispatch_current(valid, 0, 1);
    TEST_ASSERT_FALSE(valid);
    TEST_ASSERT_FALSE(sr_nodraw_dispatch_current(valid, 1, 1));
    TEST_ASSERT_FALSE(sr_nodraw_dispatch_current(true, -1, -1));
    TEST_ASSERT_TRUE(sr_nodraw_dispatch_current(true, 1, 1));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_no_draw_presence_admission_survives_specialized_mutations);
    RUN_TEST(test_component_membership_changes_invalidate_structural_cache);
    RUN_TEST(test_world_component_membership_and_motion_have_distinct_epochs);
    RUN_TEST(test_mesh_membership_and_field_edits_invalidate_correct_keys);
    RUN_TEST(test_late_registry_append_invalidates_no_draw_admission);
    RUN_TEST(test_dispatch_registry_admission_is_monotonic);
    return UNITY_END();
}
