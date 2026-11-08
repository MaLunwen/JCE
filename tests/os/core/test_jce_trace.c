/*
 * test_jce_trace.c - Runtime observability contract.
 */

#include "unity.h"

#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>

#include <string.h>
#include <SDL3/SDL_thread.h>

void setUp(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
}

static const JceTraceEvent *find_event(const JceTraceEvent *events,
                                       uint32_t count,
                                       JceTraceEventType type,
                                       const char *name)
{
    uint32_t i;

    for (i = 0; i < count; ++i) {
        if (events[i].type == type &&
            (!name || strcmp(events[i].name, name) == 0))
            return &events[i];
    }
    return NULL;
}

static void test_disabled_trace_has_zero_producer_cost_visible_to_consumer(void)
{
    JceTraceCursor cursor;
    JceTraceEvent event;

    jce_trace_set_enabled(false);
    jce_trace_reset();
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "disabled", 1);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
}

static void test_lifecycle_events_preserve_ids_names_and_timing(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[16];
    JceTraceStats stats;
    uint64_t task_id = jce_trace_next_id();
    uint64_t span_id = jce_trace_next_id();
    uint64_t wait_id = jce_trace_next_id();
    uint32_t count;

    jce_trace_thread_register("test-main");
    jce_trace_task_submit(task_id, 0, JCE_TRACE_TASK_ASYNC, "load-model");
    jce_trace_task_begin(span_id, task_id, "load-model", 1500);
    jce_trace_wait_begin(wait_id, task_id, "gpu-upload");
    jce_trace_wait_end(wait_id, task_id, "gpu-upload", 2300);
    jce_trace_task_end(span_id, task_id, "load-model",
                       JCE_TRACE_TASK_SUCCEEDED, 4700);
    jce_trace_frame_mark(42, 16666667, 16000000);

    jce_trace_cursor_init(&cursor);
    count = jce_trace_read(&cursor, events, 16);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT32(7, count);

    {
        const JceTraceEvent *e = find_event(
            events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "load-model");
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT64(task_id, e->id);
        TEST_ASSERT_EQUAL_UINT64(0, e->parent_id);
        TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_ASYNC, e->value_u32[0]);
    }
    {
        const JceTraceEvent *e = find_event(
            events, count, JCE_TRACE_EVENT_TASK_BEGIN, "load-model");
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT64(span_id, e->id);
        TEST_ASSERT_EQUAL_UINT64(task_id, e->parent_id);
        TEST_ASSERT_EQUAL_UINT64(1500, e->value_u64);
    }
    {
        const JceTraceEvent *e = find_event(
            events, count, JCE_TRACE_EVENT_FRAME, NULL);
        TEST_ASSERT_NOT_NULL(e);
        TEST_ASSERT_EQUAL_UINT64(42, e->id);
        TEST_ASSERT_EQUAL_UINT64(16666667, e->value_u64);
        TEST_ASSERT_EQUAL_UINT32(16000000, e->value_u32[0]);
    }

    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT64(1, stats.tasks_submitted);
    TEST_ASSERT_EQUAL_UINT64(1, stats.work_items_completed);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(1500, stats.max_queue_time_ns);
    TEST_ASSERT_EQUAL_UINT64(4700, stats.max_run_time_ns);
    TEST_ASSERT_EQUAL_UINT64(2300, stats.max_wait_time_ns);
    TEST_ASSERT_EQUAL_UINT64(16666667, stats.last_wall_frame_ns);
}

static void test_stats_preserve_multi_second_hitches_without_int_clamping(void)
{
    const uint64_t queue_ns = UINT64_C(5000000123);
    const uint64_t run_ns = UINT64_C(7000000456);
    const uint64_t wait_ns = UINT64_C(9000000789);
    JceTraceStats stats;
    uint64_t task_id = jce_trace_next_id();
    uint64_t span_id = jce_trace_next_id();
    uint64_t wait_id = jce_trace_next_id();

    jce_trace_task_submit(task_id, 0, JCE_TRACE_TASK_ASYNC, "long-task");
    jce_trace_task_begin(span_id, task_id, "long-task", queue_ns);
    jce_trace_wait_begin(wait_id, task_id, "long-wait");
    jce_trace_wait_end(wait_id, task_id, "long-wait", wait_ns);
    jce_trace_task_end(span_id, task_id, "long-task",
                       JCE_TRACE_TASK_SUCCEEDED, run_ns);

    jce_trace_get_stats(&stats);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(UINT64_C(5000000000),
                                        stats.max_queue_time_ns);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(UINT64_C(7000000000),
                                        stats.max_run_time_ns);
    TEST_ASSERT_GREATER_OR_EQUAL_UINT64(UINT64_C(9000000000),
                                        stats.max_wait_time_ns);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(queue_ns, stats.max_queue_time_ns);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(run_ns, stats.max_run_time_ns);
    TEST_ASSERT_LESS_OR_EQUAL_UINT64(wait_ns, stats.max_wait_time_ns);
}

