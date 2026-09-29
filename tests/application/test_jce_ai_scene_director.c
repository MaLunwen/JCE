/* Provider-neutral AI scene director contract and state-machine tests. */

#include <jce/api_runtime.h>

#include <string.h>

#include "unity.h"

void setUp(void) {}
void tearDown(void) {}

typedef struct {
    uint32_t submit_count;
    uint32_t poll_count;
    uint64_t request_ids[4];
    bool fail_first_poll;
    bool invalid_first_response;
    char response[JCE_AI_SCENE_RESPONSE_MAX_BYTES];
    char first_body[JCE_AI_SCENE_PROMPT_MAX_BYTES];
    char last_body[JCE_AI_SCENE_PROMPT_MAX_BYTES];
} FakeTransport;

typedef struct {
    bool has_value;
    uint32_t load_count;
    uint32_t store_count;
    uint8_t bytes[JCE_SCENE_FROZEN_PLAN_MAX_BYTES];
    size_t size;
} FakeCache;

static bool JCE_CALL fake_submit(void *user,
                                 const JceAiSceneTransportRequest *request,
                                 JceAiSceneTransportTicket *out_ticket)
{
    FakeTransport *transport = (FakeTransport *)user;
    size_t size = request->body_size;

    if (transport->submit_count >= 4u ||
        size + 1u > sizeof(transport->last_body))
        return false;
    transport->request_ids[transport->submit_count] = request->request_id;
    if (transport->submit_count == 0u) {
        memcpy(transport->first_body, request->body, size);
        transport->first_body[size] = '\0';
    }
    ++transport->submit_count;
    memcpy(transport->last_body, request->body, size);
    transport->last_body[size] = '\0';
    *out_ticket = transport->submit_count;
    return true;
}

static JceAiSceneTransportState JCE_CALL
fake_poll(void *user, JceAiSceneTransportTicket ticket,
          char *response, size_t capacity, size_t *out_size)
{
    FakeTransport *transport = (FakeTransport *)user;
    size_t size;

    ++transport->poll_count;
    if (transport->fail_first_poll && ticket == 1u)
        return JCE_AI_SCENE_TRANSPORT_FAILED;
    if (transport->invalid_first_response && ticket == 1u) {
        static const char invalid[] = "{\"unexpected\":true}";
        size = sizeof(invalid) - 1u;
        if (size > capacity)
            return JCE_AI_SCENE_TRANSPORT_FAILED;
        memcpy(response, invalid, size);
        *out_size = size;
        return JCE_AI_SCENE_TRANSPORT_COMPLETE;
    }
    size = strlen(transport->response);
    if (size > capacity)
        return JCE_AI_SCENE_TRANSPORT_FAILED;
    memcpy(response, transport->response, size);
    *out_size = size;
    return JCE_AI_SCENE_TRANSPORT_COMPLETE;
}

static void JCE_CALL fake_cancel(void *user,
                                 JceAiSceneTransportTicket ticket)
{
    (void)user;
    (void)ticket;
}

static bool JCE_CALL fake_cache_load(void *user,
                                     const JceAiSceneCacheKey *key,
                                     void *buffer, size_t capacity,
                                     size_t *out_size)
{
    FakeCache *cache = (FakeCache *)user;
    (void)key;
    ++cache->load_count;
    if (!cache->has_value || cache->size > capacity)
        return false;
    memcpy(buffer, cache->bytes, cache->size);
    *out_size = cache->size;
    return true;
}

static bool JCE_CALL fake_cache_store(void *user,
                                      const JceAiSceneCacheKey *key,
                                      const void *buffer, size_t size)
{
    FakeCache *cache = (FakeCache *)user;
    (void)key;
    ++cache->store_count;
    if (size > sizeof(cache->bytes))
        return false;
    memcpy(cache->bytes, buffer, size);
    cache->size = size;
    cache->has_value = true;
    return true;
}

static JceSceneCatalog make_catalog(void)
{
    JceSceneCatalog catalog;

    jce_scene_catalog_init(&catalog);
    catalog.content_hash = 273u;
    catalog.entry_count = 1u;
    strcpy(catalog.entries[0].asset_id, "elemental.grass.cluster");
    strcpy(catalog.entries[0].capability, "nature.grass");
    catalog.entries[0].content_hash = 9001u;
    catalog.entries[0].weight = 1u;
    catalog.entries[0].enabled = true;
    return catalog;
}

