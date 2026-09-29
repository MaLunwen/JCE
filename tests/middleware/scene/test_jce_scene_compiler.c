/* Deterministic SceneRecipe compiler and FrozenPlan tests. */

#include <jce/api_scene.h>

#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static JceSceneCatalog make_catalog(void)
{
    JceSceneCatalog catalog;

    jce_scene_catalog_init(&catalog);
    catalog.content_hash = 0xa55aa55af00df00dULL;
    catalog.entry_count = 4u;

    strcpy(catalog.entries[0].asset_id, "kit.tree.oak_a");
    strcpy(catalog.entries[0].capability, "nature.tree");
    catalog.entries[0].content_hash = 11u;
    catalog.entries[0].weight = 2u;
    catalog.entries[0].enabled = true;

    strcpy(catalog.entries[1].asset_id, "kit.tree.oak_b");
    strcpy(catalog.entries[1].capability, "nature.tree");
    catalog.entries[1].content_hash = 12u;
    catalog.entries[1].weight = 1u;
    catalog.entries[1].enabled = true;

    strcpy(catalog.entries[2].asset_id, "kit.light.sun");
    strcpy(catalog.entries[2].capability, "light.directional");
    catalog.entries[2].content_hash = 13u;
    catalog.entries[2].weight = 1u;
    catalog.entries[2].enabled = true;

    strcpy(catalog.entries[3].asset_id, "kit.decor.stone");
    strcpy(catalog.entries[3].capability, "nature.decor");
    catalog.entries[3].content_hash = 14u;
    catalog.entries[3].weight = 1u;
    catalog.entries[3].enabled = true;
    return catalog;
}

static JceSceneRecipe make_recipe(void)
{
    JceSceneRecipe recipe;
    JceSceneRecipeRole *trees;
    JceSceneRecipeRole *sun;

    jce_scene_recipe_init(&recipe);
    recipe.request_id = 77u;
    recipe.seed = 123456u;
    recipe.role_count = 2u;

    trees = &recipe.roles[0];
    strcpy(trees->stable_role, "backdrop_tree");
    strcpy(trees->capability, "nature.tree");
    trees->min_count = 6u;
    trees->max_count = 6u;
    trees->placement = JCE_SCENE_PLACEMENT_SCATTER;
    trees->required = true;
    trees->center[0] = 2.0;
    trees->center[1] = 0.0;
    trees->center[2] = -1.0;
    trees->extent[0] = 10.0;
    trees->extent[1] = 0.0;
    trees->extent[2] = 8.0;
    trees->min_separation = 1.25;
    trees->yaw_min_degrees = -180.0;
    trees->yaw_max_degrees = 180.0;
    trees->scale_min = 0.8;
    trees->scale_max = 1.2;
    strcpy(trees->parent_role, "primary_sun");
    trees->parent_instance = 0u;

    sun = &recipe.roles[1];
    strcpy(sun->stable_role, "primary_sun");
    strcpy(sun->capability, "light.directional");
    sun->min_count = 1u;
    sun->max_count = 1u;
    sun->placement = JCE_SCENE_PLACEMENT_SINGLE;
    sun->required = true;
    sun->center[0] = 0.0;
    sun->center[1] = 10.0;
    sun->center[2] = 0.0;
    sun->scale_min = 1.0;
    sun->scale_max = 1.0;
    return recipe;
}

