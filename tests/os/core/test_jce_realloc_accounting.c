/* Actual accounting owner with deterministic allocator outcomes, no GPU. */
#include "unity.h"
#include <mimalloc.h>
#include <stdint.h>
#include <stdbool.h>
#include <string.h>

static unsigned char block_a[256], block_b[512];
static bool valid_a, valid_b;
static size_t usable_a, usable_b;
static unsigned mode, invalid_queries;

static void *fake_malloc(size_t size)
{
    if (!size || size > sizeof(block_a)) return NULL;
    valid_a = true;
    usable_a = size;
    return block_a;
}
static void *fake_calloc(size_t count, size_t size)
{
    void *p = fake_malloc(count * size);
    if (p) memset(p, 0, count * size);
    return p;
}
static size_t fake_usable(const void *p)
{
    if (p == block_a && valid_a) return usable_a;
    if (p == block_b && valid_b) return usable_b;
    ++invalid_queries;
    return 0;
}
static void fake_free(void *p)
{
    if (p == block_a) valid_a = false;
    if (p == block_b) valid_b = false;
}
static void *fake_realloc(void *p, size_t size)
{
    if (!p) return fake_malloc(size);
    if (mode == 0) return NULL; /* Failed growth leaves the old block valid. */
    if (mode == 1) { usable_a = size; return p; }
    memcpy(block_b, block_a, usable_a);
    valid_a = false; /* Querying usable size after this point is invalid. */
    valid_b = true;
    usable_b = size;
    return block_b;
}

/* Force diagnostic tracking even in Release; isolate all public/private
 * owner symbols from the separately linked real core implementation. */
#undef NDEBUG
#undef JCE_TRACY_ENABLED
#define JCE_TRACY_ENABLED 0
#define mi_malloc fake_malloc
#define mi_calloc fake_calloc
#define mi_realloc fake_realloc
#define mi_free fake_free
#define mi_usable_size fake_usable
#define jce_alloc_frame_delta control_frame_delta
#define jce_alloc_track_snapshot control_track_snapshot
#define jce__alloc_account_alloc control_account_alloc
#define jce__alloc_account_free control_account_free
#define jce__malloc_tracked control_malloc
#define jce__calloc_tracked control_calloc
#define jce__realloc_tracked control_realloc
#define jce__free_tracked control_free
#include "os/core/jce_memory.c"

void setUp(void)
{
    valid_a = valid_b = false;
    usable_a = usable_b = 0;
    mode = invalid_queries = 0;
    jce_native_atomic_word_store(&s_live_bytes, 0);
    jce_native_atomic_word_store(&s_live_count, 0);
    jce_native_atomic_word_store(&s_peak_bytes, 0);
    jce_native_atomic_word_store(&s_total_allocs, 0);
    jce_native_atomic_word_store(&s_total_frees, 0);
    jce_native_atomic_word_store(&s_pf_allocs, 0);
    jce_native_atomic_word_store(&s_pf_alloc_bytes, 0);
    s_pf_last_allocs = s_pf_last_bytes = 0;
    for (int i = 0; i < JCE_ALLOC_TRACK_BUCKETS; ++i) {
        jce_native_atomic_word_store(&s_bkt_bytes[i], 0);
        jce_native_atomic_word_store(&s_bkt_count[i], 0);
    }
}
void tearDown(void) { TEST_ASSERT_EQUAL_UINT(0, invalid_queries); }

static void assert_live(size_t bytes, size_t allocs, size_t frees)
{
    JceAllocTrack state;
    TEST_ASSERT_TRUE(control_track_snapshot(&state));
    TEST_ASSERT_EQUAL_UINT64(bytes, state.live_bytes);
    TEST_ASSERT_EQUAL_UINT64(bytes ? 1 : 0, state.live_count);
    TEST_ASSERT_EQUAL_UINT64(allocs, state.total_allocs);
    TEST_ASSERT_EQUAL_UINT64(frees, state.total_frees);
}

static void test_failure_same_address_move_and_zero_use_actual_owner(void)
{
    void *p = control_malloc(64);
    TEST_ASSERT_EQUAL_PTR(block_a, p);
    memset(p, 0x5a, 64);
    assert_live(64, 1, 0);
    TEST_ASSERT_NULL(control_realloc(p, 128));
    TEST_ASSERT_TRUE(valid_a);
    TEST_ASSERT_EQUAL_HEX8(0x5a, block_a[0]);
    assert_live(64, 1, 0);
    mode = 1;
    TEST_ASSERT_EQUAL_PTR(p, control_realloc(p, 96));
    assert_live(96, 2, 1);
    mode = 2;
    p = control_realloc(p, 128);
    TEST_ASSERT_EQUAL_PTR(block_b, p);
    TEST_ASSERT_FALSE(valid_a);
    TEST_ASSERT_EQUAL_HEX8(0x5a, block_b[0]);
    assert_live(128, 3, 2);
    TEST_ASSERT_NULL(control_realloc(p, 0));
    TEST_ASSERT_FALSE(valid_b);
    assert_live(0, 3, 3);
}

static void test_null_and_failed_initial_allocation(void)
{
    TEST_ASSERT_NULL(control_realloc(NULL, sizeof(block_a) + 1));
    assert_live(0, 0, 0);
    void *p = control_realloc(NULL, 32);
    TEST_ASSERT_EQUAL_PTR(block_a, p);
    assert_live(32, 1, 0);
    control_free(p);
    assert_live(0, 1, 1);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_failure_same_address_move_and_zero_use_actual_owner);
    RUN_TEST(test_null_and_failed_initial_allocation);
    return UNITY_END();
}