static JceSceneRecipe make_recipe(uint64_t request_id, uint64_t seed)
{
    JceSceneRecipe recipe;

    jce_scene_recipe_init(&recipe);
    recipe.request_id = request_id;
    recipe.seed = seed;
    recipe.role_count = 1u;
    strcpy(recipe.roles[0].stable_role, "meadow_grass");
    strcpy(recipe.roles[0].capability, "nature.grass");
    recipe.roles[0].min_count = 2u;
    recipe.roles[0].max_count = 2u;
    recipe.roles[0].placement = JCE_SCENE_PLACEMENT_GRID;
    recipe.roles[0].required = true;
    recipe.roles[0].extent[0] = 4.0;
    recipe.roles[0].extent[2] = 4.0;
    recipe.roles[0].scale_min = 1.0;
    recipe.roles[0].scale_max = 1.0;
    return recipe;
}

static JceAiSceneDirectorDesc make_desc(FakeTransport *transport,
                                        FakeCache *cache)
{
    JceAiSceneDirectorDesc desc;

    jce_ai_scene_director_desc_default(&desc);
    if (transport) {
        desc.transport.user = transport;
        desc.transport.submit = fake_submit;
        desc.transport.poll = fake_poll;
        desc.transport.cancel = fake_cancel;
    }
    if (cache) {
        desc.cache.user = cache;
        desc.cache.load = fake_cache_load;
        desc.cache.store = fake_cache_store;
    }
    desc.policy_version = 3u;
    desc.max_retries = 1u;
    return desc;
}

static JceAiSceneRequest make_request(void)
{
    JceAiSceneRequest request;

    memset(&request, 0, sizeof(request));
    request.request_id = 50u;
    request.seed = 99u;
    strcpy(request.locale, "zh-CN");
    strcpy(request.intent, "calm meadow after user confirmation");
    return request;
}

static void set_valid_response(FakeTransport *transport)
{
    static const char response[] =
        "{\"contract\":\"jce.scene.recipe.response\","
        "\"schemaVersion\":1,\"policyVersion\":3,"
        "\"requestId\":\"50\",\"catalogHash\":\"273\","
        "\"recipe\":{\"schemaVersion\":2,\"compilerVersion\":2,"
        "\"requestId\":\"50\",\"seed\":\"99\",\"roles\":[{"
        "\"stableRole\":\"meadow_grass\","
        "\"capability\":\"nature.grass\",\"minCount\":2,"
        "\"maxCount\":2,\"placement\":\"grid\","
        "\"required\":true,\"extent\":[4,0,4],"
        "\"scale\":[1,1]}]}}";
    strcpy(transport->response, response);
}

static void test_prompt_exposes_semantics_not_host_paths(void)
{
    FakeTransport transport = {0};
    JceAiSceneDirectorDesc desc = make_desc(&transport, NULL);
    JceAiSceneDirector *director = jce_ai_scene_director_create(&desc);
    JceSceneCatalog catalog = make_catalog();
    JceAiSceneRequest request = make_request();

    TEST_ASSERT_NOT_NULL(director);
    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    TEST_ASSERT_EQUAL_UINT32(1u, transport.submit_count);
    TEST_ASSERT_NOT_NULL(strstr(transport.last_body,
                                "elemental.grass.cluster"));
    TEST_ASSERT_NULL(strstr(transport.last_body, "C:\\"));
    TEST_ASSERT_NULL(strstr(transport.last_body, "D:\\"));
    TEST_ASSERT_NULL(strstr(transport.last_body, "../"));
    TEST_ASSERT_NOT_NULL(strstr(transport.last_body,
                                "\"recipeSchemaVersion\":2"));
    TEST_ASSERT_NOT_NULL(strstr(transport.last_body,
                                "\"compilerVersion\":2"));
    TEST_ASSERT_NOT_NULL(strstr(transport.last_body,
                                "\"hierarchyMode\":\"stableRoleInstance\""));
    jce_ai_scene_director_destroy(director);
}