static void assert_plans_equal(const JceSceneFrozenPlan *a,
                               const JceSceneFrozenPlan *b)
{
    uint32_t i;

    TEST_ASSERT_EQUAL_UINT64(a->plan_hash, b->plan_hash);
    TEST_ASSERT_EQUAL_UINT32(a->operation_count, b->operation_count);
    for (i = 0u; i < a->operation_count; ++i) {
        TEST_ASSERT_EQUAL_UINT64(a->operations[i].stable_entity_id,
                                 b->operations[i].stable_entity_id);
        TEST_ASSERT_EQUAL_UINT64(a->operations[i].parent_stable_entity_id,
                                 b->operations[i].parent_stable_entity_id);
        TEST_ASSERT_EQUAL_STRING(a->operations[i].stable_role,
                                 b->operations[i].stable_role);
        TEST_ASSERT_EQUAL_STRING(a->operations[i].asset_id,
                                 b->operations[i].asset_id);
        TEST_ASSERT_EQUAL_MEMORY(a->operations[i].position_mm,
                                 b->operations[i].position_mm,
                                 sizeof(a->operations[i].position_mm));
        TEST_ASSERT_EQUAL_MEMORY(a->operations[i].rotation_mdeg,
                                 b->operations[i].rotation_mdeg,
                                 sizeof(a->operations[i].rotation_mdeg));
        TEST_ASSERT_EQUAL_MEMORY(a->operations[i].scale_milli,
                                 b->operations[i].scale_milli,
                                 sizeof(a->operations[i].scale_milli));
    }
}

static const JceScenePlanOperation *find_operation(
    const JceSceneFrozenPlan *plan, uint64_t stable_entity_id)
{
    uint32_t i;

    for (i = 0u; i < plan->operation_count; ++i) {
        if (plan->operations[i].stable_entity_id == stable_entity_id)
            return &plan->operations[i];
    }
    return NULL;
}

static void assert_operation_payload_equal(
    const JceScenePlanOperation *a, const JceScenePlanOperation *b)
{
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_NOT_NULL(b);
    TEST_ASSERT_EQUAL_UINT64(a->stable_entity_id, b->stable_entity_id);
    TEST_ASSERT_EQUAL_UINT64(a->parent_stable_entity_id,
                             b->parent_stable_entity_id);
    TEST_ASSERT_EQUAL_UINT64(a->asset_content_hash, b->asset_content_hash);
    TEST_ASSERT_EQUAL_STRING(a->stable_role, b->stable_role);
    TEST_ASSERT_EQUAL_STRING(a->asset_id, b->asset_id);
    TEST_ASSERT_EQUAL_MEMORY(a->position_mm, b->position_mm,
                             sizeof(a->position_mm));
    TEST_ASSERT_EQUAL_MEMORY(a->rotation_mdeg, b->rotation_mdeg,
                             sizeof(a->rotation_mdeg));
    TEST_ASSERT_EQUAL_MEMORY(a->scale_milli, b->scale_milli,
                             sizeof(a->scale_milli));
}

static void test_identical_inputs_compile_identically(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan a;
    JceSceneFrozenPlan b;
    JceSceneCompileError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL, &a, &error));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL, &b, &error));
    TEST_ASSERT_EQUAL_UINT32(7u, a.operation_count);
    assert_plans_equal(&a, &b);
}

static void test_reference_vector_has_pinned_plan_hash(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan plan;
    JceSceneCompileError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));
    TEST_ASSERT_EQUAL_HEX64(0xac31256b90e2ee23ULL, plan.plan_hash);
}

static void test_seed_and_catalog_hash_are_identity_inputs(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan base;
    JceSceneFrozenPlan changed;
    JceSceneCompileError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &base, &error));
    recipe.seed++;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &changed, &error));
    TEST_ASSERT_NOT_EQUAL(base.plan_hash, changed.plan_hash);

    recipe.seed--;
    catalog.content_hash++;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &changed, &error));
    TEST_ASSERT_NOT_EQUAL(base.plan_hash, changed.plan_hash);
}

static void test_request_id_is_provenance_not_layout_entropy(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan base;
    JceSceneFrozenPlan traced;
    JceSceneCompileError error;
    uint32_t i;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &base, &error));
    recipe.request_id += 1000u;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &traced, &error));
    TEST_ASSERT_NOT_EQUAL(base.plan_hash, traced.plan_hash);
    TEST_ASSERT_EQUAL_UINT32(base.operation_count, traced.operation_count);
    for (i = 0u; i < base.operation_count; ++i) {
        assert_operation_payload_equal(
            &base.operations[i],
            find_operation(&traced,
                           base.operations[i].stable_entity_id));
    }
}

