/* Real config publisher/enki executor/frame pool budgets; no GPU/device. */
#include "unity.h"
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_thread.h>
#include "os/core/jce_worker_budget_internal.h"
#include <limits.h>

#ifdef JCE_TEST_WORKER_CONFIG_CORES
/* Only the exact mutant config TU has this counted platform-query port.
 * Real enki schedulers, thread counts and join/destroy remain production. */
int jce_test_worker_logical_cores(void) { return JCE_TEST_WORKER_CONFIG_CORES; }
#endif

void setUp(void)
{
    jce_thread_mark_main();
    jce_thread_pool_shared_shutdown();
    jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_CANCEL_ALL, 5000);
    jce_config_publish_perf(JCE_MACHINE_CLASS_AUTO, 0);
}
void tearDown(void)
{
    jce_thread_pool_shared_shutdown();
    jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_CANCEL_ALL, 5000);
    jce_config_publish_perf(JCE_MACHINE_CLASS_AUTO, 0);
}
static int frame_default(void)
{
#ifdef JCE_TEST_WORKER_CONFIG_CORES
    return jce_worker_budget_default(false,JCE_TEST_WORKER_CONFIG_CORES);
#else
    return jce_thread_pool_default_workers();
#endif
}
static void observe_budget(int frame_expected,int async_expected)
{
    JceThreadPool *pool=jce_thread_pool_shared();
    int frame_actual=jce_thread_pool_worker_count(pool);
    JceAsyncExecutorConfig config;
    JceAsyncExecutorStats stats={0};
    jce_async_executor_config_init(&config);
    config.mode=JCE_ASYNC_EXECUTION_THREADED;
    config.worker_count=0;
    config.max_tasks=16;
    JceAsyncExecutor *executor=jce_async_executor_create(&config);
    if (executor) jce_async_executor_get_stats(executor,&stats);
    jce_async_executor_destroy(executor);
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_NOT_NULL(pool);
    TEST_ASSERT_NOT_NULL(executor);
#if JCE_PLATFORM_WEB
    TEST_ASSERT_EQUAL_INT(0,frame_actual);
    TEST_ASSERT_EQUAL_UINT32(0,stats.worker_count);
#else
    TEST_ASSERT_EQUAL_INT(frame_expected,frame_actual);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)async_expected,stats.worker_count);
#endif
}
static void pure_default_boundaries_and_low_machine(void)
{
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_default(true,32));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_default(true,INT_MAX));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_default(false,INT_MIN));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_default(false,0));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_default(false,1));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_default(false,2));
    TEST_ASSERT_EQUAL_INT(2,jce_worker_budget_default(false,3));
    TEST_ASSERT_EQUAL_INT(8,jce_worker_budget_default(false,9));
    TEST_ASSERT_EQUAL_INT(8,jce_worker_budget_default(false,INT_MAX));
}
static void explicit_pins_keep_existing_precedence_and_ceiling(void)
{
    TEST_ASSERT_EQUAL_INT(4,jce_worker_budget_requested(true,32,4));
    TEST_ASSERT_EQUAL_INT(16,jce_worker_budget_requested(true,32,16));
    TEST_ASSERT_EQUAL_INT(16,jce_worker_budget_requested(false,32,INT_MAX));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_requested(true,32,0));
    TEST_ASSERT_EQUAL_INT(1,jce_worker_budget_requested(true,32,-1));
}
static void actual_publisher_low_defaults_both_owners_to_one(void)
{
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,0);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_LOW,jce_config_machine_class());
    TEST_ASSERT_EQUAL_INT(0,jce_config_job_workers());
    observe_budget(1,1);
}
static void resolved_budget_preserves_authored_class_and_full_override(void)
{
    jce_config_publish_perf_resolved(JCE_MACHINE_CLASS_AUTO,0,true);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_AUTO,jce_config_machine_class());
    observe_budget(1,1);
    jce_config_publish_perf_resolved(JCE_MACHINE_CLASS_LOW,0,false);
    TEST_ASSERT_EQUAL_INT(JCE_MACHINE_CLASS_LOW,jce_config_machine_class());
    observe_budget(frame_default(),frame_default());
}
static void actual_low_explicit_pin_is_preserved(void)
{
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,4);
    observe_budget(4,4);
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,16);
    observe_budget(16,8);
}
static void full_auto_clears_historical_uncreated_pool_pin(void)
{
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,3);
    jce_config_publish_perf(JCE_MACHINE_CLASS_FULL,0);
    observe_budget(frame_default(),frame_default());
}
static void published_change_does_not_resize_a_live_owned_pool(void)
{
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,1);
    JceThreadPool *pool=jce_thread_pool_shared();
    int before=jce_thread_pool_worker_count(pool);
    jce_config_publish_perf(JCE_MACHINE_CLASS_FULL,4);
    bool same=pool==jce_thread_pool_shared();
    int after=jce_thread_pool_worker_count(pool);
    jce_thread_pool_shared_shutdown();
    TEST_ASSERT_TRUE(same);
    TEST_ASSERT_EQUAL_INT(before,after);
}
static void cooperative_executor_keeps_zero_workers_and_teardown(void)
{
    jce_config_publish_perf(JCE_MACHINE_CLASS_LOW,4);
    JceAsyncExecutorConfig config;
    JceAsyncExecutorStats stats={0};
    jce_async_executor_config_init(&config);
    config.mode=JCE_ASYNC_EXECUTION_COOPERATIVE;
    config.worker_count=16;
    config.max_tasks=16;
    JceAsyncExecutor *executor=jce_async_executor_create(&config);
    if (executor) jce_async_executor_get_stats(executor,&stats);
    jce_async_executor_destroy(executor);
    TEST_ASSERT_NOT_NULL(executor);
    TEST_ASSERT_EQUAL_UINT32(0,stats.worker_count);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(pure_default_boundaries_and_low_machine);
    RUN_TEST(explicit_pins_keep_existing_precedence_and_ceiling);
    RUN_TEST(actual_publisher_low_defaults_both_owners_to_one);
    RUN_TEST(resolved_budget_preserves_authored_class_and_full_override);
    RUN_TEST(actual_low_explicit_pin_is_preserved);
    RUN_TEST(full_auto_clears_historical_uncreated_pool_pin);
    RUN_TEST(published_change_does_not_resize_a_live_owned_pool);
    RUN_TEST(cooperative_executor_keeps_zero_workers_and_teardown);
    return UNITY_END();
}
