/* End-to-end AI plan to transactional scene activation tests. */

#include <jce/api_runtime.h>
#include <jce/api_scene.h>

#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    bool fail_apply;
    uint32_t apply_count;
    uint32_t attest_count;
    uint32_t activation_count;
    JceEntity root_entity;
    JceEntity primary_entity;
    JceScene *last_current;
    JceScene *last_previous;
} PipelineProbe;

static bool JCE_CALL apply_operation(
    void *user, JceScene *staging,
    const JceScenePlanOperation *operation, JceEntity *out_entity)
{
    PipelineProbe *probe = (PipelineProbe *)user;
    JceEntity entity;
    JceTransform transform;
    JceMeshRenderer mesh_renderer;

    ++probe->apply_count;
    if (probe->fail_apply)
        return false;
    entity = jce_scene_create_entity(staging, operation->stable_role);
    if (entity == JCE_ENTITY_INVALID)
        return false;
    memset(&transform, 0, sizeof(transform));
    transform.position.x = operation->position_mm[0] * 0.001f;
    transform.position.y = operation->position_mm[1] * 0.001f;
    transform.position.z = operation->position_mm[2] * 0.001f;
    transform.rotation.w = 1.0f;
    transform.scale.x = operation->scale_milli[0] * 0.001f;
    transform.scale.y = operation->scale_milli[1] * 0.001f;
    transform.scale.z = operation->scale_milli[2] * 0.001f;
    jce_scene_set_transform(staging, entity, &transform);
    jce_mesh_renderer_init(&mesh_renderer);
    mesh_renderer.mesh_path = jce_scene_intern(staging, operation->asset_id);
    mesh_renderer.visible = true;
    jce_scene_set_mesh_renderer(staging, entity, &mesh_renderer);
    if (strcmp(operation->stable_role, "environment.root") == 0)
        probe->root_entity = entity;
    else if (strcmp(operation->stable_role, "environment.primary") == 0)
        probe->primary_entity = entity;
    *out_entity = entity;
    return true;
}

static bool JCE_CALL attest_operation(
    void *user, JceScene *staging,
    const JceScenePlanOperation *expected, JceEntity entity,
    JceScenePlanOperation *out_actual)
{
    PipelineProbe *probe = (PipelineProbe *)user;
    const char *name = jce_scene_entity_name(staging, entity);
    const JceMeshRenderer *mesh_renderer =
        jce_scene_get_mesh_renderer_const(staging, entity);
    JceTransform *transform = jce_scene_get_transform(staging, entity);

    ++probe->attest_count;
    if (!name || !mesh_renderer || !transform || !out_actual)
        return false;
    memset(out_actual, 0, sizeof(*out_actual));
    out_actual->stable_entity_id = expected->stable_entity_id;
    out_actual->parent_stable_entity_id = expected->parent_stable_entity_id;
    if (strcmp(mesh_renderer->mesh_path, expected->asset_id) == 0)
        out_actual->asset_content_hash = expected->asset_content_hash;
    strcpy(out_actual->stable_role, name);
    strcpy(out_actual->asset_id, mesh_renderer->mesh_path);
    if (!jce_scene_recipe_quantize(transform->position.x, 1000,
                                   &out_actual->position_mm[0]) ||
        !jce_scene_recipe_quantize(transform->position.y, 1000,
                                   &out_actual->position_mm[1]) ||
        !jce_scene_recipe_quantize(transform->position.z, 1000,
                                   &out_actual->position_mm[2]) ||
        !jce_scene_recipe_quantize(transform->scale.x, 1000,
                                   &out_actual->scale_milli[0]) ||
        !jce_scene_recipe_quantize(transform->scale.y, 1000,
                                   &out_actual->scale_milli[1]) ||
        !jce_scene_recipe_quantize(transform->scale.z, 1000,
                                   &out_actual->scale_milli[2]))
        return false;
    return true;
}