static void test_unrelated_role_does_not_perturb_existing_role_streams(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan base;
    JceSceneFrozenPlan expanded;
    JceSceneCompileError error;
    JceSceneRecipeRole *decor;
    uint32_t i;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &base, &error));
    decor = &recipe.roles[recipe.role_count++];
    strcpy(decor->stable_role, "aaa_decor");
    strcpy(decor->capability, "nature.decor");
    decor->min_count = 3u;
    decor->max_count = 3u;
    decor->placement = JCE_SCENE_PLACEMENT_RING;
    decor->required = true;
    decor->extent[0] = 3.0;
    decor->extent[2] = 3.0;
    decor->yaw_min_degrees = -90.0;
    decor->yaw_max_degrees = 90.0;
    decor->scale_min = 0.75;
    decor->scale_max = 1.25;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &expanded, &error));
    for (i = 0u; i < base.operation_count; ++i) {
        assert_operation_payload_equal(
            &base.operations[i],
            find_operation(&expanded,
                           base.operations[i].stable_entity_id));
    }
}

static void test_unknown_capability_fails_before_emitting_operations(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan plan;
    JceSceneCompileError error;

    strcpy(recipe.roles[0].capability, "nature.impossible");
    memset(&plan, 0xa5, sizeof(plan));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_UNKNOWN_CAPABILITY,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));
    TEST_ASSERT_EQUAL_UINT32(0u, error.role_index);
    TEST_ASSERT_EQUAL_UINT32(0u, plan.operation_count);
}

static int64_t square_i64(int32_t v)
{
    return (int64_t)v * (int64_t)v;
}

static void test_scatter_respects_bounds_and_separation(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan plan;
    JceSceneCompileError error;
    uint32_t i;
    uint32_t j;
    const int32_t min_x = -8000;
    const int32_t max_x = 12000;
    const int32_t min_z = -9000;
    const int32_t max_z = 7000;
    const int64_t min_distance_sq = 1250LL * 1250LL;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));

    for (i = 0u; i < plan.operation_count; ++i) {
        const JceScenePlanOperation *a = &plan.operations[i];
        if (strcmp(a->stable_role, "backdrop_tree") != 0)
            continue;
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(min_x, a->position_mm[0]);
        TEST_ASSERT_LESS_OR_EQUAL_INT32(max_x, a->position_mm[0]);
        TEST_ASSERT_GREATER_OR_EQUAL_INT32(min_z, a->position_mm[2]);
        TEST_ASSERT_LESS_OR_EQUAL_INT32(max_z, a->position_mm[2]);
        for (j = i + 1u; j < plan.operation_count; ++j) {
            const JceScenePlanOperation *b = &plan.operations[j];
            int32_t dx;
            int32_t dz;
            int64_t d2;
            if (strcmp(b->stable_role, "backdrop_tree") != 0)
                continue;
            dx = a->position_mm[0] - b->position_mm[0];
            dz = a->position_mm[2] - b->position_mm[2];
            d2 = square_i64(dx) + square_i64(dz);
            TEST_ASSERT_TRUE(d2 >= min_distance_sq);
        }
    }
}

static void test_frozen_binary_round_trip_preserves_hash(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan original;
    JceSceneFrozenPlan decoded;
    JceSceneCompileError error;
    uint8_t bytes[JCE_SCENE_FROZEN_PLAN_MAX_BYTES];
    size_t written = 0u;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &original, &error));
    TEST_ASSERT_TRUE(jce_scene_frozen_plan_write(&original, bytes,
                                                 sizeof(bytes), &written));
    TEST_ASSERT_GREATER_THAN_UINT32(0u, (uint32_t)written);
    TEST_ASSERT_TRUE(jce_scene_frozen_plan_read(bytes, written, &decoded));
    assert_plans_equal(&original, &decoded);

    bytes[written - 1u] ^= 0x01u;
    TEST_ASSERT_FALSE(jce_scene_frozen_plan_read(bytes, written, &decoded));
}

