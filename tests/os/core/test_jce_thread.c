/*
 * test_jce_thread.c — Unit tests for jce_thread.h
 *
 * Layer: L1.  Mutex / cond / atomics / TLS / dedicated thread / pool.
 * Single-core baseline friendly: each test joins within ~100 ms.
 */

#include "unity.h"

#include <jce/os/core/jce_thread.h>

#include <string.h>

void setUp(void)    { }
void tearDown(void) { }

/* ---- mutex ---------------------------------------------------------- */

static void test_mutex_lock_unlock(void)
{
    JceMutex *m = jce_mutex_create();
    TEST_ASSERT_NOT_NULL(m);
    jce_mutex_lock(m);
    jce_mutex_unlock(m);
    jce_mutex_destroy(m);
}

/* ---- atomics -------------------------------------------------------- */

static void test_atomic_i32(void)
{
    JceAtomicI32 *a = jce_atomic_i32_create(10);
    TEST_ASSERT_NOT_NULL(a);
    TEST_ASSERT_EQUAL_INT32(10, jce_atomic_i32_load(a));

    jce_atomic_i32_store(a, 25);
    TEST_ASSERT_EQUAL_INT32(25, jce_atomic_i32_load(a));

    TEST_ASSERT_EQUAL_INT32(25, jce_atomic_i32_exchange(a, 7));
    TEST_ASSERT_EQUAL_INT32(7,  jce_atomic_i32_load(a));

    TEST_ASSERT_EQUAL_INT32(7,  jce_atomic_i32_add(a, 3));   /* returns previous */
    TEST_ASSERT_EQUAL_INT32(10, jce_atomic_i32_load(a));

    jce_atomic_i32_destroy(a);
}

static void test_atomic_u64(void)
{
    JceAtomicU64 *a = jce_atomic_u64_create(100);
    TEST_ASSERT_EQUAL_UINT64(100u, jce_atomic_u64_load(a));
    jce_atomic_u64_store(a, 500u);
    TEST_ASSERT_EQUAL_UINT64(500u, jce_atomic_u64_load(a));
    TEST_ASSERT_EQUAL_UINT64(500u, jce_atomic_u64_add(a, 1u));   /* prev */
    TEST_ASSERT_EQUAL_UINT64(501u, jce_atomic_u64_load(a));
    jce_atomic_u64_destroy(a);
}

/* ---- semaphore ------------------------------------------------------ */

static void test_semaphore_wait_acquire(void)
{
    JceSemaphore *s = jce_semaphore_create(1);
    TEST_ASSERT_NOT_NULL(s);
    jce_semaphore_wait(s);                                /* drains the 1 */
    TEST_ASSERT_FALSE(jce_semaphore_wait_timeout(s, 5));  /* now empty */
    jce_semaphore_signal(s);
    TEST_ASSERT_TRUE (jce_semaphore_wait_timeout(s, 50));
    jce_semaphore_destroy(s);
}

/* ---- TLS ------------------------------------------------------------ */

static void test_tls_get_set(void)
{
    JceTLS *tls = jce_tls_create(NULL);
    TEST_ASSERT_NOT_NULL(tls);
    TEST_ASSERT_NULL(jce_tls_get(tls));
    int x = 0;
    jce_tls_set(tls, &x);
    TEST_ASSERT_EQUAL_PTR(&x, jce_tls_get(tls));
    /* TLS handle has no public destroy in API; leak is intentional for
       process lifetime.  Test isolation is fine. */
}

/* ---- dedicated thread + main marker -------------------------------- */

static volatile int g_ran;

static void thread_body(void *arg)
{
    *(int *)arg = 42;
    g_ran = 1;
}

static void test_dedicated_thread_runs_and_joins(void)
{
    int v = 0;
    g_ran = 0;
    JceThread *t = jce_thread_create(thread_body, &v, "ut-worker");
    TEST_ASSERT_NOT_NULL(t);
    jce_thread_join(t);
    TEST_ASSERT_EQUAL_INT(1, g_ran);
    TEST_ASSERT_EQUAL_INT(42, v);
}

static void test_main_thread_mark(void)
{
    jce_thread_mark_main();
    TEST_ASSERT_TRUE(jce_thread_is_main());
    TEST_ASSERT_TRUE(jce_thread_current_id() != 0u);
}

static void test_sleep_ms_noop(void)
{
    jce_thread_sleep_ms(1);    /* just must not crash on single-core */
}

/* ---- thread pool ---------------------------------------------------- */

static void pool_task(void *arg) { *(int *)arg += 1; }

static void test_thread_pool_submit_tracked(void)
{
    JceThreadPool *pool = jce_thread_pool_create(0);  /* auto */
    TEST_ASSERT_NOT_NULL(pool);

    int counter = 0;
    JceTask *task = jce_thread_pool_submit_tracked(pool, pool_task, &counter);
    TEST_ASSERT_NOT_NULL(task);
    jce_task_wait(task);
    TEST_ASSERT_TRUE(jce_task_done(task));
    TEST_ASSERT_EQUAL_INT(1, counter);
    jce_task_free(task);

    jce_thread_pool_destroy(pool);
}

static void test_thread_pool_submit_fire_and_forget(void)
{
    JceThreadPool *pool = jce_thread_pool_create(2);
    TEST_ASSERT_NOT_NULL(pool);

    int counters[8] = { 0 };
    for (int i = 0; i < 8; ++i)
        jce_thread_pool_submit(pool, pool_task, &counters[i]);

    /* destroy waits for all + drains pending list. */
    jce_thread_pool_destroy(pool);
    int total = 0;
    for (int i = 0; i < 8; ++i) total += counters[i];
    TEST_ASSERT_EQUAL_INT(8, total);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_mutex_lock_unlock);
    RUN_TEST(test_atomic_i32);
    RUN_TEST(test_atomic_u64);
    RUN_TEST(test_semaphore_wait_acquire);
    RUN_TEST(test_tls_get_set);
    RUN_TEST(test_dedicated_thread_runs_and_joins);
    RUN_TEST(test_main_thread_mark);
    RUN_TEST(test_sleep_ms_noop);
    RUN_TEST(test_thread_pool_submit_tracked);
    RUN_TEST(test_thread_pool_submit_fire_and_forget);
    return UNITY_END();
}
