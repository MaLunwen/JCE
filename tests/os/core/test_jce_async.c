/*
 * test_jce_async.c - Structured async runtime contract.
 *
 * These tests intentionally exercise both threaded execution and the
 * cooperative mode used by Web builds without pthreads.
 */

#include "unity.h"

#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_thread.h>

#include <stdint.h>

void setUp(void)    { }
void tearDown(void) { }

typedef struct TestCounters {
    JceAtomicI32 *work;
    JceAtomicI32 *complete;
    JceAtomicI32 *cleanup;
} TestCounters;

static JceAsyncRunResult count_work(JceAsyncContext *ctx, void *user_data)
{
    TestCounters *c = (TestCounters *)user_data;
    (void)ctx;
    jce_atomic_i32_add(c->work, 1);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void count_complete(JceAsyncTask *task, void *user_data)
{
    TestCounters *c = (TestCounters *)user_data;
    TEST_ASSERT_TRUE(jce_thread_is_main());
    TEST_ASSERT_TRUE(jce_async_task_is_terminal(task));
    jce_atomic_i32_add(c->complete, 1);
}

static void count_cleanup(void *user_data)
{
    TestCounters *c = (TestCounters *)user_data;
    jce_atomic_i32_add(c->cleanup, 1);
}

static JceAsyncExecutor *make_executor(JceAsyncExecutionMode mode,
                                       uint32_t workers,
                                       uint32_t capacity)
{
    JceAsyncExecutorConfig cfg;
    jce_async_executor_config_init(&cfg);
    cfg.mode = mode;
    cfg.worker_count = workers;
    cfg.max_tasks = capacity;
    cfg.debug_name = "test-async";
    return jce_async_executor_create(&cfg);
}

static JceAsyncTask *submit_count(JceAsyncExecutor *executor,
                                  TestCounters *c,
                                  JceAsyncGroup *group)
{
    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = count_work;
    desc.complete = count_complete;
    desc.cleanup = count_cleanup;
    desc.user_data = c;
    desc.group = group;
    desc.debug_name = "count";
    return jce_async_submit(executor, &desc);
}

static void pump_until_idle(JceAsyncExecutor *executor)
{
    JceAsyncPumpBudget budget;
    JceAsyncExecutorStats stats;
    int guard = 0;

    jce_async_pump_budget_init(&budget);
    budget.max_work_items = UINT32_MAX;
    budget.max_completions = UINT32_MAX;
    budget.max_time_us = 0;

    do {
        jce_async_executor_pump(executor, &budget);
        jce_async_executor_get_stats(executor, &stats);
        if (stats.live_tasks == 0)
            return;
        jce_thread_sleep_ms(1);
    } while (++guard < 5000);

    TEST_FAIL_MESSAGE("async executor did not become idle");
}

static void counters_init(TestCounters *c)
{
    c->work = jce_atomic_i32_create(0);
    c->complete = jce_atomic_i32_create(0);
    c->cleanup = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(c->work);
    TEST_ASSERT_NOT_NULL(c->complete);
    TEST_ASSERT_NOT_NULL(c->cleanup);
}

static void counters_destroy(TestCounters *c)
{
    jce_atomic_i32_destroy(c->work);
    jce_atomic_i32_destroy(c->complete);
    jce_atomic_i32_destroy(c->cleanup);
}

static void test_submit_is_not_inline_and_completion_is_owner_pumped(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncTask *task;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    TEST_ASSERT_NOT_NULL(executor);

    task = submit_count(executor, &c, NULL);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(c.work));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_QUEUED,
                          jce_async_task_state(task));

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_SUCCEEDED,
                          jce_async_task_state(task));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(c.work));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(c.complete));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(c.cleanup));

    jce_async_task_release(task);
    jce_async_executor_destroy(executor);
    counters_destroy(&c);
}

typedef struct ResultFixture {
    int result;
} ResultFixture;

static JceAsyncRunResult result_work(JceAsyncContext *ctx, void *user_data)
{
    ResultFixture *f = (ResultFixture *)user_data;
    jce_async_context_set_progress(ctx, 0.75f);
    TEST_ASSERT_TRUE(jce_async_context_set_result(ctx, &f->result, NULL));
    return JCE_ASYNC_RUN_SUCCESS;
}

static JceAsyncRunResult failure_work(JceAsyncContext *ctx, void *user_data)
{
    (void)user_data;
    jce_async_context_fail(ctx, 73, "decode failed");
    return JCE_ASYNC_RUN_FAILED;
}

static void test_result_error_and_progress_are_published(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    JceAsyncTaskDesc desc;
    JceAsyncTask *ok;
    JceAsyncTask *failed;
    ResultFixture f = { 42 };

    TEST_ASSERT_NOT_NULL(executor);

    jce_async_task_desc_init(&desc);
    desc.work = result_work;
    desc.user_data = &f;
    ok = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(ok);

    jce_async_task_desc_init(&desc);
    desc.work = failure_work;
    failed = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(failed);

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_PTR(&f.result, jce_async_task_result(ok));
    TEST_ASSERT_FLOAT_WITHIN(0.0001f, 0.75f,
                             jce_async_task_progress(ok));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_FAILED,
                          jce_async_task_state(failed));
    TEST_ASSERT_EQUAL_INT32(73, jce_async_task_error_code(failed));
    TEST_ASSERT_EQUAL_STRING("decode failed",
                             jce_async_task_error_message(failed));

    jce_async_task_release(ok);
    jce_async_task_release(failed);
    jce_async_executor_destroy(executor);
}

