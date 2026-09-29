/* Isolated staging, generation guards, commit, health, and rollback tests. */

#include <jce/api_scene.h>

#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    int apply_calls;
    int fail_apply_at;
    bool tamper_transform;
    int attest_calls;
    bool validate_ok;
    int validate_calls;
    int prewarm_pending;
    int prewarm_calls;
    int destroy_calls;
    int activate_calls;
} TxFixture;

static bool JCE_CALL apply_operation(void *user, JceScene *scene,
                                     const JceScenePlanOperation *operation,
                                     JceEntity *out_entity)
{
    TxFixture *fixture = (TxFixture *)user;
    JceEntity entity;
    JceTransform transform;
    JceMeshRenderer mesh_renderer;

    ++fixture->apply_calls;
    if (fixture->fail_apply_at == fixture->apply_calls)
        return false;
    entity = jce_scene_create_entity(scene, operation->stable_role);
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
    if (fixture->tamper_transform && fixture->apply_calls == 1)
        transform.position.x += 1.0f;
    jce_scene_set_transform(scene, entity, &transform);
    jce_mesh_renderer_init(&mesh_renderer);
    mesh_renderer.mesh_path = jce_scene_intern(scene, operation->asset_id);
    mesh_renderer.visible = true;
    jce_scene_set_mesh_renderer(scene, entity, &mesh_renderer);
    *out_entity = entity;
    return true;
}

static bool JCE_CALL attest_operation(
    void *user, JceScene *scene,
    const JceScenePlanOperation *expected, JceEntity entity,
    JceScenePlanOperation *out_actual)
{
    TxFixture *fixture = (TxFixture *)user;
    const char *name = jce_scene_entity_name(scene, entity);
    const JceMeshRenderer *mesh_renderer =
        jce_scene_get_mesh_renderer_const(scene, entity);
    JceTransform *transform = jce_scene_get_transform(scene, entity);

    ++fixture->attest_calls;
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

static bool JCE_CALL validate_candidate(void *user, const JceScene *scene,
                                        const JceSceneFrozenPlan *plan)
{
    TxFixture *fixture = (TxFixture *)user;
    (void)scene;
    (void)plan;
    ++fixture->validate_calls;
    return fixture->validate_ok;
}

static JceScenePrewarmResult JCE_CALL
prewarm_candidate(void *user, JceScene *scene,
                  const JceSceneFrozenPlan *plan)
{
    TxFixture *fixture = (TxFixture *)user;
    (void)scene;
    (void)plan;
    ++fixture->prewarm_calls;
    if (fixture->prewarm_calls <= fixture->prewarm_pending)
        return JCE_SCENE_PREWARM_PENDING;
    return JCE_SCENE_PREWARM_READY;
}

static void JCE_CALL destroy_scene(void *user, JceScene *scene)
{
    TxFixture *fixture = (TxFixture *)user;
    ++fixture->destroy_calls;
    jce_scene_destroy(scene);
}

static void JCE_CALL activated(void *user, JceScene *current,
                               JceScene *previous, uint64_t generation)
{
    TxFixture *fixture = (TxFixture *)user;
    (void)current;
    (void)previous;
    (void)generation;
    ++fixture->activate_calls;
}

static JceSceneFrozenPlan make_plan(uint64_t request_id, uint64_t seed)
{
    JceSceneCatalog catalog;
    JceSceneRecipe recipe;
    JceSceneCompileError error;
    JceSceneFrozenPlan plan;

    jce_scene_catalog_init(&catalog);
    catalog.content_hash = 77u;
    catalog.entry_count = 2u;
    strcpy(catalog.entries[0].asset_id, "kit.rock.round");
    strcpy(catalog.entries[0].capability, "nature.rock");
    catalog.entries[0].content_hash = 88u;
    catalog.entries[0].weight = 1u;
    catalog.entries[0].enabled = true;

    strcpy(catalog.entries[1].asset_id, "scene.root");
    strcpy(catalog.entries[1].capability, "scene.root");
    catalog.entries[1].content_hash = 89u;
    catalog.entries[1].weight = 1u;
    catalog.entries[1].enabled = true;

    jce_scene_recipe_init(&recipe);
    recipe.request_id = request_id;
    recipe.seed = seed;
    recipe.role_count = 2u;
    strcpy(recipe.roles[0].stable_role, "scene_root");
    strcpy(recipe.roles[0].capability, "scene.root");
    recipe.roles[0].min_count = 1u;
    recipe.roles[0].max_count = 1u;
    recipe.roles[0].placement = JCE_SCENE_PLACEMENT_SINGLE;
    recipe.roles[0].required = true;
    recipe.roles[0].scale_min = 1.0;
    recipe.roles[0].scale_max = 1.0;

    strcpy(recipe.roles[1].stable_role, "shore_rock");
    strcpy(recipe.roles[1].capability, "nature.rock");
    strcpy(recipe.roles[1].parent_role, "scene_root");
    recipe.roles[1].parent_instance = 0u;
    recipe.roles[1].min_count = 3u;
    recipe.roles[1].max_count = 3u;
    recipe.roles[1].placement = JCE_SCENE_PLACEMENT_GRID;
    recipe.roles[1].required = true;
    recipe.roles[1].extent[0] = 3.0;
    recipe.roles[1].extent[2] = 3.0;
    recipe.roles[1].scale_min = 1.0;
    recipe.roles[1].scale_max = 1.0;
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &plan, &error));
    return plan;
}

