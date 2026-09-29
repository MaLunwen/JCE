/*
 * test_jce_allocator.c — Unit tests for jce_allocator.h
 *
 * Layer: L1.  Covers default allocator, arena, aligned alloc, mem stats.
 */

#include "unity.h"

#include <jce/os/core/jce_allocator.h>

#include <stdint.h>
#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- default allocator ----------------------------------------------- */

static void test_default_allocator_round_trip(void)
{
    jce_allocator_t a = jce_allocator_default();
    TEST_ASSERT_NOT_NULL(a.alloc);
    TEST_ASSERT_NOT_NULL(a.free);

    void *p = JCE_ALLOC(a, 128);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0, 128);

    p = JCE_AREALLOC(a, p, 256);
    TEST_ASSERT_NOT_NULL(p);

    JCE_AFREE(a, p);
}

/* ---- arena ----------------------------------------------------------- */

static void test_arena_push_and_used(void)
{
    jce_allocator_t a = jce_allocator_default();
    jce_arena_t *arena = jce_arena_create(a, 1024);
    TEST_ASSERT_NOT_NULL(arena);

    TEST_ASSERT_EQUAL_size_t(1024, jce_arena_capacity(arena));
    TEST_ASSERT_EQUAL_size_t(0,    jce_arena_used(arena));

    void *p = jce_arena_push(arena, 64);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_TRUE(jce_arena_used(arena) >= 64);

    jce_arena_destroy(arena);
}

static void test_arena_push_zero_is_zeroed(void)
{
    jce_allocator_t a = jce_allocator_default();
    jce_arena_t *arena = jce_arena_create(a, 1024);

    unsigned char *p = (unsigned char *)jce_arena_push_zero(arena, 32);
    TEST_ASSERT_NOT_NULL(p);
    for (int i = 0; i < 32; ++i)
        TEST_ASSERT_EQUAL_UINT8(0u, p[i]);

    jce_arena_destroy(arena);
}

static void test_arena_reset(void)
{
    jce_allocator_t a = jce_allocator_default();
    jce_arena_t *arena = jce_arena_create(a, 256);
    (void)jce_arena_push(arena, 100);
    TEST_ASSERT_TRUE(jce_arena_used(arena) > 0);
    jce_arena_reset(arena);
    TEST_ASSERT_EQUAL_size_t(0, jce_arena_used(arena));
    jce_arena_destroy(arena);
}

static void test_arena_exhaustion_returns_null(void)
{
    jce_allocator_t a = jce_allocator_default();
    jce_arena_t *arena = jce_arena_create(a, 64);
    /* Push something huge — must fail without crashing. */
    void *p = jce_arena_push(arena, 1024 * 1024);
    TEST_ASSERT_NULL(p);
    jce_arena_destroy(arena);
}

/* ---- aligned alloc --------------------------------------------------- */

static void test_aligned_alloc_satisfies_alignment(void)
{
    void *p = jce_aligned_alloc(256, 64);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_UINT64(0u, ((uintptr_t)p) & 63u);
    jce_aligned_free(p);
    jce_aligned_free(NULL);   /* must not crash */
}

/* ---- memory stats ---------------------------------------------------- */

static void test_mem_stats_populates(void)
{
    JceMemStats s;
    memset(&s, 0xCD, sizeof(s));
    TEST_ASSERT_TRUE(jce_mem_stats(&s));
    /* Process has been running long enough to accrue some elapsed_ms. */
    /* peak_rss should be >= current_rss after monotonic ratchet. */
    TEST_ASSERT_TRUE(s.peak_rss >= s.current_rss);
}

static void test_mem_stats_null_returns_false(void)
{
    TEST_ASSERT_FALSE(jce_mem_stats(NULL));
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_default_allocator_round_trip);
    RUN_TEST(test_arena_push_and_used);
    RUN_TEST(test_arena_push_zero_is_zeroed);
    RUN_TEST(test_arena_reset);
    RUN_TEST(test_arena_exhaustion_returns_null);
    RUN_TEST(test_aligned_alloc_satisfies_alignment);
    RUN_TEST(test_mem_stats_populates);
    RUN_TEST(test_mem_stats_null_returns_false);
    return UNITY_END();
}