typedef struct BlockingFixture {
    JceSemaphore *started;
    JceSemaphore *release;
} BlockingFixture;

static JceAsyncRunResult blocking_work(JceAsyncContext *ctx, void *user_data)
{
    BlockingFixture *f = (BlockingFixture *)user_data;
    (void)ctx;
    jce_semaphore_signal(f->started);
    jce_semaphore_wait(f->release);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_wait_timeout_does_not_complete_running_work(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 1, 16);
    BlockingFixture f;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;

    TEST_ASSERT_NOT_NULL(executor);
    f.started = jce_semaphore_create(0);
    f.release = jce_semaphore_create(0);

    jce_async_task_desc_init(&desc);
    desc.work = blocking_work;
    desc.user_data = &f;
    task = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(f.started, 1000));

    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_TIMED_OUT,
        jce_async_task_wait_timeout(task, 5));
    jce_semaphore_signal(f.release);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
        jce_async_task_wait_timeout(task, 1000));

    pump_until_idle(executor);
    jce_async_task_release(task);
    jce_semaphore_destroy(f.started);
    jce_semaphore_destroy(f.release);
    jce_async_executor_destroy(executor);
}

static void test_cancelled_queued_task_never_enters_user_work(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncTask *task;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    task = submit_count(executor, &c, NULL);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_TRUE(jce_async_task_cancel(task));

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(task));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(c.work));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(c.complete));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(c.cleanup));

    jce_async_task_release(task);
    jce_async_executor_destroy(executor);
    counters_destroy(&c);
}

typedef struct CancelFixture {
    JceSemaphore *started;
    JceAtomicI32 *saw_cancel;
} CancelFixture;

static JceAsyncRunResult cancellable_work(JceAsyncContext *ctx,
                                          void *user_data)
{
    CancelFixture *f = (CancelFixture *)user_data;
    jce_semaphore_signal(f->started);
    while (!jce_async_context_cancel_requested(ctx))
        jce_thread_sleep_ms(1);
    jce_atomic_i32_store(f->saw_cancel, 1);
    return JCE_ASYNC_RUN_CANCELLED;
}

static void test_running_task_observes_cooperative_cancellation(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 1, 16);
    CancelFixture f;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;

    f.started = jce_semaphore_create(0);
    f.saw_cancel = jce_atomic_i32_create(0);
    jce_async_task_desc_init(&desc);
    desc.work = cancellable_work;
    desc.user_data = &f;
    task = jce_async_submit(executor, &desc);

    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(f.started, 1000));
    TEST_ASSERT_TRUE(jce_async_task_cancel(task));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
        jce_async_task_wait_timeout(task, 1000));
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(f.saw_cancel));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(task));

    pump_until_idle(executor);
    jce_async_task_release(task);
    jce_semaphore_destroy(f.started);
    jce_atomic_i32_destroy(f.saw_cancel);
    jce_async_executor_destroy(executor);
}

typedef struct OrderFixture {
    int stage;
    int child_ran;
} OrderFixture;

static JceAsyncRunResult parent_work(JceAsyncContext *ctx, void *user_data)
{
    OrderFixture *f = (OrderFixture *)user_data;
    (void)ctx;
    f->stage = 1;
    return JCE_ASYNC_RUN_SUCCESS;
}

static JceAsyncRunResult child_work(JceAsyncContext *ctx, void *user_data)
{
    OrderFixture *f = (OrderFixture *)user_data;
    (void)ctx;
    if (f->stage != 1)
        return JCE_ASYNC_RUN_FAILED;
    f->child_ran++;
    f->stage = 2;
    return JCE_ASYNC_RUN_SUCCESS;
}

static JceAsyncRunResult always_child_work(JceAsyncContext *ctx,
                                           void *user_data)
{
    OrderFixture *f = (OrderFixture *)user_data;
    (void)ctx;
    f->child_ran++;
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_dependencies_order_work_and_propagate_failure_policy(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 32);
    JceAsyncTaskDesc desc;
    JceAsyncTask *parent;
    JceAsyncTask *child;
    JceAsyncTask *failed;
    JceAsyncTask *blocked;
    JceAsyncTask *always;
    JceAsyncTask *deps[1];
    OrderFixture f = { 0, 0 };

    jce_async_task_desc_init(&desc);
    desc.work = parent_work;
    desc.user_data = &f;
    parent = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(parent);

    deps[0] = parent;
    jce_async_task_desc_init(&desc);
    desc.work = child_work;
    desc.user_data = &f;
    desc.dependencies = deps;
    desc.dependency_count = 1;
    desc.dependency_policy = JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS;
    child = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_WAITING,
                          jce_async_task_state(child));

    jce_async_task_desc_init(&desc);
    desc.work = failure_work;
    failed = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(failed);

    deps[0] = failed;
    jce_async_task_desc_init(&desc);
    desc.work = always_child_work;
    desc.user_data = &f;
    desc.dependencies = deps;
    desc.dependency_count = 1;
    desc.dependency_policy = JCE_ASYNC_DEPENDENCY_REQUIRE_SUCCESS;
    blocked = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(blocked);

    jce_async_task_desc_init(&desc);
    desc.work = always_child_work;
    desc.user_data = &f;
    desc.dependencies = deps;
    desc.dependency_count = 1;
    desc.dependency_policy = JCE_ASYNC_DEPENDENCY_ALWAYS_RUN;
    always = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(always);

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_SUCCEEDED,
                          jce_async_task_state(child));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(blocked));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_SUCCEEDED,
                          jce_async_task_state(always));
    TEST_ASSERT_EQUAL_INT(2, f.child_ran);

    jce_async_task_release(parent);
    jce_async_task_release(child);
    jce_async_task_release(failed);
    jce_async_task_release(blocked);
    jce_async_task_release(always);
    jce_async_executor_destroy(executor);
}

