/* AI selects semantic state; JCE validates and compiles; Lua consumes it. */

#include "es_scene_orchestrator.h"

#include <jce/api_scene.h>
#include <jce/api.h>

#include <string.h>

#define ES_AI_CATALOG_PATH "scene_ai/elemental_catalog.json"
#define ES_AI_BOOTSTRAP_PATH "scene_ai/bootstrap_recipes.json"
#define ES_AI_MAX_BOOTSTRAP 16u
#define ES_AI_FILE_MAX_BYTES 65536u

typedef struct {
    const char *asset_id;
    int state_index;
} EsStateBinding;

static const EsStateBinding ES_STATE_BINDINGS[] = {
    { "elemental.state.spring.day", 0 },
    { "elemental.state.spring.night", 1 },
    { "elemental.state.winter.day", 2 },
    { "elemental.state.winter.night", 3 },
    { "elemental.state.autumn.day", 4 },
    { "elemental.state.autumn.night", 5 },
    { "elemental.state.rainy.day", 6 },
    { "elemental.state.rainy.night", 7 },
};

struct EsSceneOrchestrator {
    JceAiSceneDirector *director;
    JceSceneCatalog catalog;
    JceSceneRecipe bootstrap[ES_AI_MAX_BOOTSTRAP];
    uint32_t bootstrap_count;
    uint64_t next_request_id;
    uint64_t next_seed;
    uint64_t last_plan_hash;
    bool pending;
    int result_status;
    int result_state;
    int result_error;
};

static bool root_key_allowed(const char *key)
{
    return key && (strcmp(key, "contract") == 0 ||
                   strcmp(key, "schemaVersion") == 0 ||
                   strcmp(key, "recipes") == 0);
}

static bool parse_bootstrap(const void *data, size_t size,
                            JceSceneRecipe *recipes,
                            uint32_t capacity, uint32_t *out_count)
{
    JceJson *root;
    JceJson *array;
    JceJson *child;
    int count;
    uint32_t i;

    if (!data || !recipes || !out_count)
        return false;
    root = jce_json_parse((const char *)data, size);
    if (!root || !jce_json_is_object(root)) {
        jce_json_free(root);
        return false;
    }
    for (child = jce_json_first_child(root); child;
         child = jce_json_next_sibling(child)) {
        if (!root_key_allowed(jce_json_member_key(child))) {
            jce_json_free(root);
            return false;
        }
    }
    if (strcmp(jce_json_get_string(root, "contract", ""),
               "jce.ai-scene-bootstrap") != 0 ||
        jce_json_get_int(root, "schemaVersion", -1) != 1) {
        jce_json_free(root);
        return false;
    }
    array = jce_json_get(root, "recipes");
    count = array && jce_json_is_array(array)
        ? jce_json_array_size(array) : -1;
    if (count <= 0 || (uint32_t)count > capacity) {
        jce_json_free(root);
        return false;
    }
    for (i = 0u; i < (uint32_t)count; ++i) {
        JceJson *node = jce_json_array_at(array, (int)i);
        JceSceneRecipeError error;
        char *json;
        bool ok;

        if (!node || !jce_json_is_object(node)) {
            jce_json_free(root);
            return false;
        }
        json = jce_json_print(node, false);
        if (!json) {
            jce_json_free(root);
            return false;
        }
        ok = jce_scene_recipe_parse_json(
            json, strlen(json), &recipes[i], &error) ==
            JCE_SCENE_RECIPE_OK;
        jce_json_free_string(json);
        if (!ok) {
            jce_json_free(root);
            return false;
        }
    }
    jce_json_free(root);
    *out_count = (uint32_t)count;
    return true;
}

static bool load_contracts(EsSceneOrchestrator *orchestrator,
                           const JceFileSystem *filesystem)
{
    void *data;
    uint64_t size = 0u;
    JceSceneRecipeError error;
    bool ok;

    data = jce_fs_read_all(filesystem, ES_AI_CATALOG_PATH, &size);
    if (!data || size == 0u || size > ES_AI_FILE_MAX_BYTES) {
        jce_free(data);
        return false;
    }
    ok = jce_scene_catalog_parse_json(
        (const char *)data, (size_t)size, &orchestrator->catalog, &error) ==
        JCE_SCENE_RECIPE_OK;
    jce_free(data);
    if (!ok)
        return false;

    data = jce_fs_read_all(filesystem, ES_AI_BOOTSTRAP_PATH, &size);
    if (!data || size == 0u || size > ES_AI_FILE_MAX_BYTES) {
        jce_free(data);
        return false;
    }
    ok = parse_bootstrap(data, (size_t)size, orchestrator->bootstrap,
                         ES_AI_MAX_BOOTSTRAP,
                         &orchestrator->bootstrap_count);
    jce_free(data);
    return ok;
}

static int state_for_asset(const char *asset_id)
{
    uint32_t i;

    for (i = 0u; i < sizeof(ES_STATE_BINDINGS) /
                        sizeof(ES_STATE_BINDINGS[0]); ++i) {
        if (strcmp(asset_id, ES_STATE_BINDINGS[i].asset_id) == 0)
            return ES_STATE_BINDINGS[i].state_index;
    }
    return -1;
}