static JceSceneTransaction *make_transaction(TxFixture *fixture,
                                             JceScene *initial,
                                             bool own_initial)
{
    JceSceneTransactionDesc desc;

    jce_scene_transaction_desc_default(&desc);
    desc.user = fixture;
    desc.initial_scene = initial;
    desc.own_initial_scene = own_initial;
    desc.apply = apply_operation;
    desc.require_attestation = true;
    desc.attest = attest_operation;
    desc.validate = validate_candidate;
    desc.prewarm = prewarm_candidate;
    desc.destroy_scene = destroy_scene;
    desc.on_activated = activated;
    desc.retain_previous_until_healthy = true;
    return jce_scene_transaction_create(&desc);
}

static void drive_until_ready(JceSceneTransaction *transaction)
{
    int guard;
    for (guard = 0; guard < 32; ++guard) {
        JceSceneTransactionState state =
            jce_scene_transaction_step(transaction, 2u);
        if (state == JCE_SCENE_TRANSACTION_READY ||
            state == JCE_SCENE_TRANSACTION_FAILED)
            return;
    }
    TEST_FAIL_MESSAGE("transaction did not reach a terminal build state");
}

static void test_live_ecs_attestation_matches_frozen_plan(void)
{
    TxFixture fixture = {0};
    JceSceneFrozenPlan plan = make_plan(101u, 202u);
    JceSceneTransaction *transaction;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, NULL, false);
    TEST_ASSERT_NOT_NULL(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    drive_until_ready(transaction);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_READY,
                          jce_scene_transaction_state(transaction));
    TEST_ASSERT_EQUAL_UINT32(plan.operation_count,
                             (uint32_t)fixture.attest_calls);
    TEST_ASSERT_EQUAL_UINT64(
        plan.plan_hash,
        jce_scene_transaction_pending_attestation_hash(transaction));
    jce_scene_transaction_destroy(transaction);
}

static void test_tampered_live_transform_is_rejected_before_commit(void)
{
    TxFixture fixture = {0};
    JceScene *active = jce_scene_create();
    JceSceneFrozenPlan plan = make_plan(303u, 404u);
    JceSceneTransaction *transaction;

    fixture.validate_ok = true;
    fixture.tamper_transform = true;
    transaction = make_transaction(&fixture, active, false);
    TEST_ASSERT_NOT_NULL(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    drive_until_ready(transaction);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_FAILED,
                          jce_scene_transaction_state(transaction));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_ERROR_ATTESTATION,
                          jce_scene_transaction_error(transaction));
    TEST_ASSERT_EQUAL_PTR(active,
                          jce_scene_transaction_active_scene(transaction));
    TEST_ASSERT_EQUAL_INT(0, fixture.activate_calls);
    TEST_ASSERT_EQUAL_UINT64(
        0u, jce_scene_transaction_pending_attestation_hash(transaction));
    jce_scene_transaction_destroy(transaction);
    jce_scene_destroy(active);
}

static void test_failed_candidate_never_mutates_active_scene(void)
{
    TxFixture fixture = {0};
    JceScene *active = jce_scene_create();
    JceSceneFrozenPlan plan = make_plan(1u, 2u);
    JceSceneTransaction *transaction;

    fixture.validate_ok = true;
    fixture.fail_apply_at = 2;
    transaction = make_transaction(&fixture, active, false);
    TEST_ASSERT_NOT_NULL(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    drive_until_ready(transaction);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_FAILED,
                          jce_scene_transaction_state(transaction));
    TEST_ASSERT_EQUAL_PTR(active,
                          jce_scene_transaction_active_scene(transaction));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_ERROR_APPLY,
                          jce_scene_transaction_error(transaction));
    TEST_ASSERT_EQUAL_INT(0, fixture.activate_calls);
    jce_scene_transaction_destroy(transaction);
    jce_scene_destroy(active);
}