static bool JCE_CALL validate_candidate(
    void *user, const JceScene *staging,
    const JceSceneFrozenPlan *plan)
{
    (void)user;
    return staging != NULL && plan != NULL && plan->operation_count > 0u;
}

static JceScenePrewarmResult JCE_CALL prewarm_candidate(
    void *user, JceScene *staging, const JceSceneFrozenPlan *plan)
{
    (void)user;
    return staging && plan ? JCE_SCENE_PREWARM_READY
                           : JCE_SCENE_PREWARM_FAILED;
}

static void JCE_CALL on_activated(void *user, JceScene *current,
                                  JceScene *previous,
                                  uint64_t generation)
{
    PipelineProbe *probe = (PipelineProbe *)user;
    (void)generation;
    ++probe->activation_count;
    probe->last_current = current;
    probe->last_previous = previous;
}

static JceSceneCatalog make_catalog(void)
{
    JceSceneCatalog catalog;

    jce_scene_catalog_init(&catalog);
    catalog.content_hash = 0x51ceca7a10ULL;
    catalog.entry_count = 3u;
    strcpy(catalog.entries[0].asset_id, "elemental.scene.root");
    strcpy(catalog.entries[0].capability, "environment.root");
    catalog.entries[0].content_hash = 101u;
    catalog.entries[0].weight = 1u;
    catalog.entries[0].enabled = true;
    strcpy(catalog.entries[1].asset_id, "elemental.environment.spring.day");
    strcpy(catalog.entries[1].capability, "environment.spring.day");
    catalog.entries[1].content_hash = 102u;
    catalog.entries[1].weight = 1u;
    catalog.entries[1].enabled = true;
    strcpy(catalog.entries[2].asset_id, "elemental.environment.autumn.night");
    strcpy(catalog.entries[2].capability, "environment.autumn.night");
    catalog.entries[2].content_hash = 103u;
    catalog.entries[2].weight = 1u;
    catalog.entries[2].enabled = true;
    return catalog;
}

static JceSceneRecipe make_bootstrap(void)
{
    JceSceneRecipe recipe;
    JceSceneRecipeRole *root;
    JceSceneRecipeRole *primary;

    jce_scene_recipe_init(&recipe);
    recipe.request_id = 1u;
    recipe.seed = 1u;
    recipe.role_count = 2u;
    root = &recipe.roles[0];
    strcpy(root->stable_role, "environment.root");
    strcpy(root->capability, "environment.root");
    root->min_count = 1u;
    root->max_count = 1u;
    root->placement = JCE_SCENE_PLACEMENT_SINGLE;
    root->required = true;
    root->scale_min = 1.0;
    root->scale_max = 1.0;

    primary = &recipe.roles[1];
    strcpy(primary->stable_role, "environment.primary");
    strcpy(primary->capability, "environment.autumn.night");
    strcpy(primary->parent_role, "environment.root");
    primary->parent_instance = 0u;
    primary->min_count = 1u;
    primary->max_count = 1u;
    primary->placement = JCE_SCENE_PLACEMENT_SINGLE;
    primary->required = true;
    primary->scale_min = 1.0;
    primary->scale_max = 1.0;
    return recipe;
}

static JceAiSceneRequest make_request(uint64_t request_id, uint64_t seed)
{
    JceAiSceneRequest request;

    memset(&request, 0, sizeof(request));
    request.request_id = request_id;
    request.seed = seed;
    request.offline = true;
    strcpy(request.locale, "zh-CN");
    strcpy(request.intent, "advance to a calm but non-repeating scene");
    return request;
}

