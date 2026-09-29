/* Deterministic SceneRecipe contract tests. */

#include <jce/api_scene.h>

#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

static JceSceneRecipe make_valid_recipe(void)
{
    JceSceneRecipe recipe;
    JceSceneRecipeRole *role;

    jce_scene_recipe_init(&recipe);
    recipe.request_id = 41u;
    recipe.seed = 0x123456789abcdef0ULL;
    recipe.role_count = 1u;

    role = &recipe.roles[0];
    strcpy(role->stable_role, "primary_sun");
    strcpy(role->capability, "light.directional");
    role->min_count = 1u;
    role->max_count = 1u;
    role->placement = JCE_SCENE_PLACEMENT_SINGLE;
    role->required = true;
    role->scale_min = 1.0;
    role->scale_max = 1.0;
    return recipe;
}

static JceSceneRecipe make_valid_hierarchy_recipe(void)
{
    JceSceneRecipe recipe = make_valid_recipe();
    JceSceneRecipeRole *child = &recipe.roles[1];

    recipe.role_count = 2u;
    strcpy(child->stable_role, "weather_source");
    strcpy(child->capability, "weather.rain");
    strcpy(child->parent_role, "primary_sun");
    child->parent_instance = 0u;
    child->min_count = 1u;
    child->max_count = 1u;
    child->placement = JCE_SCENE_PLACEMENT_SINGLE;
    child->required = true;
    child->scale_min = 1.0;
    child->scale_max = 1.0;
    return recipe;
}

static void test_init_sets_supported_versions(void)
{
    JceSceneRecipe recipe;

    memset(&recipe, 0xa5, sizeof(recipe));
    jce_scene_recipe_init(&recipe);

    TEST_ASSERT_EQUAL_UINT32(JCE_SCENE_RECIPE_SCHEMA_VERSION,
                             recipe.schema_version);
    TEST_ASSERT_EQUAL_UINT32(JCE_SCENE_COMPILER_VERSION,
                             recipe.compiler_version);
    TEST_ASSERT_EQUAL_UINT32(0u, recipe.role_count);
}

static void test_validate_rejects_unsupported_schema(void)
{
    JceSceneRecipe recipe = make_valid_recipe();
    JceSceneRecipeError error;

    recipe.schema_version = JCE_SCENE_RECIPE_SCHEMA_VERSION + 1u;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_UNSUPPORTED_VERSION,
                          jce_scene_recipe_validate(&recipe, &error));
    TEST_ASSERT_EQUAL_UINT32(UINT32_MAX, error.item_index);
}

static void test_quantization_is_half_away_from_zero(void)
{
    int32_t value = 0;

    TEST_ASSERT_TRUE(jce_scene_recipe_quantize(1.2345, 1000, &value));
    TEST_ASSERT_EQUAL_INT32(1235, value);
    TEST_ASSERT_TRUE(jce_scene_recipe_quantize(-1.2345, 1000, &value));
    TEST_ASSERT_EQUAL_INT32(-1235, value);
    TEST_ASSERT_TRUE(jce_scene_recipe_quantize(0.00049, 1000, &value));
    TEST_ASSERT_EQUAL_INT32(0, value);
}

static void test_quantization_rejects_non_finite_and_overflow(void)
{
    int32_t value = 7;
    volatile double zero = 0.0;

    TEST_ASSERT_FALSE(jce_scene_recipe_quantize(zero / zero, 1000, &value));
    TEST_ASSERT_FALSE(jce_scene_recipe_quantize(1.0e30, 1000, &value));
    TEST_ASSERT_EQUAL_INT32(7, value);
}

static void test_validate_rejects_duplicate_stable_roles(void)
{
    JceSceneRecipe recipe = make_valid_recipe();
    JceSceneRecipeError error;

    recipe.role_count = 2u;
    recipe.roles[1] = recipe.roles[0];
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_DUPLICATE_ROLE,
                          jce_scene_recipe_validate(&recipe, &error));
    TEST_ASSERT_EQUAL_UINT32(1u, error.item_index);
    TEST_ASSERT_EQUAL_STRING("primary_sun", error.field);
}