typedef struct TraceProducer {
    int index;
    int count;
} TraceProducer;

static void trace_producer(void *arg)
{
    TraceProducer *producer = (TraceProducer *)arg;
    int i;
    char name[32];

    name[0] = 'p';
    name[1] = (char)('0' + producer->index);
    name[2] = '\0';
    jce_trace_thread_register(name);
    for (i = 0; i < producer->count; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_TASK, name, i);
    jce_trace_thread_unregister();
}

static void test_multiple_producers_publish_complete_events(void)
{
    enum { PRODUCERS = 4, EVENTS_PER_PRODUCER = 128 };
    TraceProducer producers[PRODUCERS];
    JceThread *threads[PRODUCERS];
    JceTraceCursor cursor;
    JceTraceEvent events[PRODUCERS * (EVENTS_PER_PRODUCER + 2)];
    uint32_t count;
    uint32_t counters = 0;
    int i;

    for (i = 0; i < PRODUCERS; ++i) {
        producers[i].index = i;
        producers[i].count = EVENTS_PER_PRODUCER;
        threads[i] = jce_thread_create(trace_producer, &producers[i],
                                       "trace-producer");
        TEST_ASSERT_NOT_NULL(threads[i]);
    }
    for (i = 0; i < PRODUCERS; ++i)
        jce_thread_join(threads[i]);

    jce_trace_cursor_init(&cursor);
    count = jce_trace_read(&cursor, events,
                           (uint32_t)(sizeof(events) / sizeof(events[0])));
    for (i = 0; i < (int)count; ++i) {
        if (events[i].type == JCE_TRACE_EVENT_COUNTER)
            counters++;
    }
    TEST_ASSERT_EQUAL_UINT32(PRODUCERS * EVENTS_PER_PRODUCER, counters);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
}

static void increment_task(void *arg)
{
    int *value = (int *)arg;
    (*value)++;
}

static void test_named_pool_task_emits_submit_run_and_wait_events(void)
{
    JceThreadPool *pool = jce_thread_pool_create_named(2, "trace-pool");
    JceTask *task;
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    uint32_t count;
    int value = 0;

    TEST_ASSERT_NOT_NULL(pool);
    task = jce_thread_pool_submit_tracked_named(
        pool, "named-frame-job", increment_task, &value);
    TEST_ASSERT_NOT_NULL(task);
    jce_task_wait(task);
    jce_task_free(task);
    jce_thread_pool_destroy(pool);
    TEST_ASSERT_EQUAL_INT(1, value);

    jce_trace_cursor_init(&cursor);
    count = jce_trace_read(&cursor, events, 32);
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "named-frame-job"));
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_TASK_BEGIN, "named-frame-job"));
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_TASK_END, "named-frame-job"));
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_WAIT_BEGIN, "named-frame-job"));
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_WAIT_END, "named-frame-job"));
}

static JceAsyncRunResult async_work(JceAsyncContext *ctx, void *user_data)
{
    int *value = (int *)user_data;

    (void)ctx;
    (*value)++;
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_structured_async_uses_same_trace_contract(void)
{
    JceAsyncExecutorConfig config;
    JceAsyncTaskDesc desc;
    JceAsyncExecutor *executor;
    JceAsyncTask *task;
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    uint32_t count;
    int value = 0;

    jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_THREADED;
    config.worker_count = 1;
    config.debug_name = "trace-async-executor";
    executor = jce_async_executor_create(&config);
    TEST_ASSERT_NOT_NULL(executor);

    jce_async_task_desc_init(&desc);
    desc.work = async_work;
    desc.user_data = &value;
    desc.debug_name = "trace-async-task";
    task = jce_async_submit(executor, &desc);
    TEST_ASSERT_NOT_NULL(task);
    jce_async_task_wait(task);
    jce_async_executor_pump(executor, NULL);
    jce_async_task_release(task);
    TEST_ASSERT_TRUE(jce_async_executor_shutdown(
        executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE));
    jce_async_executor_destroy(executor);
    TEST_ASSERT_EQUAL_INT(1, value);

    jce_trace_cursor_init(&cursor);
    count = jce_trace_read(&cursor, events, 32);
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_TASK_SUBMIT, "trace-async-task"));
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_TASK_BEGIN, "trace-async-task"));
    TEST_ASSERT_NOT_NULL(find_event(
        events, count, JCE_TRACE_EVENT_TASK_END, "trace-async-task"));
}

