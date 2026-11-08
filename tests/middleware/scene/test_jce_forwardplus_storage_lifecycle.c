/* Actual scene Forward+ owner controls. No window, GPU or process sampler. */
#include "unity.h"
#include "middleware/scene/jce_sr_forwardplus_storage.h"
#include <string.h>

typedef struct {
    bool live;
    bool enabled;
} TestModule;

typedef struct {
    JceSrForwardPlusStorage *owner;
    TestModule module;
    JceLightProxy proxies[256];
    JceForwardPlusLightParam params[256];
    bool proxy_live;
    bool param_live;
    unsigned allocations, releases, creates, destroys, enabled_calls;
    unsigned fail_allocation, fail_create;
} Fixture;

static JceSrForwardPlusStorage owner;
static Fixture fixture;
static JceSrForwardPlusProvider provider;

static void assert_unpublished(Fixture *f)
{
    TEST_ASSERT_NULL(f->owner->module);
    TEST_ASSERT_NULL(f->owner->proxies);
    TEST_ASSERT_NULL(f->owner->params);
    TEST_ASSERT_EQUAL_UINT32(0, f->owner->capacity);
}

static void *allocate(size_t count, size_t size, void *user)
{
    Fixture *f = user;
    assert_unpublished(f);
    TEST_ASSERT_EQUAL_UINT32(256, count);
    if (++f->allocations == f->fail_allocation) return NULL;
    if (size == sizeof(JceLightProxy)) {
        TEST_ASSERT_FALSE(f->proxy_live);
        f->proxy_live = true;
        return f->proxies;
    }
    TEST_ASSERT_EQUAL_UINT32(sizeof(JceForwardPlusLightParam), size);
    TEST_ASSERT_FALSE(f->param_live);
    f->param_live = true;
    return f->params;
}

static void release(void *allocation, void *user)
{
    Fixture *f = user;
    if (allocation == f->proxies) {
        TEST_ASSERT_TRUE_MESSAGE(f->proxy_live, "Proxy array released exactly once");
        f->proxy_live = false;
    } else {
        TEST_ASSERT_EQUAL_PTR(f->params, allocation);
        TEST_ASSERT_TRUE_MESSAGE(f->param_live, "Param array released exactly once");
        f->param_live = false;
    }
    ++f->releases;
}

static JceForwardPlus *create(uint32_t x, uint32_t y, uint32_t z,
    uint32_t per_cell, uint32_t lights, void *user)
{
    Fixture *f = user;
    assert_unpublished(f);
    TEST_ASSERT_EQUAL_UINT32(24, x);
    TEST_ASSERT_EQUAL_UINT32(16, y);
    TEST_ASSERT_EQUAL_UINT32(24, z);
    TEST_ASSERT_EQUAL_UINT32(64, per_cell);
    TEST_ASSERT_EQUAL_UINT32(256, lights);
    TEST_ASSERT_TRUE(f->proxy_live);
    TEST_ASSERT_TRUE(f->param_live);
    if (++f->creates == f->fail_create) return NULL;
    TEST_ASSERT_FALSE(f->module.live);
    f->module.live = true;
    f->module.enabled = false;
    return (JceForwardPlus *)&f->module;
}

static void destroy(JceForwardPlus *module, void *user)
{
    Fixture *f = user;
    TEST_ASSERT_EQUAL_PTR(&f->module, module);
    TEST_ASSERT_TRUE_MESSAGE(f->module.live, "Module destroyed exactly once");
    f->module.live = f->module.enabled = false;
    ++f->destroys;
}

static void set_enabled(JceForwardPlus *module, bool enabled, void *user)
{
    Fixture *f = user;
    TEST_ASSERT_EQUAL_PTR(&f->module, module);
    TEST_ASSERT_TRUE(f->module.live);
    TEST_ASSERT_EQUAL_PTR(f->proxies, f->owner->proxies);
    TEST_ASSERT_EQUAL_PTR(f->params, f->owner->params);
    TEST_ASSERT_EQUAL_UINT32(256, f->owner->capacity);
    f->module.enabled = enabled;
    ++f->enabled_calls;
}