static void test_validate_rejects_invalid_count_range(void)
{
    JceSceneRecipe recipe = make_valid_recipe();
    JceSceneRecipeError error;

    recipe.roles[0].min_count = 3u;
    recipe.roles[0].max_count = 2u;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_INVALID_RANGE,
                          jce_scene_recipe_validate(&recipe, &error));
    TEST_ASSERT_EQUAL_UINT32(0u, error.item_index);
}

static void test_validate_rejects_missing_parent_role(void)
{
    JceSceneRecipe recipe = make_valid_hierarchy_recipe();
    JceSceneRecipeError error;

    strcpy(recipe.roles[1].parent_role, "missing_root");
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                          jce_scene_recipe_validate(&recipe, &error));
    TEST_ASSERT_EQUAL_UINT32(1u, error.item_index);
    TEST_ASSERT_EQUAL_STRING("parentRole", error.field);
}

static void test_validate_rejects_parent_cycle(void)
{
    JceSceneRecipe recipe = make_valid_hierarchy_recipe();
    JceSceneRecipeError error;

    strcpy(recipe.roles[0].parent_role, "weather_source");
    recipe.roles[0].parent_instance = 0u;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                          jce_scene_recipe_validate(&recipe, &error));
    TEST_ASSERT_EQUAL_STRING("parentRole", error.field);
}

static void test_validate_requires_guaranteed_parent_instance(void)
{
    JceSceneRecipe recipe = make_valid_hierarchy_recipe();
    JceSceneRecipeError error;

    recipe.roles[1].parent_instance = 1u;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_INVALID_HIERARCHY,
                          jce_scene_recipe_validate(&recipe, &error));
    TEST_ASSERT_EQUAL_UINT32(1u, error.item_index);
    TEST_ASSERT_EQUAL_STRING("parentInstance", error.field);
}

static void test_parse_json_rejects_unapproved_operations(void)
{
    static const char json[] =
        "{\"schemaVersion\":2,\"compilerVersion\":2,\"requestId\":1,"
        "\"seed\":2,\"roles\":[],\"script\":\"spawn_anything()\"}";
    JceSceneRecipe recipe;
    JceSceneRecipeError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_UNKNOWN_FIELD,
                          jce_scene_recipe_parse_json(json, sizeof(json) - 1u,
                                                      &recipe, &error));
    TEST_ASSERT_EQUAL_STRING("script", error.field);
}

static void test_parse_json_accepts_bounded_semantic_recipe(void)
{
    static const char json[] =
        "{\"schemaVersion\":2,\"compilerVersion\":2,\"requestId\":7,"
        "\"seed\":99,\"roles\":[{\"stableRole\":\"weather_source\","
        "\"capability\":\"weather.rain\",\"minCount\":1,\"maxCount\":2,"
        "\"placement\":\"scatter\",\"required\":true,"
        "\"center\":[1.25,2.5,-3.75],\"extent\":[4,0,5],"
        "\"minSeparation\":0.5,\"yawDegrees\":[-45,45],"
        "\"scale\":[0.8,1.2]}]}";
    JceSceneRecipe recipe;
    JceSceneRecipeError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_OK,
                          jce_scene_recipe_parse_json(json, sizeof(json) - 1u,
                                                      &recipe, &error));
    TEST_ASSERT_EQUAL_UINT64(7u, recipe.request_id);
    TEST_ASSERT_EQUAL_UINT64(99u, recipe.seed);
    TEST_ASSERT_EQUAL_UINT32(1u, recipe.role_count);
    TEST_ASSERT_EQUAL_STRING("weather_source", recipe.roles[0].stable_role);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_PLACEMENT_SCATTER,
                          recipe.roles[0].placement);
    TEST_ASSERT_DOUBLE_WITHIN(0.000001, -3.75, recipe.roles[0].center[2]);
}

