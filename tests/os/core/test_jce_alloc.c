/*
 * test_jce_alloc.c — Unit tests for engine/include/jce/os/core/jce_alloc.h
 *
 * Layer: L1.  Public allocator entry points (mimalloc-backed).
 */

#include "unity.h"

#include <jce/os/core/jce_alloc.h>

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static void test_malloc_returns_writable_buffer(void)
{
    void *p = jce_malloc(64);
    TEST_ASSERT_NOT_NULL(p);
    memset(p, 0xA5, 64);
    jce_free(p);
}

static void test_malloc_zero_returns_null_or_freeable(void)
{
    /* Documented contract: NULL on size==0.  Tolerate either NULL or
       a freeable pointer to stay portable across allocator backends. */
    void *p = jce_malloc(0);
    if (p) {
        jce_free(p);
    }
}

static void test_realloc_grow_preserves_prefix(void)
{
    char *p = (char *)jce_malloc(8);
    TEST_ASSERT_NOT_NULL(p);
    memcpy(p, "ABCDEFGH", 8);

    p = (char *)jce_realloc(p, 64);
    TEST_ASSERT_NOT_NULL(p);
    TEST_ASSERT_EQUAL_MEMORY("ABCDEFGH", p, 8);

    jce_free(p);
}

static void test_realloc_null_acts_like_malloc(void)
{
    void *p = jce_realloc(NULL, 32);
    TEST_ASSERT_NOT_NULL(p);
    jce_free(p);
}

static void test_free_null_is_safe(void)
{
    jce_free(NULL);   /* must not crash */
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_malloc_returns_writable_buffer);
    RUN_TEST(test_malloc_zero_returns_null_or_freeable);
    RUN_TEST(test_realloc_grow_preserves_prefix);
    RUN_TEST(test_realloc_null_acts_like_malloc);
    RUN_TEST(test_free_null_is_safe);
    return UNITY_END();
}
