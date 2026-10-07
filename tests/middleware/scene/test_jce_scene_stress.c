#include <jce/api_scene.h>
#include "unity.h"
#include <math.h>

void setUp(void) {}
void tearDown(void) {}

static void test_exact_spawned_count_and_owned_subtree(void)
{
    JceScene *scene = jce_scene_create();
    JceEntity authored = jce_scene_create_entity(scene, "authored");
    JceSceneStressDesc desc = {JCE_SCENE_STRESS_DRAWCALL, 10, 0};
    JceSceneStressResult result;
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(scene, &desc, &result));
    TEST_ASSERT_EQUAL_UINT32(10, result.spawned);
    JceEntity children[16];
    TEST_ASSERT_EQUAL_INT(10, jce_scene_get_children(scene, result.root, children, 16));
    const JceMeshRenderer *a = jce_scene_get_mesh_renderer(scene, children[0]);
    const JceMeshRenderer *b = jce_scene_get_mesh_renderer(scene, children[1]);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_TRUE(a->roughness != b->roughness || a->metallic != b->metallic);
    jce_scene_destroy_entity(scene, result.root);
    TEST_ASSERT_FALSE(jce_scene_has_transform(scene, children[0]));
    TEST_ASSERT_NOT_NULL(jce_scene_entity_name(scene, authored));
    jce_scene_destroy(scene);
}

static void test_scatter_rounding_and_oversized_requests(void)
{
    JceScene *scene = jce_scene_create();
    JceSceneStressDesc desc = {JCE_SCENE_STRESS_INSTANCING, 5000, 0};
    JceSceneStressResult result;
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(scene, &desc, &result));
    JceEntity child;
    TEST_ASSERT_EQUAL_INT(1, jce_scene_get_children(scene, result.root, &child, 1));
    const JceVegetationScatterComponent *vs = jce_scene_get_vegetation_scatter(scene, child);
    TEST_ASSERT_NOT_NULL(vs);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1.0f, vs->tint[0]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.05f, vs->tint[1]);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, vs->scale_min);
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 2.0f, vs->scale_max);
    TEST_ASSERT_EQUAL_UINT32(5000, (uint32_t)((double)vs->density * vs->area_x * vs->area_z));
    jce_scene_destroy_entity(scene, result.root);
    desc.count = JCE_FOLIAGE_MAX_INSTANCES + 1;
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(scene, &desc, &result));
    JceEntity chunks[2];
    TEST_ASSERT_EQUAL_INT(2, jce_scene_get_children(scene, result.root, chunks, 2));
    uint32_t actual = 0;
    for (int i = 0; i < 2; ++i) {
        vs = jce_scene_get_vegetation_scatter(scene, chunks[i]);
        TEST_ASSERT_NOT_NULL(vs);
        const uint32_t n = (uint32_t)((double)vs->density * vs->area_x * vs->area_z);
        TEST_ASSERT_TRUE(n > 0 && n <= JCE_FOLIAGE_MAX_INSTANCES);
        TEST_ASSERT_EQUAL_UINT32((uint32_t)i + 1, vs->seed);
        actual += n;
    }
    TEST_ASSERT_EQUAL_UINT32(desc.count, actual);
    TEST_ASSERT_EQUAL_UINT32(desc.count, result.spawned);
    jce_scene_destroy_entity(scene, result.root);
    desc.count = 2000001;
    TEST_ASSERT_FALSE(jce_scene_stress_spawn(scene, &desc, &result));
    TEST_ASSERT_EQUAL_UINT64(JCE_ENTITY_INVALID, result.root);
    TEST_ASSERT_EQUAL_UINT32(0, result.spawned);
    desc.count = 1;
    desc.spread = NAN;
    TEST_ASSERT_FALSE(jce_scene_stress_spawn(scene, &desc, &result));
    jce_scene_destroy(scene);
}