static void test_group_cancel_propagates_and_waits_for_scope(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncGroup *group;
    JceAsyncTask *a;
    JceAsyncTask *b;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    group = jce_async_group_create();
    TEST_ASSERT_NOT_NULL(group);

    a = submit_count(executor, &c, group);
    b = submit_count(executor, &c, group);
    TEST_ASSERT_EQUAL_UINT32(2, jce_async_group_pending(group));
    TEST_ASSERT_TRUE(jce_async_group_cancel(group));

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
        jce_async_group_wait_timeout(group, 100));
    TEST_ASSERT_EQUAL_UINT32(0, jce_async_group_pending(group));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(c.work));

    jce_async_task_release(a);
    jce_async_task_release(b);
    jce_async_group_release(group);
    jce_async_executor_destroy(executor);
    counters_destroy(&c);
}

static void test_capacity_applies_backpressure_and_recovers_after_reap(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncTask *first;
    JceAsyncTask *rejected;
    JceAsyncTask *after;
    JceAsyncExecutorStats stats;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 1);
    first = submit_count(executor, &c, NULL);
    rejected = submit_count(executor, &c, NULL);
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NULL(rejected);

    jce_async_executor_get_stats(executor, &stats);
    TEST_ASSERT_EQUAL_UINT64(1, stats.rejected_tasks);

    pump_until_idle(executor);
    after = submit_count(executor, &c, NULL);
    TEST_ASSERT_NOT_NULL(after);
    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(2, jce_atomic_i32_load(c.cleanup));

    jce_async_task_release(first);
    jce_async_task_release(after);
    jce_async_executor_destroy(executor);
    counters_destroy(&c);
}

typedef struct PriorityFixture {
    int *order;
    int *next;
    int  value;
} PriorityFixture;

static JceAsyncRunResult priority_work(JceAsyncContext *ctx,
                                       void *user_data)
{
    PriorityFixture *f = (PriorityFixture *)user_data;
    (void)ctx;
    f->order[(*f->next)++] = f->value;
    return JCE_ASYNC_RUN_SUCCESS;
}

static JceAsyncTask *submit_priority(JceAsyncExecutor *executor,
                                     PriorityFixture *fixture,
                                     JceAsyncPriority priority)
{
    JceAsyncTaskDesc desc;

    jce_async_task_desc_init(&desc);
    desc.work = priority_work;
    desc.user_data = fixture;
    desc.priority = priority;
    return jce_async_submit(executor, &desc);
}

static void test_cooperative_executor_runs_higher_priorities_first(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    JceAsyncTask *background;
    JceAsyncTask *critical;
    JceAsyncTask *normal;
    JceAsyncPumpBudget budget;
    PriorityFixture fixtures[3];
    int order[3] = { 0, 0, 0 };
    int next = 0;

    fixtures[0].order = order;
    fixtures[0].next = &next;
    fixtures[0].value = 3;
    fixtures[1].order = order;
    fixtures[1].next = &next;
    fixtures[1].value = 1;
    fixtures[2].order = order;
    fixtures[2].next = &next;
    fixtures[2].value = 2;

    background = submit_priority(
        executor, &fixtures[0], JCE_ASYNC_PRIORITY_BACKGROUND);
    critical = submit_priority(
        executor, &fixtures[1], JCE_ASYNC_PRIORITY_CRITICAL);
    normal = submit_priority(
        executor, &fixtures[2], JCE_ASYNC_PRIORITY_NORMAL);
    TEST_ASSERT_NOT_NULL(background);
    TEST_ASSERT_NOT_NULL(critical);
    TEST_ASSERT_NOT_NULL(normal);

    jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1;
    budget.max_completions = 0;
    budget.max_time_us = 0;
    TEST_ASSERT_EQUAL_UINT32(1,
        jce_async_executor_pump(executor, &budget));
    TEST_ASSERT_EQUAL_INT(1, order[0]);
    TEST_ASSERT_EQUAL_UINT32(1,
        jce_async_executor_pump(executor, &budget));
    TEST_ASSERT_EQUAL_INT(2, order[1]);
    TEST_ASSERT_EQUAL_UINT32(1,
        jce_async_executor_pump(executor, &budget));
    TEST_ASSERT_EQUAL_INT(3, order[2]);

    pump_until_idle(executor);
    jce_async_task_release(background);
    jce_async_task_release(critical);
    jce_async_task_release(normal);
    jce_async_executor_destroy(executor);
}

