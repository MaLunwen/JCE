/*
 * test_jce_str_intern.c - scene-scoped string interning.
 *
 * These pin the properties the component change depends on. The pointers this
 * returns are stored INSIDE components that flecs memcpys between tables, so
 * "stable for the pool's lifetime" is not a nicety -- a reallocating pool would
 * leave every mesh renderer holding a dangling path.
 */

#include "unity.h"

#include <jce/middleware/scene/jce_str_intern.h>

#include <stdio.h>
#include <string.h>

static JceStrPool *g_pool;

void setUp(void)    { g_pool = jce_str_pool_create(); }
void tearDown(void) { jce_str_pool_destroy(g_pool); g_pool = NULL; }

static void test_equal_strings_share_one_pointer(void)
{
    const char *a = jce_str_intern(g_pool, "resources/assets/models/tree.glb");
    const char *b = jce_str_intern(g_pool, "resources/assets/models/tree.glb");
    TEST_ASSERT_EQUAL_PTR_MESSAGE(a, b, "identical paths must intern to one string");
    TEST_ASSERT_EQUAL_STRING("resources/assets/models/tree.glb", a);
    TEST_ASSERT_EQUAL_UINT32(1, jce_str_pool_count(g_pool));
}

static void test_different_strings_stay_distinct(void)
{
    const char *a = jce_str_intern(g_pool, "a/one.glb");
    const char *b = jce_str_intern(g_pool, "a/two.glb");
    TEST_ASSERT_NOT_EQUAL(a, b);
    TEST_ASSERT_EQUAL_STRING("a/one.glb", a);
    TEST_ASSERT_EQUAL_STRING("a/two.glb", b);
    TEST_ASSERT_EQUAL_UINT32(2, jce_str_pool_count(g_pool));
}

/* Every read site in the engine writes `mr->mesh_path[0]` with no null check,
 * because it used to be an array. A NULL return would turn all of those into
 * crashes, so empty and NULL must both yield a dereferenceable "". */
static void test_empty_and_null_never_return_null(void)
{
    const char *e = jce_str_intern(g_pool, "");
    const char *n = jce_str_intern(g_pool, NULL);
    TEST_ASSERT_NOT_NULL(e);
    TEST_ASSERT_NOT_NULL(n);
    TEST_ASSERT_EQUAL_PTR(e, n);
    TEST_ASSERT_EQUAL_PTR(jce_str_empty(), e);
    TEST_ASSERT_EQUAL_CHAR('\0', e[0]);
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(0, jce_str_pool_count(g_pool),
        "the shared empty string must not occupy a pool slot");
}

/* The property the component change rests on: a pointer handed out early must
 * survive every later insertion, including the rehashes that grow the table. */
static void test_pointers_survive_growth(void)
{
    enum { N = 4096 };
    static const char *held[N];
    char buf[64];

    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "resources/assets/textures/tex_%04d.png", i);
        held[i] = jce_str_intern(g_pool, buf);
    }
    TEST_ASSERT_EQUAL_UINT32(N, jce_str_pool_count(g_pool));

    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf, "resources/assets/textures/tex_%04d.png", i);
        TEST_ASSERT_EQUAL_STRING_MESSAGE(buf, held[i],
            "an early pointer changed contents after the table grew");
        TEST_ASSERT_EQUAL_PTR_MESSAGE(held[i], jce_str_intern(g_pool, buf),
            "re-interning after growth returned a different pointer");
    }
}

/* Interning is by value, which is where the memory saving comes from: a scene
 * of 200k props sharing one mesh path stores that path once. */
static void test_repeats_cost_nothing(void)
{
    const char *first = jce_str_intern(g_pool, "shared/prop.glb");
    const size_t bytes_after_first = jce_str_pool_bytes(g_pool);
    for (int i = 0; i < 10000; i++)
        TEST_ASSERT_EQUAL_PTR(first, jce_str_intern(g_pool, "shared/prop.glb"));
    TEST_ASSERT_EQUAL_UINT32(1, jce_str_pool_count(g_pool));
    TEST_ASSERT_EQUAL_size_t(bytes_after_first, jce_str_pool_bytes(g_pool));
}

/* A component may be built before any scene exists (defaults, importers). That
 * path must not crash; stability is the caller's problem there, and every such
 * caller copies the value immediately. */
static void test_null_pool_is_tolerated(void)
{
    TEST_ASSERT_EQUAL_STRING("x/y.glb", jce_str_intern(NULL, "x/y.glb"));
    TEST_ASSERT_EQUAL_PTR(jce_str_empty(), jce_str_intern(NULL, ""));
    TEST_ASSERT_EQUAL_PTR(jce_str_empty(), jce_str_intern(NULL, NULL));
}

static void test_hash_collisions_do_not_lose_entries(void)
{
    /* Long shared prefixes are the realistic case for asset paths and the one
     * that clusters an open-addressed table hardest. */
    enum { N = 1000 };
    char buf[128];
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf,
                 "resources/assets/models/props/very/deep/path/item_%d.glb", i);
        jce_str_intern(g_pool, buf);
    }
    TEST_ASSERT_EQUAL_UINT32(N, jce_str_pool_count(g_pool));
    for (int i = 0; i < N; i++) {
        snprintf(buf, sizeof buf,
                 "resources/assets/models/props/very/deep/path/item_%d.glb", i);
        TEST_ASSERT_EQUAL_STRING(buf, jce_str_intern(g_pool, buf));
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(N, jce_str_pool_count(g_pool),
        "re-interning existing strings must not add slots");
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_equal_strings_share_one_pointer);
    RUN_TEST(test_different_strings_stay_distinct);
    RUN_TEST(test_empty_and_null_never_return_null);
    RUN_TEST(test_pointers_survive_growth);
    RUN_TEST(test_repeats_cost_nothing);
    RUN_TEST(test_null_pool_is_tolerated);
    RUN_TEST(test_hash_collisions_do_not_lose_entries);
    return UNITY_END();
}