static void test_parse_json_accepts_explicit_parent_reference(void)
{
    static const char json[] =
        "{\"schemaVersion\":2,\"compilerVersion\":2,\"requestId\":8,"
        "\"seed\":100,\"roles\":[{\"stableRole\":\"scene_root\","
        "\"capability\":\"scene.root\",\"minCount\":1,\"maxCount\":1,"
        "\"placement\":\"single\",\"required\":true},{"
        "\"stableRole\":\"weather_source\",\"capability\":\"weather.rain\","
        "\"parentRole\":\"scene_root\",\"parentInstance\":0,"
        "\"minCount\":1,\"maxCount\":1,\"placement\":\"single\","
        "\"required\":true}]}";
    JceSceneRecipe recipe;
    JceSceneRecipeError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_OK,
                          jce_scene_recipe_parse_json(json, sizeof(json) - 1u,
                                                      &recipe, &error));
    TEST_ASSERT_EQUAL_UINT32(2u, recipe.role_count);
    TEST_ASSERT_EQUAL_STRING("scene_root", recipe.roles[1].parent_role);
    TEST_ASSERT_EQUAL_UINT32(0u, recipe.roles[1].parent_instance);
}

static void test_catalog_json_accepts_semantic_entries(void)
{
    static const char json[] =
        "{\"schemaVersion\":1,\"contentHash\":\"987654321\","
        "\"entries\":[{\"assetId\":\"elemental.state.spring.day\","
        "\"capability\":\"environment.spring.day\","
        "\"contentHash\":\"101\",\"weight\":1,\"enabled\":true}]}";
    JceSceneCatalog catalog;
    JceSceneRecipeError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_OK,
        jce_scene_catalog_parse_json(json, sizeof(json) - 1u,
                                     &catalog, &error));
    TEST_ASSERT_EQUAL_UINT64(987654321u, catalog.content_hash);
    TEST_ASSERT_EQUAL_UINT32(1u, catalog.entry_count);
    TEST_ASSERT_EQUAL_STRING("elemental.state.spring.day",
                             catalog.entries[0].asset_id);
    TEST_ASSERT_EQUAL_STRING("environment.spring.day",
                             catalog.entries[0].capability);
}

static void test_catalog_json_rejects_authoring_paths(void)
{
    static const char json[] =
        "{\"schemaVersion\":1,\"contentHash\":\"987654321\","
        "\"entries\":[{\"assetId\":\"elemental.state.spring.day\","
        "\"capability\":\"environment.spring.day\","
        "\"contentHash\":\"101\",\"weight\":1,\"enabled\":true,"
        "\"sourcePath\":\"D:/assets/scene.json\"}]}";
    JceSceneCatalog catalog;
    JceSceneRecipeError error;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_RECIPE_UNKNOWN_FIELD,
        jce_scene_catalog_parse_json(json, sizeof(json) - 1u,
                                     &catalog, &error));
    TEST_ASSERT_EQUAL_STRING("sourcePath", error.field);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_init_sets_supported_versions);
    RUN_TEST(test_validate_rejects_unsupported_schema);
    RUN_TEST(test_quantization_is_half_away_from_zero);
    RUN_TEST(test_quantization_rejects_non_finite_and_overflow);
    RUN_TEST(test_validate_rejects_duplicate_stable_roles);
    RUN_TEST(test_validate_rejects_invalid_count_range);
    RUN_TEST(test_validate_rejects_missing_parent_role);
    RUN_TEST(test_validate_rejects_parent_cycle);
    RUN_TEST(test_validate_requires_guaranteed_parent_instance);
    RUN_TEST(test_parse_json_rejects_unapproved_operations);
    RUN_TEST(test_parse_json_accepts_bounded_semantic_recipe);
    RUN_TEST(test_parse_json_accepts_explicit_parent_reference);
    RUN_TEST(test_catalog_json_accepts_semantic_entries);
    RUN_TEST(test_catalog_json_rejects_authoring_paths);
    return UNITY_END();
}