typedef struct LatencyLaneFixture {
    JceSemaphore *background_started;
    JceSemaphore *background_release;
    JceSemaphore *high_finished;
} LatencyLaneFixture;

static JceAsyncRunResult latency_background_work(JceAsyncContext *ctx,
                                                 void *user_data)
{
    LatencyLaneFixture *f = (LatencyLaneFixture *)user_data;
    (void)ctx;
    jce_semaphore_signal(f->background_started);
    jce_semaphore_wait(f->background_release);
    return JCE_ASYNC_RUN_SUCCESS;
}

static JceAsyncRunResult latency_high_work(JceAsyncContext *ctx,
                                           void *user_data)
{
    LatencyLaneFixture *f = (LatencyLaneFixture *)user_data;
    (void)ctx;
    jce_semaphore_signal(f->high_finished);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_background_work_preserves_a_latency_lane(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 2, 16);
    LatencyLaneFixture f;
    JceAsyncTaskDesc desc;
    JceAsyncTask *background[2] = { NULL, NULL };
    JceAsyncTask *high = NULL;
    bool high_finished = false;
    int i;

    TEST_ASSERT_NOT_NULL(executor);
    f.background_started = jce_semaphore_create(0);
    f.background_release = jce_semaphore_create(0);
    f.high_finished = jce_semaphore_create(0);
    TEST_ASSERT_NOT_NULL(f.background_started);
    TEST_ASSERT_NOT_NULL(f.background_release);
    TEST_ASSERT_NOT_NULL(f.high_finished);

    jce_async_task_desc_init(&desc);
    desc.work = latency_background_work;
    desc.user_data = &f;
    desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;
    for (i = 0; i < 2; ++i) {
        background[i] = jce_async_submit(executor, &desc);
        TEST_ASSERT_NOT_NULL(background[i]);
    }
    TEST_ASSERT_TRUE(
        jce_semaphore_wait_timeout(f.background_started, 1000));

    jce_async_task_desc_init(&desc);
    desc.work = latency_high_work;
    desc.user_data = &f;
    desc.priority = JCE_ASYNC_PRIORITY_HIGH;
    high = jce_async_submit(executor, &desc);
    if (high)
        high_finished =
            jce_semaphore_wait_timeout(f.high_finished, 250);

    jce_semaphore_signal(f.background_release);
    jce_semaphore_signal(f.background_release);
    for (i = 0; i < 2; ++i) {
        if (background[i])
            jce_async_task_wait(background[i]);
    }
    if (high)
        jce_async_task_wait(high);
    pump_until_idle(executor);

    for (i = 0; i < 2; ++i)
        jce_async_task_release(background[i]);
    jce_async_task_release(high);
    jce_semaphore_destroy(f.background_started);
    jce_semaphore_destroy(f.background_release);
    jce_semaphore_destroy(f.high_finished);
    jce_async_executor_destroy(executor);

    TEST_ASSERT_NOT_NULL(high);
    TEST_ASSERT_TRUE_MESSAGE(
        high_finished,
        "background tasks consumed every worker and starved high priority");
}

static void test_dedicated_throughput_executor_can_use_every_worker(void)
{
    JceAsyncExecutorConfig config;
    JceAsyncExecutor *executor;
    LatencyLaneFixture f;
    JceAsyncTaskDesc desc;
    JceAsyncTask *tasks[2] = { NULL, NULL };
    bool both_started;
    int i;

    jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_THREADED;
    config.worker_count = 2;
    config.max_tasks = 16;
    config.reserve_latency_worker = false;
    executor = jce_async_executor_create(&config);
    TEST_ASSERT_NOT_NULL(executor);

    f.background_started = jce_semaphore_create(0);
    f.background_release = jce_semaphore_create(0);
    f.high_finished = NULL;
    TEST_ASSERT_NOT_NULL(f.background_started);
    TEST_ASSERT_NOT_NULL(f.background_release);

    jce_async_task_desc_init(&desc);
    desc.work = latency_background_work;
    desc.user_data = &f;
    desc.priority = JCE_ASYNC_PRIORITY_BACKGROUND;
    for (i = 0; i < 2; ++i) {
        tasks[i] = jce_async_submit(executor, &desc);
        TEST_ASSERT_NOT_NULL(tasks[i]);
    }

    both_started =
        jce_semaphore_wait_timeout(f.background_started, 1000) &&
        jce_semaphore_wait_timeout(f.background_started, 1000);
    jce_semaphore_signal(f.background_release);
    jce_semaphore_signal(f.background_release);
    for (i = 0; i < 2; ++i)
        jce_async_task_wait(tasks[i]);
    pump_until_idle(executor);

    for (i = 0; i < 2; ++i)
        jce_async_task_release(tasks[i]);
    jce_semaphore_destroy(f.background_started);
    jce_semaphore_destroy(f.background_release);
    jce_async_executor_destroy(executor);

    TEST_ASSERT_TRUE_MESSAGE(
        both_started,
        "dedicated throughput executor did not use every worker");
}