static void test_parent_references_compile_to_stable_entity_ids(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan plan;
    JceSceneCompileError error;
    const JceScenePlanOperation *sun = NULL;
    uint32_t i;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));
    for (i = 0u; i < plan.operation_count; ++i) {
        if (strcmp(plan.operations[i].stable_role, "primary_sun") == 0)
            sun = &plan.operations[i];
    }
    TEST_ASSERT_NOT_NULL(sun);
    TEST_ASSERT_EQUAL_UINT64(0u, sun->parent_stable_entity_id);
    for (i = 0u; i < plan.operation_count; ++i) {
        if (strcmp(plan.operations[i].stable_role, "backdrop_tree") == 0)
            TEST_ASSERT_EQUAL_UINT64(
                sun->stable_entity_id,
                plan.operations[i].parent_stable_entity_id);
    }
}

static void test_topological_order_is_parent_first_and_deterministic(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan plan;
    JceSceneCompileError error;
    uint32_t first[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
    uint32_t second[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
    uint64_t emitted[JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS];
    uint32_t count;
    uint32_t i;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));
    count = jce_scene_frozen_plan_topological_order(
        &plan, first, JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS);
    TEST_ASSERT_EQUAL_UINT32(plan.operation_count, count);
    TEST_ASSERT_EQUAL_UINT32(
        count, jce_scene_frozen_plan_topological_order(
                   &plan, second, JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS));
    TEST_ASSERT_EQUAL_MEMORY(first, second, count * sizeof(first[0]));

    for (i = 0u; i < count; ++i) {
        const JceScenePlanOperation *operation = &plan.operations[first[i]];
        uint32_t parent_cursor;

        if (operation->parent_stable_entity_id != 0u) {
            bool found = false;
            for (parent_cursor = 0u; parent_cursor < i; ++parent_cursor) {
                if (emitted[parent_cursor] ==
                    operation->parent_stable_entity_id) {
                    found = true;
                    break;
                }
            }
            TEST_ASSERT_TRUE(found);
        }
        emitted[i] = operation->stable_entity_id;
    }
}

static void test_invalid_graph_cannot_be_hashed_or_serialized(void)
{
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe();
    JceSceneFrozenPlan plan;
    JceSceneCompileError error;
    uint8_t bytes[JCE_SCENE_FROZEN_PLAN_MAX_BYTES];
    size_t written = 0u;
    uint32_t root = UINT32_MAX;
    uint32_t child = UINT32_MAX;
    uint32_t i;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));
    for (i = 0u; i < plan.operation_count; ++i) {
        if (plan.operations[i].parent_stable_entity_id == 0u)
            root = i;
        else
            child = i;
    }
    TEST_ASSERT_NOT_EQUAL(UINT32_MAX, root);
    TEST_ASSERT_NOT_EQUAL(UINT32_MAX, child);
    plan.operations[root].parent_stable_entity_id =
        plan.operations[child].stable_entity_id;

    TEST_ASSERT_FALSE(jce_scene_frozen_plan_graph_validate(&plan));
    TEST_ASSERT_EQUAL_UINT64(0u, jce_scene_frozen_plan_hash(&plan));
    TEST_ASSERT_FALSE(jce_scene_frozen_plan_write(&plan, bytes,
                                                   sizeof(bytes), &written));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_identical_inputs_compile_identically);
    RUN_TEST(test_reference_vector_has_pinned_plan_hash);
    RUN_TEST(test_seed_and_catalog_hash_are_identity_inputs);
    RUN_TEST(test_request_id_is_provenance_not_layout_entropy);
    RUN_TEST(test_unrelated_role_does_not_perturb_existing_role_streams);
    RUN_TEST(test_unknown_capability_fails_before_emitting_operations);
    RUN_TEST(test_scatter_respects_bounds_and_separation);
    RUN_TEST(test_frozen_binary_round_trip_preserves_hash);
    RUN_TEST(test_parent_references_compile_to_stable_entity_ids);
    RUN_TEST(test_topological_order_is_parent_first_and_deterministic);
    RUN_TEST(test_invalid_graph_cannot_be_hashed_or_serialized);
    return UNITY_END();
}