static void test_chrome_trace_export_contains_thread_and_task_names(void)
{
    const char *path = "jce_trace_test.chrome.json";
    uint64_t size = 0;
    char *json;
    uint64_t task_id = jce_trace_next_id();
    uint64_t span_id = jce_trace_next_id();

    jce_trace_thread_register("export-main");
    jce_trace_task_submit(task_id, 0, JCE_TRACE_TASK_FRAME_JOB,
                          "export-task");
    jce_trace_task_begin(span_id, task_id, "export-task", 10);
    jce_trace_task_end(span_id, task_id, "export-task",
                       JCE_TRACE_TASK_SUCCEEDED, 20);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "signed-counter", -7);

    TEST_ASSERT_TRUE(jce_trace_export_chrome_json(path));
    json = (char *)jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_GREATER_THAN_UINT64(0, size);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"traceEvents\""));
    TEST_ASSERT_NOT_NULL(strstr(json, "export-main"));
    TEST_ASSERT_NOT_NULL(strstr(json, "export-task"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"value\":-7"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"lost\":0"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"capture_generation_stable\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"state\":0"));
    jce_fs_buffer_free(json);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(path));
}

static void test_export_reports_ring_overwrite_and_failed_task_state(void)
{
    const char *path = "jce_trace_overwrite.chrome.json";
    JceTraceStats stats;
    uint64_t size = 0;
    char *json;
    uint32_t i;
    uint64_t id;
    jce_trace_get_stats(&stats);
    for (i = 0; i < stats.capacity + 7; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "fill", i);
    id = jce_trace_next_id();
    jce_trace_task_submit(id, 0, JCE_TRACE_TASK_SERVICE, "failed-export");
    jce_trace_task_begin(id, id, "failed-export", 0);
    jce_trace_task_end(id, id, "failed-export", JCE_TRACE_TASK_FAILED, 20);
    TEST_ASSERT_TRUE(jce_trace_export_chrome_json(path));
    json = (char *)jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"jceTrace\""));
    char expected[96];
    snprintf(expected, sizeof expected, "\"capacity\":%u", stats.capacity);
    TEST_ASSERT_NOT_NULL(strstr(json, expected));
    snprintf(expected, sizeof expected, "\"recorded\":%u", stats.capacity + 10u);
    TEST_ASSERT_NOT_NULL(strstr(json, expected));
    snprintf(expected, sizeof expected, "\"captured\":%u", stats.capacity);
    TEST_ASSERT_NOT_NULL(strstr(json, expected));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"overwritten_before_capture\":10"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"cursor_lost\":0"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"lost\":10"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"state\":1"));
    jce_fs_buffer_free(json);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(path));
}

typedef struct PoolLifetimeControl {
    JceSemaphore *started;
    JceSemaphore *release;
    uint64_t worker_tid;
} PoolLifetimeControl;

static void pool_lifetime_worker(void *arg)
{
    PoolLifetimeControl *control = arg;
    control->worker_tid = jce_thread_current_id();
    jce_semaphore_signal(control->started);
    jce_semaphore_wait(control->release);
}

static void test_real_pool_worker_exit_unregisters_trace_thread(void)
{
    PoolLifetimeControl control = {0};
    control.started = jce_semaphore_create(0);
    control.release = jce_semaphore_create(0);
    JceThreadPool *pool = jce_thread_pool_create_named(2, "trace-worker-exit");
    TEST_ASSERT_NOT_NULL(control.started);
    TEST_ASSERT_NOT_NULL(control.release);
    TEST_ASSERT_NOT_NULL(pool);
    bool submitted = jce_thread_pool_submit_named(pool, "worker-lifetime-control",
                                                  pool_lifetime_worker, &control);
    /* Owner blocks on a semaphore, not enki wait: it cannot steal this task. */
    bool started = submitted && jce_semaphore_wait_timeout(control.started, 5000);
    JceTraceStats during;
    jce_trace_get_stats(&during);
    jce_semaphore_signal(control.release);
    jce_thread_pool_destroy(pool);
    JceTraceStats after;
    jce_trace_get_stats(&after);
    jce_semaphore_destroy(control.started);
    jce_semaphore_destroy(control.release);
    TEST_ASSERT_TRUE(submitted);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_NOT_EQUAL(jce_thread_current_id(), control.worker_tid);
    TEST_ASSERT_EQUAL_UINT32(1, during.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, after.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, after.active_work_items);
    JceTraceCursor cursor;
    JceTraceEvent events[64];
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 64);
    const JceTraceEvent *exit = find_event(events, count,
                                          JCE_TRACE_EVENT_THREAD_UNREGISTER,
                                          "thread-exit");
    TEST_ASSERT_NOT_NULL(exit);
    TEST_ASSERT_EQUAL_UINT64(control.worker_tid, exit->thread_id);
}