static JceSrForwardPlusProvider fixture_provider(Fixture *f)
{
    return (JceSrForwardPlusProvider){
        create, destroy, set_enabled, allocate, release, f
    };
}

void setUp(void)
{
    memset(&owner, 0, sizeof(owner));
    memset(&fixture, 0, sizeof(fixture));
    fixture.owner = &owner;
    provider = fixture_provider(&fixture);
}

void tearDown(void)
{
    sr_forwardplus_storage_destroy(&owner, &provider);
    TEST_ASSERT_FALSE(fixture.module.live);
    TEST_ASSERT_FALSE(fixture.proxy_live);
    TEST_ASSERT_FALSE(fixture.param_live);
}

static void test_initial_disabled_view_does_not_admit_any_payload(void)
{
    for (unsigned i = 0; i < 4; ++i)
        TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, false, &provider));
    assert_unpublished(&fixture);
    TEST_ASSERT_EQUAL_UINT(0, fixture.allocations);
    TEST_ASSERT_EQUAL_UINT(0, fixture.creates);
    TEST_ASSERT_EQUAL_UINT(0, fixture.enabled_calls);
}

static void test_first_enabled_view_has_complete_parallel_light_capacity(void)
{
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_TRUE(fixture.module.enabled);
    TEST_ASSERT_EQUAL_UINT(2, fixture.allocations);
    TEST_ASSERT_EQUAL_UINT(1, fixture.creates);
    /* Both inputs can be filled on the first requested view, including the
     * final supported light. This is CPU admission, not scene/GPU fidelity. */
    for (uint32_t i = 0; i < owner.capacity; ++i) {
        owner.proxies[i].user_id = i + 1u;
        owner.params[i].color.x = (float)(i + 1u);
    }
    TEST_ASSERT_EQUAL_UINT32(256, owner.proxies[255].user_id);
    TEST_ASSERT_EQUAL_FLOAT(256.0f, owner.params[255].color.x);
}

static void test_mode_edges_reuse_complete_owner_without_allocation_churn(void)
{
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    const JceSrForwardPlusStorage first = owner;
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, false, &provider));
    TEST_ASSERT_FALSE(fixture.module.enabled);
    TEST_ASSERT_EQUAL_PTR(first.module, owner.module);
    TEST_ASSERT_EQUAL_PTR(first.proxies, owner.proxies);
    TEST_ASSERT_EQUAL_PTR(first.params, owner.params);
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, false, &provider));
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_EQUAL_UINT(2, fixture.allocations);
    TEST_ASSERT_EQUAL_UINT(1, fixture.creates);
    TEST_ASSERT_EQUAL_UINT(0, fixture.releases);
    TEST_ASSERT_EQUAL_UINT(0, fixture.destroys);
}

static void test_proxy_allocation_failure_stays_off_and_retries(void)
{
    fixture.fail_allocation = 1;
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    assert_unpublished(&fixture);
    TEST_ASSERT_EQUAL_UINT(0, fixture.creates);
    TEST_ASSERT_EQUAL_UINT(0, fixture.enabled_calls);
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, false, &provider));
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_TRUE(fixture.module.enabled);
}

static void test_param_allocation_failure_rolls_back_before_collection_and_retries(void)
{
    fixture.fail_allocation = 2;
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    assert_unpublished(&fixture);
    TEST_ASSERT_FALSE(fixture.proxy_live);
    TEST_ASSERT_FALSE(fixture.param_live);
    TEST_ASSERT_EQUAL_UINT(1, fixture.releases);
    TEST_ASSERT_EQUAL_UINT(0, fixture.creates);
    TEST_ASSERT_EQUAL_UINT(0, fixture.enabled_calls);
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_TRUE(fixture.module.enabled);
}

