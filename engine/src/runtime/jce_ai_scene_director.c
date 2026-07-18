/* Provider-neutral AI scene request, validation, cache, and fallback state. */

#include <jce/runtime/jce_ai_scene_director.h>

#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_json.h>

#include <limits.h>
#include <string.h>

#define JCE_AI_REQUEST_CONTRACT "jce.scene.recipe.request"
#define JCE_AI_RESPONSE_CONTRACT "jce.scene.recipe.response"
#define JCE_AI_FNV64_OFFSET 14695981039346656037ULL
#define JCE_AI_FNV64_PRIME 1099511628211ULL

struct JceAiSceneDirector {
    JceAiSceneDirectorDesc desc;
    JceAiSceneDirectorStatus status;
    JceAiScenePlanSource source;
    JceAiSceneError error;
    JceAiSceneRequest request;
    JceSceneCatalog catalog;
    JceAiSceneCacheKey cache_key;
    JceAiSceneTransportTicket ticket;
    uint32_t attempt_count;
    double elapsed_seconds;
    JceSceneFrozenPlan result;
    char prompt[JCE_AI_SCENE_PROMPT_MAX_BYTES];
    char response[JCE_AI_SCENE_RESPONSE_MAX_BYTES];
    uint8_t wire[JCE_SCENE_FROZEN_PLAN_MAX_BYTES];
};

static size_t bounded_length(const char *text, size_t capacity)
{
    size_t i;

    if (!text)
        return capacity;
    for (i = 0u; i < capacity; ++i) {
        if (text[i] == '\0')
            return i;
    }
    return capacity;
}

