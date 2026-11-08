/* Complete production frame-pool owner and actual enki workers. The shutdown
 * forwarding port releases an already-running task at the real join boundary. */
#include "unity.h"
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
#include <enkiTS/TaskScheduler_c.h>
#include <SDL3/SDL.h>
#include "os/core/jce_thread_session_internal.h"

static JceSemaphore *retire_started, *retire_release;
static enkiTaskScheduler *retire_scheduler;
static unsigned retire_joins;
static SDL_AtomicInt retire_recreated, retire_recursive_returned, retire_timed_out;
static uint64_t retire_owner_tid, retire_worker_tid;
static void retire_wait_all(enkiTaskScheduler *scheduler)
{
    if (scheduler == retire_scheduler) {
        retire_scheduler = NULL;
        jce_semaphore_signal(retire_release);
    }
    enkiWaitforAllAndShutdown(scheduler);
    ++retire_joins;
}
#define enkiWaitforAllAndShutdown retire_wait_all
#ifndef JCE_TEST_THREAD_RETIREMENT_OWNER
#define JCE_TEST_THREAD_RETIREMENT_OWNER "os/core/jce_thread.c"
#endif
#include JCE_TEST_THREAD_RETIREMENT_OWNER
#undef enkiWaitforAllAndShutdown

static void retire_reentrant_work(void *argument)
{
    (void)argument;
    retire_worker_tid = jce_thread_current_id();
    jce_semaphore_signal(retire_started);
    if (!jce_semaphore_wait_timeout(retire_release, 5000)) {
        SDL_SetAtomicInt(&retire_timed_out, 1);
        return;
    }
    SDL_SetAtomicInt(&retire_recreated, jce_thread_pool_shared() != NULL);
    /* Current code returns while the outer owner joins. The old-code mutant
     * has created a distinct pool on this worker; this safely drains that pool
     * before the original worker returns, avoiding a leak as the test oracle. */
    jce_thread_pool_shared_shutdown();
    SDL_SetAtomicInt(&retire_recursive_returned, 1);
}
void setUp(void)
{
    retire_scheduler = NULL;
    retire_joins = 0;
    retire_owner_tid = jce_thread_current_id();
    retire_worker_tid = 0;
    SDL_SetAtomicInt(&retire_recreated, 0);
    SDL_SetAtomicInt(&retire_recursive_returned, 0);
    SDL_SetAtomicInt(&retire_timed_out, 0);
    jce_thread_pool_session_open_internal();
    jce_thread_pool_shared_set_workers(1);
    retire_started = jce_semaphore_create(0);
    retire_release = jce_semaphore_create(0);
    TEST_ASSERT_NOT_NULL(retire_started);
    TEST_ASSERT_NOT_NULL(retire_release);
}
void tearDown(void)
{
    if (retire_release) jce_semaphore_signal(retire_release);
    jce_thread_pool_shared_shutdown();
    jce_thread_pool_session_open_internal();
    jce_semaphore_destroy(retire_started);
    jce_semaphore_destroy(retire_release);
    retire_started = retire_release = NULL;
}
static void test_real_worker_cannot_recreate_shared_pool_during_its_join(void)
{
    JceThreadPool *pool = jce_thread_pool_shared();
    TEST_ASSERT_NOT_NULL(pool);
    retire_scheduler = pool->scheduler;
    TEST_ASSERT_TRUE(jce_thread_pool_submit_named(pool, "retirement.reentry",
                                                retire_reentrant_work, NULL));
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(retire_started, 5000));
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_TRUE(retire_owner_tid != retire_worker_tid);
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&retire_timed_out));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&retire_recursive_returned));
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&retire_recreated));
    TEST_ASSERT_EQUAL_UINT(1, retire_joins);
    TEST_ASSERT_NULL(g_shared_pool);
    TEST_ASSERT_FALSE(g_shared_closing);
    /* Public standalone shutdown still admits the next ordinary pool. */
    jce_thread_pool_shared_set_workers(1);
    TEST_ASSERT_NOT_NULL(jce_thread_pool_shared());
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_EQUAL_UINT(2, retire_joins);
}
static void test_session_close_preserves_existing_borrow_then_blocks_recreation(void)
{
    JceThreadPool *pool = jce_thread_pool_shared();
    TEST_ASSERT_NOT_NULL(pool);
    jce_thread_pool_session_close_internal();
    jce_thread_pool_session_close_internal();
    TEST_ASSERT_EQUAL_PTR(pool, jce_thread_pool_shared());
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_NULL(jce_thread_pool_shared());
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_NULL(jce_thread_pool_shared());
    jce_thread_pool_session_open_internal();
    jce_thread_pool_shared_set_workers(1);
    TEST_ASSERT_NOT_NULL(jce_thread_pool_shared());
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_EQUAL_UINT(2, retire_joins);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_real_worker_cannot_recreate_shared_pool_during_its_join);
    RUN_TEST(test_session_close_preserves_existing_borrow_then_blocks_recreation);
    return UNITY_END();
}