static void test_perfect_cube_grid_has_exact_integer_side(void)
{
    JceScene *scene = jce_scene_create();
    JceSceneStressDesc desc = {JCE_SCENE_STRESS_ENTITY, 27, 0};
    JceSceneStressResult result;
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(scene, &desc, &result));
    JceEntity entities[27];
    TEST_ASSERT_EQUAL_INT(27, jce_scene_get_children(scene, result.root, entities, 27));
    for (int i = 0; i < 27; ++i) {
        const JceTransform *t = jce_scene_get_transform(scene, entities[i]);
        TEST_ASSERT_NOT_NULL(t);
        TEST_ASSERT_TRUE(fabsf(t->position.x) <= 1.5f);
        TEST_ASSERT_TRUE(fabsf(t->position.y) <= 1.5f);
        TEST_ASSERT_TRUE(fabsf(t->position.z) <= 1.5f);
    }
    jce_scene_destroy(scene);
}

static void test_physics_fixture_has_real_gravity_and_colliders(void)
{
    JceScene *scene = jce_scene_create();
    JceSceneStressDesc desc = {JCE_SCENE_STRESS_PHYSICS, 3, 0};
    JceSceneStressResult result;
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(scene, &desc, &result));
    JceEntity children[3];
    TEST_ASSERT_EQUAL_INT(3, jce_scene_get_children(scene, result.root, children, 3));
    const JceRigidBodyComponent *rb = jce_scene_get_rigidbody(scene, children[0]);
    TEST_ASSERT_NOT_NULL(rb);
    TEST_ASSERT_EQUAL_INT(1, rb->body_type);
    TEST_ASSERT_TRUE(rb->use_gravity);
    TEST_ASSERT_EQUAL_FLOAT(1, rb->mass);
    TEST_ASSERT_NOT_NULL(jce_scene_get_box_collider(scene, children[0]));
    jce_scene_destroy(scene);
}

static void test_motion_is_scene_local_and_preserves_unit_rotation(void)
{
    JceScene *a = jce_scene_create(), *b = jce_scene_create();
    JceSceneStressDesc desc = {JCE_SCENE_STRESS_ENTITY, 2, 0};
    JceSceneStressResult ra, rb;
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(a, &desc, &ra));
    TEST_ASSERT_TRUE(jce_scene_stress_spawn(b, &desc, &rb));
    JceEntity ca, cb;
    jce_scene_get_children(a, ra.root, &ca, 1);
    jce_scene_get_children(b, rb.root, &cb, 1);
    const float before = jce_scene_get_transform(b, cb)->position.x;
    jce_scene_stress_motion(a, ra.root, true);
    jce_scene_update(b, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT(before, jce_scene_get_transform(b, cb)->position.x);
    const uint64_t structural = jce_scene_get_structural_epoch(a);
    int i;
    for (i = 0; i < 256; ++i) jce_scene_update(a, 0.016f);
    const JceTransform *t = jce_scene_get_transform(a, ca);
    const float norm = t->rotation.x * t->rotation.x + t->rotation.y * t->rotation.y +
                       t->rotation.z * t->rotation.z + t->rotation.w * t->rotation.w;
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 1, norm);
    TEST_ASSERT_EQUAL_FLOAT(0, jce_scene_get_transform(a, ra.root)->position.x);
    TEST_ASSERT_EQUAL_UINT64(structural, jce_scene_get_structural_epoch(a));
    jce_scene_stress_motion(a, ra.root, false);
    const float stopped = jce_scene_get_transform(a, ca)->position.x;
    jce_scene_update(a, 0.016f);
    TEST_ASSERT_EQUAL_FLOAT(stopped, jce_scene_get_transform(a, ca)->position.x);
    jce_scene_destroy(a);
    jce_scene_destroy(b);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_exact_spawned_count_and_owned_subtree);
    RUN_TEST(test_scatter_rounding_and_oversized_requests);
    RUN_TEST(test_perfect_cube_grid_has_exact_integer_side);
    RUN_TEST(test_physics_fixture_has_real_gravity_and_colliders);
    RUN_TEST(test_motion_is_scene_local_and_preserves_unit_rotation);
    return UNITY_END();
}