static bool request_strings_valid(const JceAiSceneRequest *request)
{
    size_t locale_length = bounded_length(request->locale,
                                          sizeof(request->locale));
    size_t intent_length = bounded_length(request->intent,
                                          sizeof(request->intent));
    size_t i;

    if (locale_length == 0u || locale_length >= sizeof(request->locale) ||
        intent_length == 0u || intent_length >= sizeof(request->intent))
        return false;
    for (i = 0u; i < locale_length; ++i) {
        char c = request->locale[i];
        if (!((c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
              (c >= '0' && c <= '9') || c == '-'))
            return false;
    }
    return true;
}

static uint64_t hash_bytes(uint64_t hash, const void *data, size_t size)
{
    const uint8_t *bytes = (const uint8_t *)data;
    size_t i;

    for (i = 0u; i < size; ++i) {
        hash ^= bytes[i];
        hash *= JCE_AI_FNV64_PRIME;
    }
    return hash;
}

static uint64_t hash_u32(uint64_t hash, uint32_t value)
{
    uint8_t bytes[4];
    uint32_t i;
    for (i = 0u; i < 4u; ++i)
        bytes[i] = (uint8_t)((value >> (8u * i)) & 0xffu);
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_u64(uint64_t hash, uint64_t value)
{
    uint8_t bytes[8];
    uint32_t i;
    for (i = 0u; i < 8u; ++i)
        bytes[i] = (uint8_t)((value >> (8u * i)) & 0xffu);
    return hash_bytes(hash, bytes, sizeof(bytes));
}

static uint64_t hash_text(uint64_t hash, const char *text, size_t capacity)
{
    size_t length = bounded_length(text, capacity);
    hash = hash_u32(hash, (uint32_t)length);
    return hash_bytes(hash, text, length);
}

static void build_cache_key(JceAiSceneDirector *director)
{
    uint64_t hash = JCE_AI_FNV64_OFFSET;

    hash = hash_u32(hash, director->desc.policy_version);
    hash = hash_u32(hash, JCE_SCENE_COMPILER_VERSION);
    hash = hash_u64(hash, director->request.request_id);
    hash = hash_u64(hash, director->request.seed);
    hash = hash_u64(hash, director->catalog.content_hash);
    hash = hash_text(hash, director->request.locale,
                     sizeof(director->request.locale));
    hash = hash_text(hash, director->request.intent,
                     sizeof(director->request.intent));
    director->cache_key.value = hash ? hash : 1u;
    director->cache_key.catalog_hash = director->catalog.content_hash;
    director->cache_key.seed = director->request.seed;
    director->cache_key.policy_version = director->desc.policy_version;
    director->cache_key.compiler_version = JCE_SCENE_COMPILER_VERSION;
}

static void u64_decimal(uint64_t value, char out[21])
{
    char reversed[20];
    size_t count = 0u;
    size_t i;

    do {
        reversed[count++] = (char)('0' + value % 10u);
        value /= 10u;
    } while (value != 0u && count < sizeof(reversed));
    for (i = 0u; i < count; ++i)
        out[i] = reversed[count - i - 1u];
    out[count] = '\0';
}

static void sort_catalog_indices(const JceSceneCatalog *catalog,
                                 uint32_t *indices)
{
    uint32_t i;

    for (i = 0u; i < catalog->entry_count; ++i) {
        uint32_t j = i;
        indices[i] = i;
        while (j > 0u) {
            const JceSceneCatalogEntry *left =
                &catalog->entries[indices[j - 1u]];
            const JceSceneCatalogEntry *right =
                &catalog->entries[indices[j]];
            int order = strcmp(left->capability, right->capability);
            uint32_t swap;

            if (order == 0)
                order = strcmp(left->asset_id, right->asset_id);
            if (order <= 0)
                break;
            swap = indices[j - 1u];
            indices[j - 1u] = indices[j];
            indices[j] = swap;
            --j;
        }
    }
}

static bool build_prompt(JceAiSceneDirector *director)
{
    JceJson *root = jce_json_object();
    JceJson *constraints = jce_json_object();
    JceJson *capabilities = jce_json_array();
    char request_id[21];
    char seed[21];
    char catalog_hash[21];
    char *printed;
    size_t length;
    uint32_t catalog_order[JCE_SCENE_CATALOG_MAX_ENTRIES];
    uint32_t i;

    if (!root || !constraints || !capabilities) {
        jce_json_free(root);
        jce_json_free(constraints);
        jce_json_free(capabilities);
        return false;
    }
    u64_decimal(director->request.request_id, request_id);
    u64_decimal(director->request.seed, seed);
    u64_decimal(director->catalog.content_hash, catalog_hash);
    jce_json_set_string(root, "contract", JCE_AI_REQUEST_CONTRACT);
    jce_json_set_int(root, "schemaVersion",
                     (int)JCE_AI_SCENE_RESPONSE_SCHEMA_VERSION);
    jce_json_set_int(root, "policyVersion",
                     (int)director->desc.policy_version);
    jce_json_set_string(root, "requestId", request_id);
    jce_json_set_string(root, "catalogHash", catalog_hash);
    jce_json_set_string(root, "seed", seed);
    jce_json_set_string(root, "locale", director->request.locale);
    jce_json_set_string(root, "intent", director->request.intent);

    jce_json_set_int(constraints, "maxOperations",
                     (int)JCE_SCENE_FROZEN_PLAN_MAX_OPERATIONS);
    jce_json_set_int(constraints, "recipeSchemaVersion",
                     (int)JCE_SCENE_RECIPE_SCHEMA_VERSION);
    jce_json_set_int(constraints, "compilerVersion",
                     (int)JCE_SCENE_COMPILER_VERSION);
    jce_json_set_string(constraints, "hierarchyMode",
                        "stableRoleInstance");
    jce_json_set_bool(constraints, "deterministic", true);
    jce_json_set_bool(constraints, "allowScripts", false);
    jce_json_set_bool(constraints, "allowAssetPaths", false);
    jce_json_set_child(root, "constraints", constraints);
    constraints = NULL;

    sort_catalog_indices(&director->catalog, catalog_order);
    for (i = 0u; i < director->catalog.entry_count; ++i) {
        const JceSceneCatalogEntry *entry =
            &director->catalog.entries[catalog_order[i]];
        JceJson *item;
        char content_hash[21];

        if (!entry->enabled)
            continue;
        item = jce_json_object();
        if (!item) {
            jce_json_free(root);
            jce_json_free(capabilities);
            return false;
        }
        u64_decimal(entry->content_hash, content_hash);
        jce_json_set_string(item, "assetId", entry->asset_id);
        jce_json_set_string(item, "capability", entry->capability);
        jce_json_set_string(item, "contentHash", content_hash);
        jce_json_set_int(item, "weight", (int)entry->weight);
        jce_json_array_push(capabilities, item);
    }
    jce_json_set_child(root, "capabilities", capabilities);
    capabilities = NULL;

    printed = jce_json_print(root, false);
    jce_json_free(root);
    if (!printed)
        return false;
    length = strlen(printed);
    if (length + 1u > sizeof(director->prompt)) {
        jce_json_free_string(printed);
        return false;
    }
    memcpy(director->prompt, printed, length + 1u);
    jce_json_free_string(printed);
    return true;
}

static bool submit_current(JceAiSceneDirector *director)
{
    JceAiSceneTransportRequest request;
    JceAiSceneTransportTicket ticket = 0u;

    if (!director->desc.transport.submit || !director->desc.transport.poll)
        return false;
    request.request_id = director->request.request_id;
    request.body = director->prompt;
    request.body_size = strlen(director->prompt);
    if (!director->desc.transport.submit(director->desc.transport.user,
                                         &request, &ticket) || ticket == 0u)
        return false;
    director->ticket = ticket;
    ++director->attempt_count;
    director->elapsed_seconds = 0.0;
    director->status = JCE_AI_SCENE_DIRECTOR_WAITING;
    director->error = JCE_AI_SCENE_ERROR_NONE;
    return true;
}

static bool plan_matches_request(const JceAiSceneDirector *director,
                                 const JceSceneFrozenPlan *plan)
{
    return plan->format_version == JCE_SCENE_FROZEN_PLAN_FORMAT_VERSION &&
           plan->compiler_version == JCE_SCENE_COMPILER_VERSION &&
           plan->request_id == director->request.request_id &&
           plan->seed == director->request.seed &&
           plan->catalog_hash == director->catalog.content_hash;
}

static void cache_store_result(JceAiSceneDirector *director)
{
    size_t size = 0u;

    if (!director->desc.cache.store)
        return;
    if (!jce_scene_frozen_plan_write(&director->result, director->wire,
                                     sizeof(director->wire), &size))
        return;
    director->desc.cache.store(director->desc.cache.user,
                               &director->cache_key,
                               director->wire, size);
}

static bool try_cache(JceAiSceneDirector *director)
{
    size_t size = 0u;
    JceSceneFrozenPlan plan;

    if (!director->desc.cache.load)
        return false;
    if (!director->desc.cache.load(director->desc.cache.user,
                                   &director->cache_key,
                                   director->wire, sizeof(director->wire),
                                   &size))
        return false;
    if (!jce_scene_frozen_plan_read(director->wire, size, &plan) ||
        !plan_matches_request(director, &plan))
        return false;
    director->result = plan;
    director->status = JCE_AI_SCENE_DIRECTOR_READY;
    director->source = JCE_AI_SCENE_SOURCE_CACHE;
    director->error = JCE_AI_SCENE_ERROR_NONE;
    return true;
}

static bool try_bootstrap(JceAiSceneDirector *director)
{
    uint32_t index;
    JceSceneRecipe recipe;
    JceSceneCompileError error;

    if (!director->desc.bootstrap_recipes ||
        director->desc.bootstrap_recipe_count == 0u)
        return false;
    index = (uint32_t)((director->request.seed ^
                        director->catalog.content_hash ^
                        director->desc.policy_version) %
                       director->desc.bootstrap_recipe_count);
    recipe = director->desc.bootstrap_recipes[index];
    recipe.schema_version = JCE_SCENE_RECIPE_SCHEMA_VERSION;
    recipe.compiler_version = JCE_SCENE_COMPILER_VERSION;
    recipe.request_id = director->request.request_id;
    recipe.seed = director->request.seed;
    if (jce_scene_compile(&recipe, &director->catalog, NULL,
                          &director->result, &error) != JCE_SCENE_COMPILE_OK)
        return false;
    director->status = JCE_AI_SCENE_DIRECTOR_READY;
    director->source = JCE_AI_SCENE_SOURCE_BOOTSTRAP;
    director->error = JCE_AI_SCENE_ERROR_NONE;
    cache_store_result(director);
    return true;
}

static bool key_allowed(const char *key, const char *const *allowed,
                        size_t count)
{
    size_t i;
    if (!key)
        return false;
    for (i = 0u; i < count; ++i) {
        if (strcmp(key, allowed[i]) == 0)
            return true;
    }
    return false;
}

static bool response_fields_strict(const JceJson *root)
{
    static const char *const allowed[] = {
        "contract", "schemaVersion", "policyVersion", "requestId",
        "catalogHash", "recipe"
    };
    JceJson *child;

    for (child = jce_json_first_child(root); child;
         child = jce_json_next_sibling(child)) {
        if (!key_allowed(jce_json_member_key(child), allowed,
                         sizeof(allowed) / sizeof(allowed[0])))
            return false;
    }
    return true;
}

static bool parse_u64_text(const char *text, uint64_t *out)
{
    uint64_t value = 0u;
    size_t i = 0u;
    bool any = false;

    if (!text || !out)
        return false;
    for (; text[i] != '\0'; ++i) {
        uint32_t digit;
        if (text[i] < '0' || text[i] > '9')
            return false;
        digit = (uint32_t)(text[i] - '0');
        if (value > (UINT64_MAX - digit) / 10u)
            return false;
        value = value * 10u + digit;
        any = true;
    }
    if (!any)
        return false;
    *out = value;
    return true;
}

static bool json_u64(const JceJson *root, const char *key, uint64_t *out)
{
    JceJson *node = jce_json_get(root, key);

    if (!node || !jce_json_is_string(node))
        return false;
    return parse_u64_text(jce_json_string_value(node, NULL), out);
}

static bool consume_response(JceAiSceneDirector *director, size_t size)
{
    JceJson *root;
    JceJson *recipe_node;
    const char *contract;
    uint64_t request_id;
    uint64_t catalog_hash;
    char *recipe_json;
    JceSceneRecipe recipe;
    JceSceneRecipeError recipe_error;
    JceSceneCompileError compile_error;
    JceSceneRecipeStatus recipe_status;

    root = jce_json_parse(director->response, size);
    if (!root || !jce_json_is_object(root) ||
        !response_fields_strict(root)) {
        jce_json_free(root);
        director->error = JCE_AI_SCENE_ERROR_INVALID_RESPONSE;
        return false;
    }
    contract = jce_json_get_string(root, "contract", "");
    if (strcmp(contract, JCE_AI_RESPONSE_CONTRACT) != 0 ||
        jce_json_get_int(root, "schemaVersion", -1) !=
            (int)JCE_AI_SCENE_RESPONSE_SCHEMA_VERSION) {
        jce_json_free(root);
        director->error = JCE_AI_SCENE_ERROR_INVALID_RESPONSE;
        return false;
    }
    if (jce_json_get_int(root, "policyVersion", -1) !=
            (int)director->desc.policy_version ||
        !json_u64(root, "requestId", &request_id) ||
        !json_u64(root, "catalogHash", &catalog_hash) ||
        request_id != director->request.request_id ||
        catalog_hash != director->catalog.content_hash) {
        jce_json_free(root);
        director->error = JCE_AI_SCENE_ERROR_STALE_RESPONSE;
        return false;
    }
    recipe_node = jce_json_get(root, "recipe");
    if (!recipe_node || !jce_json_is_object(recipe_node)) {
        jce_json_free(root);
        director->error = JCE_AI_SCENE_ERROR_INVALID_RESPONSE;
        return false;
    }
    recipe_json = jce_json_print(recipe_node, false);
    jce_json_free(root);
    if (!recipe_json) {
        director->error = JCE_AI_SCENE_ERROR_INVALID_RESPONSE;
        return false;
    }
    recipe_status = jce_scene_recipe_parse_json(
        recipe_json, strlen(recipe_json), &recipe, &recipe_error);
    jce_json_free_string(recipe_json);
    if (recipe_status != JCE_SCENE_RECIPE_OK) {
        director->error = JCE_AI_SCENE_ERROR_INVALID_RECIPE;
        return false;
    }
    if (recipe.request_id != director->request.request_id ||
        recipe.seed != director->request.seed ||
        recipe.compiler_version != JCE_SCENE_COMPILER_VERSION) {
        director->error = JCE_AI_SCENE_ERROR_STALE_RESPONSE;
        return false;
    }
    if (jce_scene_compile(&recipe, &director->catalog, NULL,
                          &director->result,
                          &compile_error) != JCE_SCENE_COMPILE_OK) {
        director->error = JCE_AI_SCENE_ERROR_COMPILE;
        return false;
    }
    director->status = JCE_AI_SCENE_DIRECTOR_READY;
    director->source = JCE_AI_SCENE_SOURCE_NETWORK;
    director->error = JCE_AI_SCENE_ERROR_NONE;
    cache_store_result(director);
    return true;
}

static void exhaust_or_retry(JceAiSceneDirector *director,
                             JceAiSceneError terminal_error)
{
    if (director->attempt_count <= director->desc.max_retries &&
        submit_current(director))
        return;
    if (try_bootstrap(director))
        return;
    director->status = JCE_AI_SCENE_DIRECTOR_FAILED;
    director->source = JCE_AI_SCENE_SOURCE_NONE;
    director->error = terminal_error;
}

JCE_API void JCE_CALL
jce_ai_scene_director_desc_default(JceAiSceneDirectorDesc *desc)
{
    if (!desc)
        return;
    memset(desc, 0, sizeof(*desc));
    desc->policy_version = 1u;
    desc->max_retries = 2u;
    desc->request_timeout_seconds = 15.0;
}

JCE_API JceAiSceneDirector *JCE_CALL
jce_ai_scene_director_create(const JceAiSceneDirectorDesc *desc)
{
    JceAiSceneDirectorDesc effective;
    JceAiSceneDirector *director;

    jce_ai_scene_director_desc_default(&effective);
    if (desc)
        effective = *desc;
    if (effective.policy_version == 0u ||
        effective.policy_version > INT_MAX ||
        effective.request_timeout_seconds <= 0.0 ||
        effective.request_timeout_seconds != effective.request_timeout_seconds ||
        ((effective.transport.submit == NULL) !=
         (effective.transport.poll == NULL)) ||
        (effective.bootstrap_recipe_count > 0u &&
         !effective.bootstrap_recipes))
        return NULL;
    director = (JceAiSceneDirector *)jce_malloc(sizeof(*director));
    if (!director)
        return NULL;
    memset(director, 0, sizeof(*director));
    director->desc = effective;
    director->status = JCE_AI_SCENE_DIRECTOR_IDLE;
    return director;
}

JCE_API void JCE_CALL
jce_ai_scene_director_destroy(JceAiSceneDirector *director)
{
    if (!director)
        return;
    if (director->status == JCE_AI_SCENE_DIRECTOR_WAITING &&
        director->desc.transport.cancel)
        director->desc.transport.cancel(director->desc.transport.user,
                                        director->ticket);
    jce_free(director);
}

JCE_API bool JCE_CALL
jce_ai_scene_director_request(JceAiSceneDirector *director,
                              const JceAiSceneRequest *request,
                              const JceSceneCatalog *catalog)
{
    JceSceneRecipeError catalog_error;

    if (!director || !request || !catalog || !request_strings_valid(request)) {
        if (director) {
            director->status = JCE_AI_SCENE_DIRECTOR_FAILED;
            director->error = JCE_AI_SCENE_ERROR_INVALID_ARGUMENT;
        }
        return false;
    }
    if (jce_scene_catalog_validate(catalog, &catalog_error) !=
        JCE_SCENE_RECIPE_OK) {
        director->status = JCE_AI_SCENE_DIRECTOR_FAILED;
        director->error = JCE_AI_SCENE_ERROR_INVALID_CATALOG;
        return false;
    }
    if (director->status == JCE_AI_SCENE_DIRECTOR_WAITING &&
        director->desc.transport.cancel)
        director->desc.transport.cancel(director->desc.transport.user,
                                        director->ticket);

    director->request = *request;
    director->catalog = *catalog;
    director->status = JCE_AI_SCENE_DIRECTOR_IDLE;
    director->source = JCE_AI_SCENE_SOURCE_NONE;
    director->error = JCE_AI_SCENE_ERROR_NONE;
    director->ticket = 0u;
    director->attempt_count = 0u;
    director->elapsed_seconds = 0.0;
    memset(&director->result, 0, sizeof(director->result));
    build_cache_key(director);

    if (try_cache(director))
        return true;
    if (request->offline || !director->desc.transport.submit) {
        if (try_bootstrap(director))
            return true;
        director->status = JCE_AI_SCENE_DIRECTOR_FAILED;
        director->error = JCE_AI_SCENE_ERROR_NO_FALLBACK;
        return false;
    }
    if (!build_prompt(director)) {
        director->status = JCE_AI_SCENE_DIRECTOR_FAILED;
        director->error = JCE_AI_SCENE_ERROR_PROMPT_TOO_LARGE;
        return false;
    }
    if (!submit_current(director)) {
        exhaust_or_retry(director, JCE_AI_SCENE_ERROR_TRANSPORT);
        return director->status != JCE_AI_SCENE_DIRECTOR_FAILED;
    }
    return true;
}

JCE_API void JCE_CALL
jce_ai_scene_director_update(JceAiSceneDirector *director,
                             double delta_seconds)
{
    JceAiSceneTransportState transport_state;
    size_t response_size = 0u;

    if (!director || director->status != JCE_AI_SCENE_DIRECTOR_WAITING)
        return;
    if (delta_seconds > 0.0 && delta_seconds == delta_seconds)
        director->elapsed_seconds += delta_seconds;
    if (director->elapsed_seconds >= director->desc.request_timeout_seconds) {
        if (director->desc.transport.cancel)
            director->desc.transport.cancel(director->desc.transport.user,
                                            director->ticket);
        exhaust_or_retry(director, JCE_AI_SCENE_ERROR_TIMEOUT);
        return;
    }
    transport_state = director->desc.transport.poll(
        director->desc.transport.user, director->ticket,
        director->response, sizeof(director->response) - 1u,
        &response_size);
    if (transport_state == JCE_AI_SCENE_TRANSPORT_PENDING)
        return;
    if (transport_state == JCE_AI_SCENE_TRANSPORT_FAILED ||
        response_size >= sizeof(director->response)) {
        exhaust_or_retry(director, JCE_AI_SCENE_ERROR_TRANSPORT);
        return;
    }
    director->response[response_size] = '\0';
    if (!consume_response(director, response_size)) {
        JceAiSceneError response_error = director->error;

        if (response_error == JCE_AI_SCENE_ERROR_INVALID_RESPONSE) {
            exhaust_or_retry(director, response_error);
            return;
        }
        director->status = JCE_AI_SCENE_DIRECTOR_FAILED;
        director->source = JCE_AI_SCENE_SOURCE_NONE;
    }
}

JCE_API void JCE_CALL
jce_ai_scene_director_cancel(JceAiSceneDirector *director)
{
    if (!director)
        return;
    if (director->status == JCE_AI_SCENE_DIRECTOR_WAITING &&
        director->desc.transport.cancel)
        director->desc.transport.cancel(director->desc.transport.user,
                                        director->ticket);
    director->status = JCE_AI_SCENE_DIRECTOR_CANCELLED;
    director->source = JCE_AI_SCENE_SOURCE_NONE;
    director->error = JCE_AI_SCENE_ERROR_NONE;
}

JCE_API JceAiSceneDirectorStatus JCE_CALL
jce_ai_scene_director_status(const JceAiSceneDirector *director)
{
    return director ? director->status : JCE_AI_SCENE_DIRECTOR_FAILED;
}

JCE_API JceAiScenePlanSource JCE_CALL
jce_ai_scene_director_source(const JceAiSceneDirector *director)
{
    return director ? director->source : JCE_AI_SCENE_SOURCE_NONE;
}

JCE_API JceAiSceneError JCE_CALL
jce_ai_scene_director_error(const JceAiSceneDirector *director)
{
    return director ? director->error : JCE_AI_SCENE_ERROR_INVALID_ARGUMENT;
}

JCE_API bool JCE_CALL
jce_ai_scene_director_result(const JceAiSceneDirector *director,
                             JceSceneFrozenPlan *out_plan)
{
    if (!director || !out_plan ||
        director->status != JCE_AI_SCENE_DIRECTOR_READY)
        return false;
    *out_plan = director->result;
    return true;
}

JCE_API const JceAiSceneCacheKey *JCE_CALL
jce_ai_scene_director_cache_key(const JceAiSceneDirector *director)
{
    return director ? &director->cache_key : NULL;
}