static void test_prompt_is_canonical_for_catalog_input_order(void)
{
    FakeTransport first_transport = {0};
    FakeTransport second_transport = {0};
    JceAiSceneDirectorDesc first_desc = make_desc(&first_transport, NULL);
    JceAiSceneDirectorDesc second_desc = make_desc(&second_transport, NULL);
    JceAiSceneDirector *first = jce_ai_scene_director_create(&first_desc);
    JceAiSceneDirector *second = jce_ai_scene_director_create(&second_desc);
    JceSceneCatalog ordered = make_catalog();
    JceSceneCatalog reversed;
    JceSceneCatalogEntry swap;
    JceAiSceneRequest request = make_request();

    ordered.entry_count = 3u;
    strcpy(ordered.entries[1].asset_id, "elemental.environment.autumn");
    strcpy(ordered.entries[1].capability, "environment.autumn");
    ordered.entries[1].content_hash = 9002u;
    ordered.entries[1].weight = 1u;
    ordered.entries[1].enabled = true;
    strcpy(ordered.entries[2].asset_id, "elemental.environment.winter");
    strcpy(ordered.entries[2].capability, "environment.winter");
    ordered.entries[2].content_hash = 9003u;
    ordered.entries[2].weight = 1u;
    ordered.entries[2].enabled = true;
    reversed = ordered;
    swap = reversed.entries[0];
    reversed.entries[0] = reversed.entries[2];
    reversed.entries[2] = swap;

    TEST_ASSERT_TRUE(jce_ai_scene_director_request(first, &request,
                                                    &ordered));
    TEST_ASSERT_TRUE(jce_ai_scene_director_request(second, &request,
                                                    &reversed));
    TEST_ASSERT_EQUAL_STRING(first_transport.last_body,
                             second_transport.last_body);
    jce_ai_scene_director_destroy(first);
    jce_ai_scene_director_destroy(second);
}

static void test_retry_keeps_request_identity_and_seed(void)
{
    FakeTransport transport = {0};
    FakeCache cache = {0};
    JceAiSceneDirectorDesc desc;
    JceAiSceneDirector *director;
    JceSceneCatalog catalog = make_catalog();
    JceAiSceneRequest request = make_request();
    JceSceneFrozenPlan plan;

    transport.fail_first_poll = true;
    set_valid_response(&transport);
    desc = make_desc(&transport, &cache);
    director = jce_ai_scene_director_create(&desc);

    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    jce_ai_scene_director_update(director, 0.01);
    TEST_ASSERT_EQUAL_UINT32(2u, transport.submit_count);
    TEST_ASSERT_EQUAL_UINT64(transport.request_ids[0],
                             transport.request_ids[1]);
    jce_ai_scene_director_update(director, 0.01);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_DIRECTOR_READY,
                          jce_ai_scene_director_status(director));
    TEST_ASSERT_TRUE(jce_ai_scene_director_result(director, &plan));
    TEST_ASSERT_EQUAL_UINT64(99u, plan.seed);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_SOURCE_NETWORK,
                          jce_ai_scene_director_source(director));
    TEST_ASSERT_EQUAL_UINT32(1u, cache.store_count);
    jce_ai_scene_director_destroy(director);
}

static void test_invalid_response_gets_one_bounded_retry(void)
{
    FakeTransport transport = {0};
    JceAiSceneDirectorDesc desc;
    JceAiSceneDirector *director;
    JceSceneCatalog catalog = make_catalog();
    JceAiSceneRequest request = make_request();

    transport.invalid_first_response = true;
    set_valid_response(&transport);
    desc = make_desc(&transport, NULL);
    desc.max_retries = 1u;
    director = jce_ai_scene_director_create(&desc);

    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    jce_ai_scene_director_update(director, 0.01);
    TEST_ASSERT_EQUAL_UINT32(2u, transport.submit_count);
    TEST_ASSERT_EQUAL_STRING(transport.first_body, transport.last_body);
    jce_ai_scene_director_update(director, 0.01);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_DIRECTOR_READY,
                          jce_ai_scene_director_status(director));
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_SOURCE_NETWORK,
                          jce_ai_scene_director_source(director));
    jce_ai_scene_director_destroy(director);
}

static void test_cache_hit_bypasses_transport(void)
{
    FakeTransport transport = {0};
    FakeCache cache = {0};
    JceSceneCatalog catalog = make_catalog();
    JceSceneRecipe recipe = make_recipe(50u, 99u);
    JceSceneFrozenPlan cached_plan;
    JceSceneCompileError error;
    JceAiSceneDirectorDesc desc;
    JceAiSceneDirector *director;
    JceAiSceneRequest request = make_request();
    JceSceneFrozenPlan result;

    TEST_ASSERT_EQUAL_INT(JCE_SCENE_COMPILE_OK,
                          jce_scene_compile(&recipe, &catalog, NULL,
                                            &cached_plan, &error));
    TEST_ASSERT_TRUE(jce_scene_frozen_plan_write(&cached_plan, cache.bytes,
                                                 sizeof(cache.bytes),
                                                 &cache.size));
    cache.has_value = true;
    desc = make_desc(&transport, &cache);
    director = jce_ai_scene_director_create(&desc);

    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_DIRECTOR_READY,
                          jce_ai_scene_director_status(director));
    TEST_ASSERT_EQUAL_UINT32(1u, cache.load_count);
    TEST_ASSERT_EQUAL_UINT32(0u, transport.submit_count);
    TEST_ASSERT_TRUE(jce_ai_scene_director_result(director, &result));
    TEST_ASSERT_EQUAL_UINT64(cached_plan.plan_hash, result.plan_hash);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_SOURCE_CACHE,
                          jce_ai_scene_director_source(director));
    jce_ai_scene_director_destroy(director);
}