static const JceScenePlanOperation *find_plan_role(
    const JceSceneFrozenPlan *plan, const char *stable_role)
{
    const JceScenePlanOperation *found = NULL;
    uint32_t i;

    for (i = 0u; i < plan->operation_count; ++i) {
        if (strcmp(plan->operations[i].stable_role, stable_role) != 0)
            continue;
        if (found)
            return NULL;
        found = &plan->operations[i];
    }
    return found;
}

EsSceneOrchestrator *es_scene_orchestrator_create(
    JceRuntime *runtime, const JceFileSystem *filesystem)
{
    EsSceneOrchestrator *orchestrator;
    JceAiSceneDirectorDesc desc;

    if (!runtime || !filesystem)
        return NULL;
    orchestrator = (EsSceneOrchestrator *)jce_malloc(sizeof(*orchestrator));
    if (!orchestrator)
        return NULL;
    memset(orchestrator, 0, sizeof(*orchestrator));
    orchestrator->next_request_id = 1u;
    orchestrator->next_seed = 1u;
    if (!load_contracts(orchestrator, filesystem)) {
        LOG_ERROR("es_ai", "%s", "invalid or missing scene AI contracts");
        jce_free(orchestrator);
        return NULL;
    }

    jce_ai_scene_director_desc_default(&desc);
    desc.bootstrap_recipes = orchestrator->bootstrap;
    desc.bootstrap_recipe_count = orchestrator->bootstrap_count;
    desc.policy_version = 1u;
    orchestrator->director = jce_ai_scene_director_create(&desc);
    if (!orchestrator->director) {
        jce_free(orchestrator);
        return NULL;
    }
    return orchestrator;
}

void es_scene_orchestrator_destroy(EsSceneOrchestrator *orchestrator)
{
    if (!orchestrator)
        return;
    jce_ai_scene_director_destroy(orchestrator->director);
    jce_free(orchestrator);
}

bool es_scene_orchestrator_request_next(
    EsSceneOrchestrator *orchestrator)
{
    JceAiSceneRequest request;

    if (!orchestrator || orchestrator->pending)
        return false;
    memset(&request, 0, sizeof(request));
    request.request_id = orchestrator->next_request_id++;
    request.seed = orchestrator->next_seed++;
    request.offline = true;
    jce_strlcpy(request.locale, "zh-CN", sizeof(request.locale));
    jce_strlcpy(
        request.intent,
        "advance the authored elemental diorama without repeating intent",
        sizeof(request.intent));
    if (!jce_ai_scene_director_request(orchestrator->director, &request,
                                       &orchestrator->catalog)) {
        LOG_WARN("es_ai", "scene request rejected: %d",
                 (int)jce_ai_scene_director_error(orchestrator->director));
        return false;
    }
    orchestrator->result_status = 0;
    orchestrator->result_error = 0;
    orchestrator->pending = true;
    return true;
}

void es_scene_orchestrator_update(EsSceneOrchestrator *orchestrator,
                                  double delta_seconds)
{
    JceAiSceneDirectorStatus status;
    JceSceneFrozenPlan plan;
    const JceScenePlanOperation *root;
    const JceScenePlanOperation *primary;
    int state;
    if (!orchestrator || !orchestrator->pending) return;
    jce_ai_scene_director_update(orchestrator->director, delta_seconds);
    status = jce_ai_scene_director_status(orchestrator->director);
    if (status == JCE_AI_SCENE_DIRECTOR_WAITING) return;
    orchestrator->pending = false;
    orchestrator->result_status = -1;
    orchestrator->result_error = (int)jce_ai_scene_director_error(orchestrator->director);
    if (status != JCE_AI_SCENE_DIRECTOR_READY ||
        !jce_ai_scene_director_result(orchestrator->director, &plan) ||
        plan.operation_count != 2u || !jce_scene_frozen_plan_graph_validate(&plan)) {
        LOG_WARN("es_ai", "planning failed: stage=result error=%d", orchestrator->result_error);
        return;
    }
    root = find_plan_role(&plan, "environment.root");
    primary = find_plan_role(&plan, "environment.primary");
    if (!root || !primary || root->parent_stable_entity_id != 0u ||
        primary->parent_stable_entity_id != root->stable_entity_id) {
        orchestrator->result_error = -2;
        LOG_ERROR("es_ai", "%s", "planning failed: stage=hierarchy");
        return;
    }
    state = state_for_asset(primary->asset_id);
    if (state < 0) {
        orchestrator->result_error = -3;
        LOG_ERROR("es_ai", "planning failed: stage=semantic asset=%s", primary->asset_id);
        return;
    }
    orchestrator->last_plan_hash = plan.plan_hash;
    orchestrator->result_state = state;
    orchestrator->result_status = 1;
    LOG_INFO("es_ai", "planned (not yet applied) plan=%llu source=%d asset=%s",
             (unsigned long long)plan.plan_hash,
             (int)jce_ai_scene_director_source(orchestrator->director), primary->asset_id);
}

int es_scene_orchestrator_take_result(EsSceneOrchestrator *orchestrator,
                                     int *state, int *error)
{
    int result;
    if (!orchestrator) return -1;
    result = orchestrator->result_status;
    if (state) *state = orchestrator->result_state;
    if (error) *error = orchestrator->result_error;
    orchestrator->result_status = 0;
    return result;
}

uint64_t es_scene_orchestrator_last_plan_hash(
    const EsSceneOrchestrator *orchestrator)
{
    return orchestrator ? orchestrator->last_plan_hash : 0u;
}
