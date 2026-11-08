/* Real private roots-buffer owner controls; no GPU or upstream test types. */
#include "unity.h"
#include "middleware/scene/jce_sr_foliage_buffer.h"
#include <string.h>

#define HANDLE_CAP 24u
static bool live[HANDLE_CAP], uploaded[HANDLE_CAP];
static uint16_t handle, next_handle;
static uint32_t resident_hash, resident_count;
static bool resident_compute;
static unsigned creates, uploads, destroys, fail_create, fail_upload;
static uint32_t created_count;
static bool created_compute;
static unsigned char roots[64];

static uint16_t create_buffer(uint32_t count, bool compute_read, void *user)
{
    TEST_ASSERT_EQUAL_PTR(roots, user);
    created_count = count;
    created_compute = compute_read;
    if (++creates == fail_create)
        return UINT16_MAX;
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, next_handle);
    live[next_handle] = true;
    return next_handle++;
}
static bool upload_buffer(uint16_t buffer, const void *data, uint32_t count, void *user)
{
    TEST_ASSERT_EQUAL_PTR(roots, user);
    TEST_ASSERT_EQUAL_PTR(roots, data);
    TEST_ASSERT_EQUAL_UINT32(created_count, count);
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, buffer);
    TEST_ASSERT_TRUE(live[buffer]);
    if (++uploads == fail_upload)
        return false;
    uploaded[buffer] = true;
    return true;
}
static void destroy_buffer(uint16_t buffer, void *user)
{
    TEST_ASSERT_EQUAL_PTR(roots, user);
    TEST_ASSERT_LESS_THAN(HANDLE_CAP, buffer);
    TEST_ASSERT_TRUE_MESSAGE(live[buffer], "Each buffer must be destroyed exactly once");
    live[buffer] = false;
    ++destroys;
}
static const JceSrFoliageBufferProvider provider = {
    create_buffer, upload_buffer, destroy_buffer, roots
};
static bool ensure(uint32_t count, uint32_t hash, bool compute_read)
{
    return sr_foliage_buffer_ensure(&handle, &resident_hash, &resident_count,
        &resident_compute, roots, count, hash, compute_read, &provider);
}
static void destroy(void)
{
    sr_foliage_buffer_destroy(&handle, &resident_hash, &resident_count,
        &resident_compute, &provider);
}
static unsigned live_count(void)
{
    unsigned count = 0;
    for (unsigned i = 0; i < HANDLE_CAP; ++i)
        if (live[i]) ++count;
    return count;
}
void setUp(void)
{
    memset(live, 0, sizeof(live));
    memset(uploaded, 0, sizeof(uploaded));
    handle = next_handle = 0;
    resident_hash = resident_count = 0;
    resident_compute = false;
    creates = uploads = destroys = fail_create = fail_upload = 0;
}
void tearDown(void) { destroy(); }