static JceAsyncRunResult timed_work(JceAsyncContext *ctx, void *user_data)
{
    (void)ctx;
    (void)user_data;
    jce_thread_sleep_ms(5);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_threaded_executor_reports_queue_and_run_telemetry(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 2, 16);
    JceAsyncTaskDesc desc;
    JceAsyncExecutorStats stats;
    JceAsyncTask *task;

    TEST_ASSERT_NOT_NULL(executor);
    jce_async_task_desc_init(&desc);
    desc.work = timed_work;
    task = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(task);

    jce_async_task_wait(task);
    pump_until_idle(executor);
    jce_async_executor_get_stats(executor, &stats);

    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.peak_live_tasks);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(1, stats.peak_queued_tasks);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(1000000, stats.total_run_time_ns);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(stats.max_run_time_ns,
                                        stats.total_run_time_ns);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(stats.max_queue_time_ns,
                                        stats.total_queue_time_ns);

    jce_async_task_release(task);
    jce_async_executor_destroy(executor);
}

typedef struct ResultLifetimeFixture {
    JceAtomicI32 *destroy_count;
} ResultLifetimeFixture;

static void count_result_destroy(void *result)
{
    ResultLifetimeFixture *fixture =
        (ResultLifetimeFixture *)result;
    jce_atomic_i32_add(fixture->destroy_count, 1);
}

static JceAsyncRunResult result_lifetime_work(JceAsyncContext *ctx,
                                              void *user_data)
{
    return jce_async_context_set_result(
        ctx, user_data, count_result_destroy)
        ? JCE_ASYNC_RUN_SUCCESS : JCE_ASYNC_RUN_FAILED;
}

static void test_result_lives_with_handle_and_is_destroyed_exactly_once(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    ResultLifetimeFixture fixture;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;

    fixture.destroy_count = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(fixture.destroy_count);
    jce_async_task_desc_init(&desc);
    desc.work = result_lifetime_work;
    desc.user_data = &fixture;
    task = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(task);

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_PTR(&fixture, jce_async_task_result(task));
    jce_async_executor_destroy(executor);
    TEST_ASSERT_EQUAL_INT(0,
                          jce_atomic_i32_load(fixture.destroy_count));

    jce_async_task_release(task);
    TEST_ASSERT_EQUAL_INT(1,
                          jce_atomic_i32_load(fixture.destroy_count));
    jce_atomic_i32_destroy(fixture.destroy_count);
}

static void test_invalid_worker_count_is_rejected(void)
{
#if JCE_PLATFORM_WEB
    TEST_IGNORE_MESSAGE("Web forces cooperative execution");
#else
    JceAsyncExecutorConfig config;

    jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_THREADED;
    config.worker_count = 17;
    TEST_ASSERT_NULL(jce_async_executor_create(&config));
#endif
}

static void test_worker_request_obeys_process_budget(void)
{
#if JCE_PLATFORM_WEB
    TEST_IGNORE_MESSAGE("Web forces cooperative execution");
#else
    JceAsyncExecutorConfig config;
    JceAsyncExecutorStats stats;
    JceAsyncExecutor *executor;
    int expected = jce_thread_pool_default_workers();

    /* Mirrors the cap in async_process_worker_budget().  Duplicating the
     * constant here is why this test failed when the cap moved 4 -> 8 on
     * 2026-09-03: the CONTRACT under test -- "a request of 16 is clamped to
     * the process budget" -- never changed.  There is no public accessor for
     * the budget, so the copy stays; keep the two in step. */
    if (expected > 8)
        expected = 8;

    jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_THREADED;
    config.worker_count = 16;
    executor = jce_async_executor_create(&config);
    TEST_ASSERT_NOT_NULL(executor);

    jce_async_executor_get_stats(executor, &stats);
    TEST_ASSERT_EQUAL_UINT32((uint32_t)expected, stats.worker_count);
    jce_async_executor_destroy(executor);
#endif
}

static JceAsyncRunResult default_no_op_work(JceAsyncContext *ctx,
                                             void *user_data)
{
    (void)ctx;
    (void)user_data;
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_process_default_executor_can_shutdown_and_recreate(void)
{
    JceAsyncExecutor *first;
    JceAsyncExecutor *second;
    JceAsyncExecutorStats stats;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;

    TEST_ASSERT_TRUE(jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE));
    first = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_TRUE(jce_async_executor_is_owner(first));

    jce_async_task_desc_init(&desc);
    desc.work = default_no_op_work;
    task = jce_async_submit(first, &desc);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
        jce_async_task_wait_timeout(task, 2000));
    pump_until_idle(first);
    jce_async_task_release(task);
    TEST_ASSERT_TRUE(jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE));

    second = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_TRUE(jce_async_executor_is_owner(second));
    jce_async_executor_get_stats(second, &stats);
    TEST_ASSERT_EQUAL_UINT64(0, stats.submitted_tasks);
    TEST_ASSERT_EQUAL_UINT32(0, stats.live_tasks);
    TEST_ASSERT_TRUE(jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE));
}

typedef struct DefaultShutdownProbe {
    JceSemaphore *release;
    JceAtomicI32 *observed_closing;
} DefaultShutdownProbe;

static void default_shutdown_probe_thread(void *user_data)
{
    DefaultShutdownProbe *probe = (DefaultShutdownProbe *)user_data;
    int i;

    for (i = 0; i < 500; ++i) {
        if (!jce_async_default_executor()) {
            jce_atomic_i32_store(probe->observed_closing, 1);
            break;
        }
        jce_thread_sleep_ms(1);
    }
    jce_semaphore_signal(probe->release);
}