static void test_commit_is_explicit_and_generation_guarded(void)
{
    TxFixture fixture = {0};
    JceScene *active = jce_scene_create();
    JceSceneFrozenPlan plan = make_plan(3u, 4u);
    JceSceneTransaction *transaction;
    uint64_t generation;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, active, false);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    generation = jce_scene_transaction_pending_generation(transaction);
    drive_until_ready(transaction);

    TEST_ASSERT_EQUAL_PTR(active,
                          jce_scene_transaction_active_scene(transaction));
    TEST_ASSERT_FALSE(jce_scene_transaction_commit(transaction,
                                                    generation - 1u));
    TEST_ASSERT_EQUAL_PTR(active,
                          jce_scene_transaction_active_scene(transaction));
    TEST_ASSERT_TRUE(jce_scene_transaction_commit(transaction, generation));
    TEST_ASSERT_NOT_EQUAL(active,
                          jce_scene_transaction_active_scene(transaction));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_HEALTH_CHECK,
                          jce_scene_transaction_state(transaction));
    TEST_ASSERT_EQUAL_UINT64(plan.plan_hash,
                             jce_scene_transaction_active_plan_hash(
                                 transaction));
    TEST_ASSERT_EQUAL_UINT64(plan.plan_hash,
                             jce_scene_transaction_active_attestation_hash(
                                 transaction));
    TEST_ASSERT_EQUAL_INT(1, fixture.activate_calls);

    TEST_ASSERT_TRUE(jce_scene_transaction_mark_healthy(transaction, true));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_ACTIVE,
                          jce_scene_transaction_state(transaction));
    jce_scene_transaction_destroy(transaction);
    jce_scene_destroy(active);
}

static void test_failed_health_check_rolls_back_previous_scene(void)
{
    TxFixture fixture = {0};
    JceScene *active = jce_scene_create();
    JceSceneFrozenPlan plan = make_plan(5u, 6u);
    JceSceneTransaction *transaction;
    uint64_t generation;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, active, false);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    generation = jce_scene_transaction_pending_generation(transaction);
    drive_until_ready(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_commit(transaction, generation));
    TEST_ASSERT_TRUE(jce_scene_transaction_mark_healthy(transaction, false));

    TEST_ASSERT_EQUAL_PTR(active,
                          jce_scene_transaction_active_scene(transaction));
    TEST_ASSERT_EQUAL_UINT64(0u,
                             jce_scene_transaction_active_plan_hash(
                                 transaction));
    TEST_ASSERT_EQUAL_UINT64(0u,
                             jce_scene_transaction_active_attestation_hash(
                                 transaction));
    TEST_ASSERT_EQUAL_INT(1, fixture.destroy_calls);
    TEST_ASSERT_EQUAL_INT(2, fixture.activate_calls);
    jce_scene_transaction_destroy(transaction);
    jce_scene_destroy(active);
}

static void test_cancelled_generation_cannot_commit_after_restart(void)
{
    TxFixture fixture = {0};
    JceSceneFrozenPlan first = make_plan(7u, 8u);
    JceSceneFrozenPlan second = make_plan(9u, 10u);
    JceSceneTransaction *transaction;
    uint64_t stale_generation;
    uint64_t current_generation;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, NULL, false);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &first));
    stale_generation = jce_scene_transaction_pending_generation(transaction);
    jce_scene_transaction_cancel(transaction);
    TEST_ASSERT_EQUAL_UINT64(
        0u, jce_scene_transaction_pending_generation(transaction));
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &second));
    current_generation = jce_scene_transaction_pending_generation(transaction);
    TEST_ASSERT_GREATER_THAN_UINT64(stale_generation, current_generation);
    drive_until_ready(transaction);
    TEST_ASSERT_FALSE(jce_scene_transaction_commit(transaction,
                                                    stale_generation));
    TEST_ASSERT_TRUE(jce_scene_transaction_commit(transaction,
                                                   current_generation));
    TEST_ASSERT_EQUAL_UINT64(
        0u, jce_scene_transaction_pending_generation(transaction));
    TEST_ASSERT_TRUE(jce_scene_transaction_mark_healthy(transaction, true));
    jce_scene_transaction_destroy(transaction);
}