static void test_pool_exit_preserves_other_pool_and_disabled_trace_cleanup(void)
{
    PoolLifetimeControl controls[2] = {{0}};
    JceThreadPool *pools[2];
    JceTask *tasks[2];
    jce_trace_set_enabled(false);
    for (unsigned i = 0; i < 2; ++i) {
        controls[i].started = jce_semaphore_create(0);
        controls[i].release = jce_semaphore_create(0);
        pools[i] = jce_thread_pool_create_named(2, "trace-independent-pool");
        TEST_ASSERT_NOT_NULL(pools[i]);
    }
    /* Workers already exist when tracing starts; registration must be lazy. */
    jce_trace_set_enabled(true);
    for (unsigned i = 0; i < 2; ++i) {
        tasks[i] = jce_thread_pool_submit_tracked_named(pools[i], "late-trace-worker",
                                                        pool_lifetime_worker, &controls[i]);
        TEST_ASSERT_NOT_NULL(tasks[i]);
        TEST_ASSERT_TRUE(jce_semaphore_wait_timeout(controls[i].started, 5000));
    }
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(2, stats.active_threads);
    TEST_ASSERT_NOT_EQUAL(controls[0].worker_tid, controls[1].worker_tid);
    jce_semaphore_signal(controls[0].release);
    jce_task_wait(tasks[0]);
    jce_task_free(tasks[0]);
    jce_thread_pool_destroy(pools[0]);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(1, stats.active_threads);
    /* Disabling emission must not leak an already registered worker owner. */
    jce_semaphore_signal(controls[1].release);
    jce_task_wait(tasks[1]);
    jce_task_free(tasks[1]);
    jce_trace_set_enabled(false);
    jce_thread_pool_destroy(pools[1]);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    for (unsigned i = 0; i < 2; ++i) {
        jce_semaphore_destroy(controls[i].started);
        jce_semaphore_destroy(controls[i].release);
    }
}

static JceAsyncRunResult async_lifetime_worker(JceAsyncContext *ctx, void *arg)
{
    (void)ctx;
    pool_lifetime_worker(arg);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void test_default_async_worker_exit_unregisters_trace_thread(void)
{
    PoolLifetimeControl control = {0};
    control.started = jce_semaphore_create(0);
    control.release = jce_semaphore_create(0);
    JceAsyncExecutor *executor = jce_async_default_executor();
    TEST_ASSERT_NOT_NULL(executor);
    JceAsyncTaskDesc desc;
    jce_async_task_desc_init(&desc);
    desc.work = async_lifetime_worker;
    desc.user_data = &control;
    desc.debug_name = "default-async-worker-lifetime";
    JceAsyncTask *task = jce_async_submit(executor, &desc);
    bool started = task && jce_semaphore_wait_timeout(control.started, 5000);
    JceTraceStats during;
    jce_trace_get_stats(&during);
    jce_semaphore_signal(control.release);
    if (task) { jce_async_task_wait(task); jce_async_task_release(task); }
    bool stopped = jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_DRAIN,
                                               JCE_ASYNC_WAIT_INFINITE);
    JceTraceStats after;
    jce_trace_get_stats(&after);
    jce_semaphore_destroy(control.started);
    jce_semaphore_destroy(control.release);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_NOT_EQUAL(jce_thread_current_id(), control.worker_tid);
    TEST_ASSERT_EQUAL_UINT32(1, during.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, after.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, after.active_work_items);
    JceTraceCursor cursor;
    JceTraceEvent events[64];
    jce_trace_cursor_init(&cursor);
    uint32_t count = jce_trace_read(&cursor, events, 64);
    const JceTraceEvent *exit = find_event(events, count,
                                          JCE_TRACE_EVENT_THREAD_UNREGISTER,
                                          "thread-exit");
    TEST_ASSERT_NOT_NULL(exit);
    TEST_ASSERT_EQUAL_UINT64(control.worker_tid, exit->thread_id);
}