static void test_default_executor_is_not_published_while_closing(void)
{
    JceAsyncExecutor *executor;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;
    BlockingFixture blocker;
    DefaultShutdownProbe probe;
    JceThread *thread;

    TEST_ASSERT_TRUE(jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE));
    executor = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(executor);

    blocker.started = jce_semaphore_create(0);
    blocker.release = jce_semaphore_create(0);
    probe.release = blocker.release;
    probe.observed_closing = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(blocker.started);
    TEST_ASSERT_NOT_NULL(blocker.release);
    TEST_ASSERT_NOT_NULL(probe.observed_closing);

    jce_async_task_desc_init(&desc);
    desc.work = blocking_work;
    desc.user_data = &blocker;
    task = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(task);
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(blocker.started, 1000));

    thread = jce_thread_create(default_shutdown_probe_thread, &probe,
                               "async-close-probe");
    TEST_ASSERT_NOT_NULL(thread);
    TEST_ASSERT_TRUE(jce_async_default_shutdown(
        JCE_ASYNC_SHUTDOWN_CANCEL_ALL, JCE_ASYNC_WAIT_INFINITE));
    jce_thread_join(thread);

    TEST_ASSERT_EQUAL_INT(1,
        jce_atomic_i32_load(probe.observed_closing));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(task));

    jce_async_task_release(task);
    jce_semaphore_destroy(blocker.started);
    jce_semaphore_destroy(blocker.release);
    jce_atomic_i32_destroy(probe.observed_closing);
}

typedef struct ProducerFixture {
    JceAsyncExecutor *executor;
    JceAsyncTask     **tasks;
    uint32_t           count;
    JceAtomicI32      *work_count;
    JceAtomicI32      *submit_failures;
} ProducerFixture;

static JceAsyncRunResult producer_count_work(JceAsyncContext *ctx,
                                             void *user_data)
{
    (void)ctx;
    jce_atomic_i32_add((JceAtomicI32 *)user_data, 1);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void producer_thread(void *user_data)
{
    ProducerFixture *f = (ProducerFixture *)user_data;
    uint32_t i;

    for (i = 0; i < f->count; ++i) {
        JceAsyncTaskDesc desc;
        jce_async_task_desc_init(&desc);
        desc.work = producer_count_work;
        desc.user_data = f->work_count;
        f->tasks[i] = jce_async_submit(f->executor, &desc);
        if (!f->tasks[i])
            jce_atomic_i32_add(f->submit_failures, 1);
    }
}

static void test_submission_is_safe_from_concurrent_foreign_threads(void)
{
    enum { PRODUCERS = 4, TASKS_PER_PRODUCER = 24 };
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 2, 256);
    JceThread *threads[PRODUCERS];
    ProducerFixture fixtures[PRODUCERS];
    JceAsyncTask *tasks[PRODUCERS][TASKS_PER_PRODUCER] = { { NULL } };
    JceAtomicI32 *work_count = jce_atomic_i32_create(0);
    JceAtomicI32 *failures = jce_atomic_i32_create(0);
    int p;
    int i;

    TEST_ASSERT_NOT_NULL(executor);
    for (p = 0; p < PRODUCERS; ++p) {
        fixtures[p].executor = executor;
        fixtures[p].tasks = tasks[p];
        fixtures[p].count = TASKS_PER_PRODUCER;
        fixtures[p].work_count = work_count;
        fixtures[p].submit_failures = failures;
        threads[p] = jce_thread_create(producer_thread, &fixtures[p],
                                       "async-producer");
        TEST_ASSERT_NOT_NULL(threads[p]);
    }
    for (p = 0; p < PRODUCERS; ++p)
        jce_thread_join(threads[p]);

    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(failures));
    for (p = 0; p < PRODUCERS; ++p) {
        for (i = 0; i < TASKS_PER_PRODUCER; ++i) {
            TEST_ASSERT_NOT_NULL(tasks[p][i]);
            TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
                jce_async_task_wait_timeout(tasks[p][i], 2000));
        }
    }

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(PRODUCERS * TASKS_PER_PRODUCER,
                          jce_atomic_i32_load(work_count));
    for (p = 0; p < PRODUCERS; ++p)
        for (i = 0; i < TASKS_PER_PRODUCER; ++i)
            jce_async_task_release(tasks[p][i]);

    jce_atomic_i32_destroy(work_count);
    jce_atomic_i32_destroy(failures);
    jce_async_executor_destroy(executor);
}

typedef struct NestedWaitFixture {
    JceAsyncExecutor *executor;
    JceAtomicI32      *wait_result;
} NestedWaitFixture;

static JceAsyncRunResult no_op_work(JceAsyncContext *ctx, void *user_data)
{
    (void)ctx;
    (void)user_data;
    return JCE_ASYNC_RUN_SUCCESS;
}

