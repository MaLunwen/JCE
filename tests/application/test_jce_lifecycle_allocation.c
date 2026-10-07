/* Exact production registry with a faultable allocator, no device/engine boot. */
#include <jce/application/jce_lifecycle.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include "unity.h"
#include <stdlib.h>
#include <string.h>

typedef struct AllocationWitness {
    bool fail_allocation;
    unsigned errors;
    unsigned snapshot_attempts;
    unsigned allocation_attempts;
} AllocationWitness;
static AllocationWitness witness;
static void *test_alloc(size_t size, void *ctx)
{
    AllocationWitness *w = ctx;
    ++w->allocation_attempts;
    if (w->fail_allocation) return NULL;
    return malloc(size);
}
static void *test_realloc(void *ptr, size_t size, void *ctx)
{
    AllocationWitness *w = ctx;
    ++w->allocation_attempts;
    if (!ptr && size == 65u * sizeof(uint32_t)) ++w->snapshot_attempts;
    if (w->fail_allocation) return NULL;
    return realloc(ptr, size);
}
static void test_free(void *ptr, void *ctx)
{
    (void)ctx;
    free(ptr);
}
jce_allocator_t JCE_CALL jce_allocator_default(void)
{
    jce_allocator_t a = {test_alloc, test_realloc, test_free, &witness};
    return a;
}
void JCE_CALL jce_log_write(JceLogLevel level, const char *tag,
    const char *file, int line, const char *format, ...)
{
    (void)tag; (void)file; (void)line; (void)format;
    if (level == JCE_LOG_LEVEL_ERROR) ++witness.errors;
}
void setUp(void)
{
    jce_lifecycle_shutdown();
    memset(&witness, 0, sizeof witness);
}
void tearDown(void) { jce_lifecycle_shutdown(); }
static void count_one(JceLifecycleEvent event, void *user)
{
    (void)event;
    ++*(unsigned *)user;
}
static void test_low_memory_dispatch_reaches_all_65_without_allocation(void)
{
    unsigned seen[65] = {0};
    for (unsigned i = 0; i < 65; ++i) {
        JceLifecycleHandle h = jce_lifecycle_register(count_one, (int32_t)i, &seen[i]);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, h.id);
    }
    const unsigned allocations_before = witness.allocation_attempts;
    witness.fail_allocation = true;
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    for (unsigned i = 0; i < 65; ++i) TEST_ASSERT_EQUAL_UINT(1, seen[i]);
    TEST_ASSERT_EQUAL_UINT(0, witness.snapshot_attempts);
    TEST_ASSERT_EQUAL_UINT(0, witness.errors);
    TEST_ASSERT_EQUAL_UINT(allocations_before, witness.allocation_attempts);

    jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE);
    TEST_ASSERT_TRUE(jce_lifecycle_is_paused());
    jce_lifecycle_emit(JCE_LIFECYCLE_RESUME);
    TEST_ASSERT_FALSE(jce_lifecycle_is_paused());
    for (unsigned i = 0; i < 65; ++i) TEST_ASSERT_EQUAL_UINT(3, seen[i]);
    TEST_ASSERT_EQUAL_UINT(allocations_before, witness.allocation_attempts);
}

static void test_registration_grow_failure_preserves_listeners_and_can_retry(void)
{
    unsigned seen[5] = {0};
    JceLifecycleHandle last = {0};
    for (unsigned i = 0; i < 4; ++i) {
        last = jce_lifecycle_register(count_one, (int32_t)i, &seen[i]);
        TEST_ASSERT_NOT_EQUAL_UINT32(0, last.id);
    }
    const unsigned allocations_before = witness.allocation_attempts;
    witness.fail_allocation = true;
    JceLifecycleHandle rejected = jce_lifecycle_register(count_one, 4, &seen[4]);
    TEST_ASSERT_EQUAL_UINT32(0, rejected.id);
    TEST_ASSERT_EQUAL_UINT(1, witness.errors);
    TEST_ASSERT_EQUAL_UINT(allocations_before + 1, witness.allocation_attempts);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    for (unsigned i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_UINT(1, seen[i]);
    TEST_ASSERT_EQUAL_UINT(0, seen[4]);
    TEST_ASSERT_EQUAL_UINT(allocations_before + 1, witness.allocation_attempts);
    witness.fail_allocation = false;
    JceLifecycleHandle retried = jce_lifecycle_register(count_one, 4, &seen[4]);
    TEST_ASSERT_EQUAL_UINT32(last.id + 1, retried.id);
    jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY);
    for (unsigned i = 0; i < 4; ++i) TEST_ASSERT_EQUAL_UINT(2, seen[i]);
    TEST_ASSERT_EQUAL_UINT(1, seen[4]);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_low_memory_dispatch_reaches_all_65_without_allocation);
    RUN_TEST(test_registration_grow_failure_preserves_listeners_and_can_retry);
    return UNITY_END();
}