static void test_default_static_roots_do_not_consume_each_view_transient_pool(void)
{
    TEST_ASSERT_TRUE(sr_foliage_persist_enabled(NULL, false));
    TEST_ASSERT_TRUE(sr_foliage_persist_enabled("", false));
}
static void test_explicit_transient_diagnostic_is_preserved(void)
{
    TEST_ASSERT_FALSE(sr_foliage_persist_enabled("0", false));
    TEST_ASSERT_FALSE(sr_foliage_persist_enabled("0-diagnostic", false));
    TEST_ASSERT_TRUE(sr_foliage_persist_enabled("1", false));
}
static void test_compute_culling_always_requires_resident_roots(void)
{
    TEST_ASSERT_TRUE(sr_foliage_persist_enabled(NULL, true));
    TEST_ASSERT_TRUE(sr_foliage_persist_enabled("0", true));
}
static void test_zero_handle_and_zero_hash_are_valid_first_upload(void)
{
    TEST_ASSERT_TRUE(ensure(65536u, 0u, false));
    TEST_ASSERT_EQUAL_UINT16(0, handle);
    TEST_ASSERT_EQUAL_UINT32(0, resident_hash);
    TEST_ASSERT_EQUAL_UINT32(65536u, resident_count);
    TEST_ASSERT_TRUE(uploaded[handle]);
}
static void test_second_view_reuses_exact_static_roots_without_copy(void)
{
    TEST_ASSERT_TRUE(ensure(65536u, 7u, false));
    TEST_ASSERT_TRUE(ensure(65536u, 7u, false));
    TEST_ASSERT_EQUAL_UINT(1, creates);
    TEST_ASSERT_EQUAL_UINT(1, uploads);
    TEST_ASSERT_EQUAL_UINT(0, destroys);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
}
static void test_changed_count_requires_new_contents_even_with_same_hash(void)
{
    TEST_ASSERT_TRUE(ensure(4u, 7u, false));
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_EQUAL_UINT32(8, resident_count);
    TEST_ASSERT_EQUAL_UINT(2, uploads);
    TEST_ASSERT_EQUAL_UINT(1, destroys);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
}
static void test_changed_hash_replaces_one_buffer(void)
{
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_TRUE(ensure(8u, 9u, false));
    TEST_ASSERT_EQUAL_UINT32(9, resident_hash);
    TEST_ASSERT_EQUAL_UINT(2, uploads);
    TEST_ASSERT_EQUAL_UINT(1, destroys);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
}
static void test_compute_capability_edge_cannot_reuse_noncompute_buffer(void)
{
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_TRUE(ensure(8u, 7u, true));
    TEST_ASSERT_TRUE(resident_compute);
    TEST_ASSERT_TRUE(created_compute);
    TEST_ASSERT_EQUAL_UINT(2, creates);
}
static void test_return_to_cpu_path_releases_compute_storage_once(void)
{
    TEST_ASSERT_TRUE(ensure(8u, 7u, true));
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_FALSE(resident_compute);
    TEST_ASSERT_FALSE(created_compute);
    TEST_ASSERT_EQUAL_UINT(2, creates);
    TEST_ASSERT_EQUAL_UINT(1, destroys);
}
static void test_cold_create_failure_has_no_owner_and_retries(void)
{
    fail_create = 1;
    TEST_ASSERT_FALSE(ensure(8u, 7u, false));
    TEST_ASSERT_EQUAL_UINT(0, live_count());
    TEST_ASSERT_EQUAL_UINT32(0, resident_count);
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_EQUAL_UINT(1, live_count());
    TEST_ASSERT_EQUAL_UINT(1, uploads);
}
static void test_failed_replacement_keeps_owner_but_never_reports_new_contents(void)
{
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    const uint16_t old = handle;
    fail_create = 2;
    TEST_ASSERT_FALSE(ensure(8u, 9u, false));
    TEST_ASSERT_EQUAL_UINT16(old, handle);
    TEST_ASSERT_EQUAL_UINT32(7, resident_hash);
    TEST_ASSERT_EQUAL_UINT32(8, resident_count);
    TEST_ASSERT_EQUAL_UINT(0, destroys);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
}
static void test_failed_copy_destroys_unpublished_candidate_and_retries(void)
{
    fail_upload = 1;
    TEST_ASSERT_FALSE(ensure(8u, 7u, false));
    TEST_ASSERT_EQUAL_UINT32(0, resident_count);
    TEST_ASSERT_EQUAL_UINT(1, destroys);
    TEST_ASSERT_EQUAL_UINT(0, live_count());
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_EQUAL_UINT(2, creates);
    TEST_ASSERT_EQUAL_UINT(2, uploads);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
}
static void test_failed_copy_during_replacement_preserves_old_owner_only(void)
{
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    const uint16_t old = handle;
    fail_upload = 2;
    TEST_ASSERT_FALSE(ensure(8u, 9u, true));
    TEST_ASSERT_EQUAL_UINT16(old, handle);
    TEST_ASSERT_EQUAL_UINT32(7, resident_hash);
    TEST_ASSERT_FALSE(resident_compute);
    TEST_ASSERT_EQUAL_UINT(1, destroys);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
    TEST_ASSERT_TRUE(ensure(8u, 9u, true));
    TEST_ASSERT_EQUAL_UINT32(9, resident_hash);
    TEST_ASSERT_TRUE(resident_compute);
    TEST_ASSERT_EQUAL_UINT(3, creates);
    TEST_ASSERT_EQUAL_UINT(2, destroys);
    TEST_ASSERT_EQUAL_UINT(1, live_count());
}
static void test_empty_request_is_not_a_successful_current_buffer(void)
{
    TEST_ASSERT_FALSE(ensure(0u, 7u, false));
    TEST_ASSERT_EQUAL_UINT(0, creates);
}
static void test_destroy_is_idempotent_and_handles_zero(void)
{
    TEST_ASSERT_TRUE(ensure(8u, 7u, false));
    TEST_ASSERT_EQUAL_UINT16(0, handle);
    destroy(); destroy();
    TEST_ASSERT_EQUAL_UINT16(UINT16_MAX, handle);
    TEST_ASSERT_EQUAL_UINT32(0, resident_count);
    TEST_ASSERT_EQUAL_UINT32(0, resident_hash);
    TEST_ASSERT_FALSE(resident_compute);
    TEST_ASSERT_EQUAL_UINT(1, destroys);
    TEST_ASSERT_EQUAL_UINT(0, live_count());
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_static_roots_do_not_consume_each_view_transient_pool);
    RUN_TEST(test_explicit_transient_diagnostic_is_preserved);
    RUN_TEST(test_compute_culling_always_requires_resident_roots);
    RUN_TEST(test_zero_handle_and_zero_hash_are_valid_first_upload);
    RUN_TEST(test_second_view_reuses_exact_static_roots_without_copy);
    RUN_TEST(test_changed_count_requires_new_contents_even_with_same_hash);
    RUN_TEST(test_changed_hash_replaces_one_buffer);
    RUN_TEST(test_compute_capability_edge_cannot_reuse_noncompute_buffer);
    RUN_TEST(test_return_to_cpu_path_releases_compute_storage_once);
    RUN_TEST(test_cold_create_failure_has_no_owner_and_retries);
    RUN_TEST(test_failed_replacement_keeps_owner_but_never_reports_new_contents);
    RUN_TEST(test_failed_copy_destroys_unpublished_candidate_and_retries);
    RUN_TEST(test_failed_copy_during_replacement_preserves_old_owner_only);
    RUN_TEST(test_empty_request_is_not_a_successful_current_buffer);
    RUN_TEST(test_destroy_is_idempotent_and_handles_zero);
    return UNITY_END();
}