static JceAsyncRunResult nested_wait_work(JceAsyncContext *ctx,
                                          void *user_data)
{
    NestedWaitFixture *f = (NestedWaitFixture *)user_data;
    JceAsyncTaskDesc desc;
    JceAsyncTask *child;
    JceAsyncWaitResult result;
    (void)ctx;

    jce_async_task_desc_init(&desc);
    desc.work = no_op_work;
    child = jce_async_submit(f->executor, &desc);
    if (!child)
        return JCE_ASYNC_RUN_FAILED;
    result = jce_async_task_wait_timeout(child, 100);
    jce_atomic_i32_store(f->wait_result, (int32_t)result);
    jce_async_task_release(child);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_worker_wait_on_same_executor_reports_deadlock(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 1, 16);
    NestedWaitFixture f;
    JceAsyncTaskDesc desc;
    JceAsyncTask *parent;

    f.executor = executor;
    f.wait_result = jce_atomic_i32_create(-1);
    jce_async_task_desc_init(&desc);
    desc.work = nested_wait_work;
    desc.user_data = &f;
    parent = jce_async_submit(executor, &desc);

    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
        jce_async_task_wait_timeout(parent, 1000));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_WOULD_DEADLOCK,
                          jce_atomic_i32_load(f.wait_result));

    pump_until_idle(executor);
    jce_async_task_release(parent);
    jce_atomic_i32_destroy(f.wait_result);
    jce_async_executor_destroy(executor);
}

typedef struct CompletionWaitFixture {
    JceAsyncTask *other;
    JceAtomicI32 *wait_result;
} CompletionWaitFixture;

static void completion_wait_other(JceAsyncTask *task, void *user_data)
{
    CompletionWaitFixture *f = (CompletionWaitFixture *)user_data;
    JceAsyncWaitResult result;
    (void)task;

    result = jce_async_task_wait_timeout(f->other, 100);
    jce_atomic_i32_store(f->wait_result, (int32_t)result);
}

static void test_completion_wait_on_same_executor_reports_deadlock(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    CompletionWaitFixture f;
    JceAsyncTaskDesc desc;
    JceAsyncTask *first;
    JceAsyncTask *second;
    JceAsyncPumpBudget budget;

    TEST_ASSERT_NOT_NULL(executor);
    f.other = NULL;
    f.wait_result = jce_atomic_i32_create(-1);

    jce_async_task_desc_init(&desc);
    desc.work = no_op_work;
    desc.complete = completion_wait_other;
    desc.user_data = &f;
    first = jce_async_submit(executor, &desc);

    jce_async_task_desc_init(&desc);
    desc.work = no_op_work;
    second = jce_async_submit(executor, &desc);
    f.other = second;

    jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1;
    budget.max_completions = 1;
    budget.max_time_us = 0;
    jce_async_executor_pump(executor, &budget);

    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_WOULD_DEADLOCK,
                          jce_atomic_i32_load(f.wait_result));
    TEST_ASSERT_FALSE(jce_async_task_is_terminal(second));

    pump_until_idle(executor);
    jce_async_task_release(first);
    jce_async_task_release(second);
    jce_atomic_i32_destroy(f.wait_result);
    jce_async_executor_destroy(executor);
}

static void test_deadline_cancels_before_cooperative_work_starts(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    jce_async_task_desc_init(&desc);
    desc.work = count_work;
    desc.user_data = &c;
    desc.timeout_ms = 1;
    task = jce_async_submit(executor, &desc);
    jce_thread_sleep_ms(3);

    pump_until_idle(executor);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(task));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(c.work));

    jce_async_task_release(task);
    jce_async_executor_destroy(executor);
    counters_destroy(&c);
}

static void test_waiting_dependency_deadline_is_cancelled_by_owner_pump(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 2, 16);
    BlockingFixture blocker;
    TestCounters c;
    JceAsyncTaskDesc desc;
    JceAsyncTask *parent;
    JceAsyncTask *child;
    JceAsyncTask *deps[1];
    JceAsyncWaitResult child_wait;

    TEST_ASSERT_NOT_NULL(executor);
    counters_init(&c);
    blocker.started = jce_semaphore_create(0);
    blocker.release = jce_semaphore_create(0);

    jce_async_task_desc_init(&desc);
    desc.work = blocking_work;
    desc.user_data = &blocker;
    parent = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(parent);
    TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(blocker.started, 1000));

    deps[0] = parent;
    jce_async_task_desc_init(&desc);
    desc.work = count_work;
    desc.user_data = &c;
    desc.dependencies = deps;
    desc.dependency_count = 1;
    desc.timeout_ms = 5;
    child = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(child);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_WAITING,
                          jce_async_task_state(child));

    jce_thread_sleep_ms(10);
    jce_async_executor_pump(executor, NULL);
    child_wait = jce_async_task_wait_timeout(child, 100);

    jce_semaphore_signal(blocker.release);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED,
        jce_async_task_wait_timeout(parent, 1000));
    pump_until_idle(executor);

    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_WAIT_COMPLETED, child_wait);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(child));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(c.work));

    jce_async_task_release(parent);
    jce_async_task_release(child);
    jce_semaphore_destroy(blocker.started);
    jce_semaphore_destroy(blocker.release);
    counters_destroy(&c);
    jce_async_executor_destroy(executor);
}