static void test_offline_mode_compiles_bootstrap_deterministically(void)
{
    JceSceneRecipe bootstrap = make_recipe(1u, 1u);
    JceSceneCatalog catalog = make_catalog();
    JceAiSceneDirectorDesc desc = make_desc(NULL, NULL);
    JceAiSceneDirector *director;
    JceAiSceneRequest request = make_request();
    JceSceneFrozenPlan first;
    JceSceneFrozenPlan second;

    desc.bootstrap_recipes = &bootstrap;
    desc.bootstrap_recipe_count = 1u;
    director = jce_ai_scene_director_create(&desc);
    request.offline = true;

    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    TEST_ASSERT_TRUE(jce_ai_scene_director_result(director, &first));
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_SOURCE_BOOTSTRAP,
                          jce_ai_scene_director_source(director));
    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    TEST_ASSERT_TRUE(jce_ai_scene_director_result(director, &second));
    TEST_ASSERT_EQUAL_UINT64(first.plan_hash, second.plan_hash);
    jce_ai_scene_director_destroy(director);
}

static void test_stale_catalog_response_is_rejected(void)
{
    FakeTransport transport = {0};
    JceAiSceneDirectorDesc desc = make_desc(&transport, NULL);
    JceAiSceneDirector *director;
    JceSceneCatalog catalog = make_catalog();
    JceAiSceneRequest request = make_request();

    set_valid_response(&transport);
    {
        char *hash = strstr(transport.response, "\"273\"");
        TEST_ASSERT_NOT_NULL(hash);
        memcpy(hash, "\"999\"", 5u);
    }
    director = jce_ai_scene_director_create(&desc);
    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    jce_ai_scene_director_update(director, 0.01);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_DIRECTOR_FAILED,
                          jce_ai_scene_director_status(director));
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_ERROR_STALE_RESPONSE,
                          jce_ai_scene_director_error(director));
    jce_ai_scene_director_destroy(director);
}

static void test_unapproved_recipe_field_is_rejected(void)
{
    FakeTransport transport = {0};
    JceAiSceneDirectorDesc desc = make_desc(&transport, NULL);
    JceAiSceneDirector *director;
    JceSceneCatalog catalog = make_catalog();
    JceAiSceneRequest request = make_request();
    static const char response[] =
        "{\"contract\":\"jce.scene.recipe.response\","
        "\"schemaVersion\":1,\"policyVersion\":3,"
        "\"requestId\":\"50\",\"catalogHash\":\"273\","
        "\"recipe\":{\"schemaVersion\":2,\"compilerVersion\":2,"
        "\"requestId\":\"50\",\"seed\":\"99\",\"roles\":[],"
        "\"script\":\"load_host_file()\"}}";

    strcpy(transport.response, response);
    director = jce_ai_scene_director_create(&desc);
    TEST_ASSERT_TRUE(jce_ai_scene_director_request(director, &request,
                                                    &catalog));
    jce_ai_scene_director_update(director, 0.01);
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_DIRECTOR_FAILED,
                          jce_ai_scene_director_status(director));
    TEST_ASSERT_EQUAL_INT(JCE_AI_SCENE_ERROR_INVALID_RECIPE,
                          jce_ai_scene_director_error(director));
    jce_ai_scene_director_destroy(director);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_prompt_exposes_semantics_not_host_paths);
    RUN_TEST(test_prompt_is_canonical_for_catalog_input_order);
    RUN_TEST(test_retry_keeps_request_identity_and_seed);
    RUN_TEST(test_invalid_response_gets_one_bounded_retry);
    RUN_TEST(test_cache_hit_bypasses_transport);
    RUN_TEST(test_offline_mode_compiles_bootstrap_deterministically);
    RUN_TEST(test_stale_catalog_response_is_rejected);
    RUN_TEST(test_unapproved_recipe_field_is_rejected);
    return UNITY_END();
}