static JceSceneGenerationCoordinator *make_coordinator(
    PipelineProbe *probe, JceScene *initial,
    const JceSceneRecipe *bootstrap)
{
    JceSceneGenerationDesc desc;

    jce_scene_generation_desc_default(&desc);
    desc.catalog = make_catalog();
    desc.director.bootstrap_recipes = bootstrap;
    desc.director.bootstrap_recipe_count = 1u;
    desc.transaction.user = probe;
    desc.transaction.initial_scene = initial;
    desc.transaction.own_initial_scene = true;
    desc.transaction.entity_budget = 16u;
    desc.transaction.retain_previous_until_healthy = true;
    desc.transaction.apply = apply_operation;
    desc.transaction.require_attestation = true;
    desc.transaction.attest = attest_operation;
    desc.transaction.validate = validate_candidate;
    desc.transaction.prewarm = prewarm_candidate;
    desc.transaction.on_activated = on_activated;
    desc.max_operations_per_update = 1u;
    return jce_scene_generation_create(&desc);
}

static void advance_to_ready(JceSceneGenerationCoordinator *coordinator)
{
    uint32_t guard = 0u;

    while (jce_scene_generation_status(coordinator) !=
               JCE_SCENE_GENERATION_FROZEN_READY &&
           jce_scene_generation_status(coordinator) !=
               JCE_SCENE_GENERATION_FAILED &&
           guard++ < 16u) {
        jce_scene_generation_update(coordinator, 0.016);
    }
}

static void test_offline_ai_plan_requires_explicit_commit_and_health_gate(void)
{
    PipelineProbe probe = {0};
    JceSceneRecipe bootstrap = make_bootstrap();
    JceScene *initial = jce_scene_create();
    JceSceneGenerationCoordinator *coordinator;
    JceAiSceneRequest request = make_request(40u, 500u);
    JceSceneFrozenPlan plan;

    TEST_ASSERT_NOT_NULL(initial);
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID,
                          jce_scene_create_entity(initial, "active"));
    coordinator = make_coordinator(&probe, initial, &bootstrap);
    TEST_ASSERT_NOT_NULL(coordinator);
    TEST_ASSERT_TRUE(jce_scene_generation_request(coordinator, &request));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_PLANNING,
                          jce_scene_generation_status(coordinator));

    advance_to_ready(coordinator);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_FROZEN_READY,
                          jce_scene_generation_status(coordinator));
    TEST_ASSERT_EQUAL_PTR(initial,
                          jce_scene_generation_active_scene(coordinator));
    TEST_ASSERT_TRUE(jce_scene_generation_frozen_plan(coordinator, &plan));
    TEST_ASSERT_EQUAL_UINT32(2u, plan.operation_count);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_SOURCE_BOOTSTRAP,
                          jce_scene_generation_source(coordinator));
    TEST_ASSERT_EQUAL_UINT32(plan.operation_count, probe.attest_count);
    TEST_ASSERT_EQUAL_UINT64(
        plan.plan_hash,
        jce_scene_generation_pending_attestation_hash(coordinator));

    TEST_ASSERT_TRUE(jce_scene_generation_commit(coordinator));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_HEALTH_CHECK,
                          jce_scene_generation_status(coordinator));
    TEST_ASSERT_EQUAL_UINT32(1u, probe.activation_count);
    TEST_ASSERT_EQUAL_PTR(initial, probe.last_previous);
    TEST_ASSERT_TRUE(jce_scene_generation_mark_healthy(coordinator, true));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_ACTIVE,
                          jce_scene_generation_status(coordinator));
    TEST_ASSERT_EQUAL_UINT64(
        probe.root_entity,
        jce_scene_get_parent(jce_scene_generation_active_scene(coordinator),
                             probe.primary_entity));
    TEST_ASSERT_EQUAL_UINT64(plan.plan_hash,
        jce_scene_generation_active_plan_hash(coordinator));
    TEST_ASSERT_EQUAL_UINT64(
        plan.plan_hash,
        jce_scene_generation_active_attestation_hash(coordinator));
    jce_scene_generation_destroy(coordinator);
}

