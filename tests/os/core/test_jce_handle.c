/*
 * test_jce_handle.c — Unit tests for jce_handle.h
 *
 * Layer: L1.  Generational handle pool.
 */

#include "unity.h"

#include <jce/os/core/jce_handle.h>

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

static jce_handle_pool_t *make_pool(uint32_t cap)
{
    return jce_handle_pool_create(jce_allocator_default(), sizeof(int), cap);
}

static void test_null_handle_classification(void)
{
    jce_handle_t h = JCE_HANDLE_NULL;
    TEST_ASSERT_FALSE(JCE_HANDLE_VALID(h));
}

static void test_make_decomposes_back(void)
{
    jce_handle_t h = JCE_HANDLE_MAKE(3u, 7u);
    TEST_ASSERT_EQUAL_UINT32(3u, JCE_HANDLE_GEN(h));
    TEST_ASSERT_EQUAL_UINT32(7u, JCE_HANDLE_INDEX(h));
    TEST_ASSERT_TRUE(JCE_HANDLE_VALID(h));
}

static void test_add_get_remove_roundtrip(void)
{
    jce_handle_pool_t *p = make_pool(8);
    TEST_ASSERT_NOT_NULL(p);

    int v = 42;
    jce_handle_t h = jce_handle_pool_add(p, &v);
    TEST_ASSERT_TRUE(JCE_HANDLE_VALID(h));
    TEST_ASSERT_EQUAL_UINT32(1u, jce_handle_pool_count(p));
    TEST_ASSERT_TRUE(jce_handle_pool_alive(p, h));

    int *got = (int *)jce_handle_pool_get(p, h);
    TEST_ASSERT_NOT_NULL(got);
    TEST_ASSERT_EQUAL_INT(42, *got);

    jce_handle_pool_remove(p, h);
    TEST_ASSERT_FALSE(jce_handle_pool_alive(p, h));
    TEST_ASSERT_NULL(jce_handle_pool_get(p, h));
    TEST_ASSERT_EQUAL_UINT32(0u, jce_handle_pool_count(p));

    jce_handle_pool_destroy(p);
}

static void test_stale_handle_after_remove_reuse(void)
{
    jce_handle_pool_t *p = make_pool(4);
    int v = 1;
    jce_handle_t h1 = jce_handle_pool_add(p, &v);
    jce_handle_pool_remove(p, h1);

    v = 2;
    jce_handle_t h2 = jce_handle_pool_add(p, &v);
    /* Generation must differ — old handle no longer addresses the slot. */
    TEST_ASSERT_NOT_EQUAL(h1.id, h2.id);
    TEST_ASSERT_NULL(jce_handle_pool_get(p, h1));
    TEST_ASSERT_NOT_NULL(jce_handle_pool_get(p, h2));

    jce_handle_pool_destroy(p);
}

static void test_pool_full_returns_null_handle(void)
{
    jce_handle_pool_t *p = make_pool(2);
    int v = 0;
    TEST_ASSERT_TRUE(JCE_HANDLE_VALID(jce_handle_pool_add(p, &v)));
    TEST_ASSERT_TRUE(JCE_HANDLE_VALID(jce_handle_pool_add(p, &v)));
    jce_handle_t h = jce_handle_pool_add(p, &v);
    TEST_ASSERT_FALSE(JCE_HANDLE_VALID(h));
    jce_handle_pool_destroy(p);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_null_handle_classification);
    RUN_TEST(test_make_decomposes_back);
    RUN_TEST(test_add_get_remove_roundtrip);
    RUN_TEST(test_stale_handle_after_remove_reuse);
    RUN_TEST(test_pool_full_returns_null_handle);
    return UNITY_END();
}