typedef struct ExternalTlsControl {
    PoolLifetimeControl life;
    SDL_TLSID slot;
    bool installed;
    unsigned destroyed;
    uint64_t cleanup_tid;
} ExternalTlsControl;

static void SDLCALL external_tls_destructor(void *value)
{
    ExternalTlsControl *control = value;
    control->cleanup_tid = jce_thread_current_id();
    ++control->destroyed;
}

static void external_tls_worker(void *arg)
{
    ExternalTlsControl *control = arg;
    control->installed = SDL_SetTLS(&control->slot, control, external_tls_destructor);
    pool_lifetime_worker(&control->life);
}

static JceAsyncRunResult external_tls_async_worker(JceAsyncContext *ctx, void *arg)
{
    (void)ctx;
    external_tls_worker(arg);
    return JCE_ASYNC_RUN_SUCCESS;
}

static void check_external_worker_tls_cleanup(bool asynchronous, bool trace_on)
{
    ExternalTlsControl control = {0};
    control.life.started = jce_semaphore_create(0);
    control.life.release = jce_semaphore_create(0);
    JceThreadPool *pool = NULL;
    JceAsyncTask *task = NULL;
    bool admitted;
    jce_trace_set_enabled(trace_on);
    if (asynchronous) {
        JceAsyncTaskDesc desc;
        jce_async_task_desc_init(&desc);
        desc.work = external_tls_async_worker;
        desc.user_data = &control;
        task = jce_async_submit(jce_async_default_executor(), &desc);
        admitted = task != NULL;
    } else {
        pool = jce_thread_pool_create_named(2, "external-tls-control");
        admitted = pool && jce_thread_pool_submit(pool, external_tls_worker, &control);
    }
    bool started = admitted && jce_semaphore_wait_timeout(control.life.started, 5000);
    unsigned before_exit = control.destroyed;
    jce_semaphore_signal(control.life.release);
    bool stopped = true;
    if (asynchronous) {
        if (task) { jce_async_task_wait(task); jce_async_task_release(task); }
        stopped = jce_async_default_shutdown(JCE_ASYNC_SHUTDOWN_DRAIN,
                                              JCE_ASYNC_WAIT_INFINITE);
    } else {
        jce_thread_pool_destroy(pool);
    }
    jce_semaphore_destroy(control.life.started);
    jce_semaphore_destroy(control.life.release);
    TEST_ASSERT_TRUE(admitted);
    TEST_ASSERT_TRUE(started);
    TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_TRUE(control.installed);
    TEST_ASSERT_NOT_EQUAL(jce_thread_current_id(), control.life.worker_tid);
    TEST_ASSERT_EQUAL_UINT(0, before_exit);
    TEST_ASSERT_EQUAL_UINT(1, control.destroyed);
    TEST_ASSERT_EQUAL_UINT64(control.life.worker_tid, control.cleanup_tid);
}

static void test_pool_external_tls_cleanup_trace_on(void)
{ check_external_worker_tls_cleanup(false, true); }
static void test_pool_external_tls_cleanup_trace_off(void)
{ check_external_worker_tls_cleanup(false, false); }
static void test_async_external_tls_cleanup_trace_on(void)
{ check_external_worker_tls_cleanup(true, true); }
static void test_async_external_tls_cleanup_trace_off(void)
{ check_external_worker_tls_cleanup(true, false); }

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_pool_external_tls_cleanup_trace_on);
    RUN_TEST(test_pool_external_tls_cleanup_trace_off);
    RUN_TEST(test_async_external_tls_cleanup_trace_on);
    RUN_TEST(test_async_external_tls_cleanup_trace_off);
    RUN_TEST(test_default_async_worker_exit_unregisters_trace_thread);
    RUN_TEST(test_real_pool_worker_exit_unregisters_trace_thread);
    RUN_TEST(test_pool_exit_preserves_other_pool_and_disabled_trace_cleanup);
    RUN_TEST(test_disabled_trace_has_zero_producer_cost_visible_to_consumer);
    RUN_TEST(test_lifecycle_events_preserve_ids_names_and_timing);
    RUN_TEST(test_stats_preserve_multi_second_hitches_without_int_clamping);
    RUN_TEST(test_multiple_producers_publish_complete_events);
    RUN_TEST(test_named_pool_task_emits_submit_run_and_wait_events);
    RUN_TEST(test_structured_async_uses_same_trace_contract);
    RUN_TEST(test_chrome_trace_export_contains_thread_and_task_names);
    RUN_TEST(test_export_reports_ring_overwrite_and_failed_task_state);
    return UNITY_END();
}