static void test_coordinator_surfaces_live_attestation_state(void)
{
    PipelineProbe probe = {0};
    JceSceneRecipe bootstrap = make_bootstrap();
    JceSceneGenerationCoordinator *coordinator =
        make_coordinator(&probe, jce_scene_create(), &bootstrap);
    JceAiSceneRequest request = make_request(43u, 503u);
    uint32_t guard = 0u;
    bool saw_attesting = false;

    TEST_ASSERT_NOT_NULL(coordinator);
    TEST_ASSERT_TRUE(jce_scene_generation_request(coordinator, &request));
    while (guard++ < 16u) {
        jce_scene_generation_update(coordinator, 0.016);
        if (jce_scene_generation_status(coordinator) ==
            JCE_SCENE_GENERATION_ATTESTING) {
            saw_attesting = true;
            break;
        }
        if (jce_scene_generation_status(coordinator) ==
            JCE_SCENE_GENERATION_FAILED)
            break;
    }

    TEST_ASSERT_TRUE(saw_attesting);
    TEST_ASSERT_EQUAL_UINT32(0u, probe.attest_count);
    jce_scene_generation_destroy(coordinator);
}

static void test_apply_failure_and_cancel_preserve_active_scene(void)
{
    PipelineProbe probe = {0};
    JceSceneRecipe bootstrap = make_bootstrap();
    JceScene *initial = jce_scene_create();
    JceSceneGenerationCoordinator *coordinator =
        make_coordinator(&probe, initial, &bootstrap);
    JceAiSceneRequest request = make_request(41u, 501u);

    probe.fail_apply = true;
    TEST_ASSERT_TRUE(jce_scene_generation_request(coordinator, &request));
    advance_to_ready(coordinator);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_FAILED,
                          jce_scene_generation_status(coordinator));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_ERROR_APPLY,
                          jce_scene_generation_transaction_error(coordinator));
    TEST_ASSERT_EQUAL_PTR(initial,
                          jce_scene_generation_active_scene(coordinator));

    probe.fail_apply = false;
    request.request_id++;
    request.seed++;
    TEST_ASSERT_TRUE(jce_scene_generation_request(coordinator, &request));
    jce_scene_generation_cancel(coordinator);
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_CANCELLED,
                          jce_scene_generation_status(coordinator));
    TEST_ASSERT_EQUAL_PTR(initial,
                          jce_scene_generation_active_scene(coordinator));
    jce_scene_generation_destroy(coordinator);
}

static void test_failed_health_check_rolls_back_generation(void)
{
    PipelineProbe probe = {0};
    JceSceneRecipe bootstrap = make_bootstrap();
    JceScene *initial = jce_scene_create();
    JceSceneGenerationCoordinator *coordinator =
        make_coordinator(&probe, initial, &bootstrap);
    JceAiSceneRequest request = make_request(42u, 502u);

    TEST_ASSERT_TRUE(jce_scene_generation_request(coordinator, &request));
    advance_to_ready(coordinator);
    TEST_ASSERT_TRUE(jce_scene_generation_commit(coordinator));
    TEST_ASSERT_TRUE(jce_scene_generation_mark_healthy(coordinator, false));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_GENERATION_ACTIVE,
                          jce_scene_generation_status(coordinator));
    TEST_ASSERT_EQUAL_PTR(initial,
                          jce_scene_generation_active_scene(coordinator));
    TEST_ASSERT_EQUAL_UINT64(0u,
        jce_scene_generation_active_plan_hash(coordinator));
    TEST_ASSERT_EQUAL_UINT64(
        0u, jce_scene_generation_active_attestation_hash(coordinator));
    TEST_ASSERT_EQUAL_UINT32(2u, probe.activation_count);
    jce_scene_generation_destroy(coordinator);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_offline_ai_plan_requires_explicit_commit_and_health_gate);
    RUN_TEST(test_coordinator_surfaces_live_attestation_state);
    RUN_TEST(test_apply_failure_and_cancel_preserve_active_scene);
    RUN_TEST(test_failed_health_check_rolls_back_generation);
    return UNITY_END();
}