static void test_module_failure_releases_both_arrays_and_retries(void)
{
    fixture.fail_create = 1;
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    assert_unpublished(&fixture);
    TEST_ASSERT_EQUAL_UINT(2, fixture.releases);
    TEST_ASSERT_FALSE(fixture.proxy_live);
    TEST_ASSERT_FALSE(fixture.param_live);
    TEST_ASSERT_EQUAL_UINT(0, fixture.enabled_calls);
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_EQUAL_UINT(2, fixture.creates);
    TEST_ASSERT_TRUE(fixture.module.enabled);
}

static void test_independent_renderers_and_sequential_views_keep_distinct_owners(void)
{
    JceSrForwardPlusStorage other = {0};
    Fixture other_fixture = {0};
    other_fixture.owner = &other;
    const JceSrForwardPlusProvider other_provider = fixture_provider(&other_fixture);
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&other, false, &other_provider));
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&other, true, &other_provider));
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    TEST_ASSERT_TRUE(owner.module != other.module);
    TEST_ASSERT_TRUE(owner.proxies != other.proxies);
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&other, false, &other_provider));
    TEST_ASSERT_TRUE(fixture.module.enabled);
    TEST_ASSERT_FALSE(other_fixture.module.enabled);
    TEST_ASSERT_EQUAL_UINT(1, fixture.creates);
    TEST_ASSERT_EQUAL_UINT(1, other_fixture.creates);
    sr_forwardplus_storage_destroy(&other, &other_provider);
    TEST_ASSERT_EQUAL_UINT(1, other_fixture.destroys);
    TEST_ASSERT_EQUAL_UINT(2, other_fixture.releases);
}

static void test_repeated_teardown_releases_each_owned_resource_once(void)
{
    sr_forwardplus_storage_destroy(&owner, &provider);
    TEST_ASSERT_EQUAL_UINT(0, fixture.destroys);
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&owner, true, &provider));
    sr_forwardplus_storage_destroy(&owner, &provider);
    sr_forwardplus_storage_destroy(&owner, &provider);
    assert_unpublished(&fixture);
    TEST_ASSERT_EQUAL_UINT(1, fixture.destroys);
    TEST_ASSERT_EQUAL_UINT(2, fixture.releases);
}

static void test_production_provider_uses_real_cpu_module_and_keeps_public_toggle(void)
{
    JceSrForwardPlusStorage actual = {0};
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&actual, false, NULL));
    TEST_ASSERT_NULL(actual.module);
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&actual, true, NULL));
    TEST_ASSERT_NOT_NULL(actual.proxies);
    TEST_ASSERT_NOT_NULL(actual.params);
    TEST_ASSERT_TRUE(jce_forwardplus_is_enabled(actual.module));
    JceForwardPlus *first = actual.module;
    TEST_ASSERT_FALSE(sr_forwardplus_storage_prepare(&actual, false, NULL));
    TEST_ASSERT_FALSE(jce_forwardplus_is_enabled(actual.module));
    TEST_ASSERT_TRUE(sr_forwardplus_storage_prepare(&actual, true, NULL));
    TEST_ASSERT_EQUAL_PTR(first, actual.module);
    sr_forwardplus_storage_destroy(&actual, NULL);
    TEST_ASSERT_NULL(actual.module);
    TEST_ASSERT_NULL(actual.proxies);
    TEST_ASSERT_NULL(actual.params);
    TEST_ASSERT_EQUAL_UINT32(0, actual.capacity);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_initial_disabled_view_does_not_admit_any_payload);
    RUN_TEST(test_first_enabled_view_has_complete_parallel_light_capacity);
    RUN_TEST(test_mode_edges_reuse_complete_owner_without_allocation_churn);
    RUN_TEST(test_proxy_allocation_failure_stays_off_and_retries);
    RUN_TEST(test_param_allocation_failure_rolls_back_before_collection_and_retries);
    RUN_TEST(test_module_failure_releases_both_arrays_and_retries);
    RUN_TEST(test_independent_renderers_and_sequential_views_keep_distinct_owners);
    RUN_TEST(test_repeated_teardown_releases_each_owned_resource_once);
    RUN_TEST(test_production_provider_uses_real_cpu_module_and_keeps_public_toggle);
    return UNITY_END();
}