static void test_cancel_is_ignored_after_commit_until_health_result(void)
{
    TxFixture fixture = {0};
    JceScene *active = jce_scene_create();
    JceSceneFrozenPlan plan = make_plan(17u, 18u);
    JceSceneTransaction *transaction;
    uint64_t generation;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, active, true);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    generation = jce_scene_transaction_pending_generation(transaction);
    drive_until_ready(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_commit(transaction, generation));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_HEALTH_CHECK,
                          jce_scene_transaction_state(transaction));

    jce_scene_transaction_cancel(transaction);

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_HEALTH_CHECK,
                          jce_scene_transaction_state(transaction));
    TEST_ASSERT_EQUAL_INT(0, fixture.destroy_calls);
    TEST_ASSERT_TRUE(jce_scene_transaction_mark_healthy(transaction, true));
    TEST_ASSERT_EQUAL_INT(1, fixture.destroy_calls);
    jce_scene_transaction_destroy(transaction);
    TEST_ASSERT_EQUAL_INT(2, fixture.destroy_calls);
}

static void test_role_map_replays_from_frozen_plan(void)
{
    TxFixture fixture = {0};
    JceSceneFrozenPlan plan = make_plan(11u, 12u);
    JceSceneTransaction *transaction;
    JceEntity entities[4];
    uint64_t generation;
    uint32_t count;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, NULL, false);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    generation = jce_scene_transaction_pending_generation(transaction);
    drive_until_ready(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_commit(transaction, generation));
    TEST_ASSERT_TRUE(jce_scene_transaction_mark_healthy(transaction, true));

    count = jce_scene_transaction_find_role(transaction, "shore_rock",
                                            entities, 4u);
    TEST_ASSERT_EQUAL_UINT32(3u, count);
    TEST_ASSERT_NOT_EQUAL(JCE_ENTITY_INVALID, entities[0]);
    TEST_ASSERT_NOT_EQUAL(entities[0], entities[1]);
    TEST_ASSERT_EQUAL_UINT64(plan.plan_hash,
                             jce_scene_transaction_active_plan_hash(
                                 transaction));
    jce_scene_transaction_destroy(transaction);
}

static void test_commit_preserves_frozen_plan_hierarchy(void)
{
    TxFixture fixture = {0};
    JceSceneFrozenPlan plan = make_plan(13u, 14u);
    JceSceneTransaction *transaction;
    JceScene *active;
    JceEntity root;
    JceEntity rocks[4];
    uint64_t generation;
    uint32_t count;
    uint32_t i;

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, NULL, false);
    TEST_ASSERT_TRUE(jce_scene_transaction_begin(transaction, &plan));
    generation = jce_scene_transaction_pending_generation(transaction);
    drive_until_ready(transaction);
    TEST_ASSERT_TRUE(jce_scene_transaction_commit(transaction, generation));
    TEST_ASSERT_TRUE(jce_scene_transaction_mark_healthy(transaction, true));

    active = jce_scene_transaction_active_scene(transaction);
    count = jce_scene_transaction_find_role(transaction, "scene_root",
                                            &root, 1u);
    TEST_ASSERT_EQUAL_UINT32(1u, count);
    count = jce_scene_transaction_find_role(transaction, "shore_rock",
                                            rocks, 4u);
    TEST_ASSERT_EQUAL_UINT32(3u, count);
    for (i = 0u; i < count; ++i)
        TEST_ASSERT_EQUAL_UINT64(root,
                                jce_scene_get_parent(active, rocks[i]));
    jce_scene_transaction_destroy(transaction);
}

static void test_begin_rejects_cyclic_frozen_plan(void)
{
    TxFixture fixture = {0};
    JceSceneFrozenPlan plan = make_plan(15u, 16u);
    JceSceneTransaction *transaction;
    uint32_t root = UINT32_MAX;
    uint32_t child = UINT32_MAX;
    uint32_t i;

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

    fixture.validate_ok = true;
    transaction = make_transaction(&fixture, NULL, false);
    TEST_ASSERT_FALSE(jce_scene_transaction_begin(transaction, &plan));
    TEST_ASSERT_EQUAL_INT(JCE_SCENE_TRANSACTION_ERROR_HIERARCHY,
                          jce_scene_transaction_error(transaction));
    TEST_ASSERT_EQUAL_INT(0, fixture.apply_calls);
    jce_scene_transaction_destroy(transaction);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_live_ecs_attestation_matches_frozen_plan);
    RUN_TEST(test_tampered_live_transform_is_rejected_before_commit);
    RUN_TEST(test_failed_candidate_never_mutates_active_scene);
    RUN_TEST(test_commit_is_explicit_and_generation_guarded);
    RUN_TEST(test_failed_health_check_rolls_back_previous_scene);
    RUN_TEST(test_cancelled_generation_cannot_commit_after_restart);
    RUN_TEST(test_cancel_is_ignored_after_commit_until_health_result);
    RUN_TEST(test_role_map_replays_from_frozen_plan);
    RUN_TEST(test_commit_preserves_frozen_plan_hierarchy);
    RUN_TEST(test_begin_rejects_cyclic_frozen_plan);
    return UNITY_END();
}