static void test_cooperative_pump_honours_work_item_budget(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncTask *tasks[3];
    JceAsyncPumpBudget budget;
    int i;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    for (i = 0; i < 3; ++i)
        tasks[i] = submit_count(executor, &c, NULL);

    jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1;
    budget.max_completions = UINT32_MAX;
    budget.max_time_us = 0;
    jce_async_executor_pump(executor, &budget);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(c.work));

    pump_until_idle(executor);
    for (i = 0; i < 3; ++i)
        jce_async_task_release(tasks[i]);
    jce_async_executor_destroy(executor);
    counters_destroy(&c);
}

static void test_shutdown_cancels_pending_and_handle_outlives_executor(void)
{
    TestCounters c;
    JceAsyncExecutor *executor;
    JceAsyncTask *task;

    counters_init(&c);
    executor = make_executor(JCE_ASYNC_EXECUTION_COOPERATIVE, 0, 16);
    task = submit_count(executor, &c, NULL);
    TEST_ASSERT_NOT_NULL(task);

    TEST_ASSERT_TRUE(jce_async_executor_shutdown(
        executor, JCE_ASYNC_SHUTDOWN_CANCEL_PENDING,
        JCE_ASYNC_WAIT_INFINITE));
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(task));
    TEST_ASSERT_EQUAL_INT(0, jce_atomic_i32_load(c.work));

    jce_async_executor_destroy(executor);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_STATE_CANCELLED,
                          jce_async_task_state(task));
    jce_async_task_release(task);
    counters_destroy(&c);
}

/* The teardown idiom used by seven production sites:
 *   cancel -> wait -> release -> free the job the completion reads.
 * The wait returns on TERMINAL STATE, and terminal state is set before the
 * completion is queued, not after it runs.  So the callback is still ahead
 * of the caller when the caller frees its user_data.  This test pins the
 * behaviour down without dereferencing freed memory: it flags the moment the
 * owner would have freed, and asks whether the callback ran after it. */
typedef struct { int torn_down; int ran_after_teardown; } TeardownProbe;

static void teardown_complete(JceAsyncTask *task, void *user_data)
{
    TeardownProbe *p = (TeardownProbe *)user_data;
    (void)task;
    if (p->torn_down) p->ran_after_teardown = 1;
}

static JceAsyncRunResult teardown_work(JceAsyncContext *ctx, void *arg)
{
    (void)ctx; (void)arg;
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_discard_disarms_the_completion_before_the_owner_frees(void)
{
    JceAsyncExecutor *executor =
        make_executor(JCE_ASYNC_EXECUTION_THREADED, 1, 16);
    TeardownProbe probe = {0, 0};
    JceAsyncTaskDesc desc;
    JceAsyncTask *task;

    TEST_ASSERT_NOT_NULL(executor);
    jce_async_task_desc_init(&desc);
    desc.work       = teardown_work;
    desc.complete   = teardown_complete;
    desc.user_data  = &probe;
    desc.debug_name = "teardown.probe";
    task = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(task);

    TEST_ASSERT_TRUE(jce_async_task_discard(task));

    probe.torn_down = 1;   /* the production sites free the job right here */
    pump_until_idle(executor);

    TEST_ASSERT_EQUAL_INT(0, probe.ran_after_teardown);
    jce_async_executor_destroy(executor);
}

int main(void)
{
    jce_thread_mark_main();
    UNITY_BEGIN();
    RUN_TEST(test_submit_is_not_inline_and_completion_is_owner_pumped);
    RUN_TEST(test_result_error_and_progress_are_published);
    RUN_TEST(test_wait_timeout_does_not_complete_running_work);
    RUN_TEST(test_cancelled_queued_task_never_enters_user_work);
    RUN_TEST(test_running_task_observes_cooperative_cancellation);
    RUN_TEST(test_dependencies_order_work_and_propagate_failure_policy);
    RUN_TEST(test_group_cancel_propagates_and_waits_for_scope);
    RUN_TEST(test_capacity_applies_backpressure_and_recovers_after_reap);
    RUN_TEST(test_cooperative_executor_runs_higher_priorities_first);
    RUN_TEST(test_background_work_preserves_a_latency_lane);
    RUN_TEST(test_dedicated_throughput_executor_can_use_every_worker);
    RUN_TEST(test_threaded_executor_reports_queue_and_run_telemetry);
    RUN_TEST(test_result_lives_with_handle_and_is_destroyed_exactly_once);
    RUN_TEST(test_invalid_worker_count_is_rejected);
    RUN_TEST(test_worker_request_obeys_process_budget);
    RUN_TEST(test_process_default_executor_can_shutdown_and_recreate);
    RUN_TEST(test_default_executor_is_not_published_while_closing);
    RUN_TEST(test_submission_is_safe_from_concurrent_foreign_threads);
    RUN_TEST(test_worker_wait_on_same_executor_reports_deadlock);
    RUN_TEST(test_completion_wait_on_same_executor_reports_deadlock);
    RUN_TEST(test_deadline_cancels_before_cooperative_work_starts);
    RUN_TEST(test_waiting_dependency_deadline_is_cancelled_by_owner_pump);
    RUN_TEST(test_cooperative_pump_honours_work_item_budget);
    RUN_TEST(test_shutdown_cancels_pending_and_handle_outlives_executor);
    RUN_TEST(test_discard_disarms_the_completion_before_the_owner_frees);
    return UNITY_END();
}
