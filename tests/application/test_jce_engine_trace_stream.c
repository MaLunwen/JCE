/* Real headless engine lifetime and producer-tail capture, without a GPU. */
#include "unity.h"
#include <jce/application/jce_app_interface.h>
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_trace.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifndef JCE_TRACE_TEST_OUTPUT
#error "JCE_TRACE_TEST_OUTPUT must name an ignored test-output directory"
#endif

const unsigned char assets_pak_data[1] = {0};
const size_t assets_pak_data_size = 0;

typedef struct TailWorker {
    JceMutex *mutex;
    JceCondVar *condition;
    JceThread *thread;
    bool stop;
    bool joined;
    unsigned updates;
} TailWorker;

static TailWorker worker;

void setUp(void)
{
    memset(&worker, 0, sizeof worker);
    jce_trace_set_enabled(false);
    jce_trace_reset();
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
}

static void tail_thread(void *argument)
{
    TailWorker *tail = (TailWorker *)argument;
    uint64_t id;
    jce_trace_thread_register("integration.tail.worker");
    jce_mutex_lock(tail->mutex);
    while (!tail->stop)
        jce_cond_wait(tail->condition, tail->mutex);
    jce_mutex_unlock(tail->mutex);
    id = jce_trace_next_id();
    jce_trace_task_submit(id, 0, JCE_TRACE_TASK_SERVICE, "integration.tail.shutdown");
    jce_trace_task_begin(id, id, "integration.tail.shutdown", 0);
    jce_trace_task_end(id, id, "integration.tail.shutdown", JCE_TRACE_TASK_SUCCEEDED, 0);
    jce_trace_thread_unregister();
}

static bool application_init(const JceServices *services, void *argument)
{
    TailWorker *tail = (TailWorker *)argument;
    (void)services;
    tail->mutex = jce_mutex_create();
    tail->condition = jce_cond_create();
    if (!tail->mutex || !tail->condition) return false;
    tail->thread = jce_thread_create(tail_thread, tail, "integration.tail.worker");
    return tail->thread != NULL;
}

static void application_update(float dt, void *argument)
{
    TailWorker *tail = (TailWorker *)argument;
    (void)dt;
    ++tail->updates;
}

static void application_exit(void *argument)
{
    TailWorker *tail = (TailWorker *)argument;
    if (tail->thread) {
        jce_mutex_lock(tail->mutex);
        tail->stop = true;
        jce_cond_signal(tail->condition);
        jce_mutex_unlock(tail->mutex);
        jce_thread_join(tail->thread);
        tail->thread = NULL;
        tail->joined = true;
    }
    jce_cond_destroy(tail->condition);
    jce_mutex_destroy(tail->mutex);
    tail->condition = NULL;
    tail->mutex = NULL;
}

static void run_engine(void)
{
    JceAppDesc app = {0};
    JceEngine *engine;
    char argument[] = "test_jce_engine_trace_stream";
    char *argv[] = {argument, NULL};
    app.name = "trace-headless-integration";
    app.headless = true;
    app.init = application_init;
    app.update = application_update;
    app.exit = application_exit;
    app.user_data = &worker;
    jce_engine_set_app_desc(&app);
    engine = jce_engine_create(1, argv);
    TEST_ASSERT_NOT_NULL(engine);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, jce_engine_iterate(engine));
    jce_engine_destroy(engine);
    TEST_ASSERT_TRUE(worker.joined);
    TEST_ASSERT_TRUE(worker.updates > 0);
}

static void assert_number(const JceJson *object, const char *name, double expected)
{
    JceJson *value = jce_json_get(object, name);
    TEST_ASSERT_TRUE(jce_json_is_number(value));
    TEST_ASSERT_TRUE(jce_json_number_value(value, -1) == expected);
}

static void assert_true(const JceJson *object, const char *name)
{
    JceJson *value = jce_json_get(object, name);
    TEST_ASSERT_TRUE(jce_json_is_bool(value));
    TEST_ASSERT_TRUE(jce_json_bool_value(value, false));
}

static int event_index(const JceJson *events, const char *name, const char *phase)
{
    int i;
    for (i = 0; i < jce_json_array_size(events); ++i) {
        JceJson *event = jce_json_array_at(events, i);
        if (strcmp(jce_json_get_string(event, "name", ""), name) == 0 &&
            strcmp(jce_json_get_string(event, "ph", ""), phase) == 0)
            return i;
    }
    return -1;
}

static void verify_capture(const char *path, const char *mode)
{
    const char *phases[] = {"engine.renderer_headless", "engine.app_init", "engine.app_update",
                            "engine.async_shutdown", "engine.app_exit", "engine.renderer_destroy",
                            "integration.tail.shutdown", "engine.shared_pool_shutdown",
                            "engine.log_quiesce"};
    JceJson *document = jce_json_parse_file(path);
    JceJson *events;
    JceJson *metadata;
    int count, i, tail_end, worker_unregister = -1;
    double worker_tid, owner_tid;
    int owner_unregister = -1;
    TEST_ASSERT_NOT_NULL_MESSAGE(document, jce_json_last_error());
    events = jce_json_get(document, "traceEvents");
    metadata = jce_json_get(document, "jceTrace");
    TEST_ASSERT_TRUE(jce_json_is_array(events));
    TEST_ASSERT_TRUE(jce_json_is_object(metadata));
    count = jce_json_array_size(events);
    TEST_ASSERT_TRUE(count > 0);
    assert_number(metadata, "captured", count);
    assert_number(metadata, "recorded", count);
    assert_number(metadata, "overwritten_before_capture", 0);
    assert_number(metadata, "cursor_lost", 0);
    assert_number(metadata, "lost", 0);
    assert_number(metadata, "unexported", 0);
    assert_number(metadata, "active_threads", 0);
    assert_number(metadata, "active_work_items", 0);
    assert_number(metadata, "active_waits", 0);
    assert_true(metadata, "write_ok");
    assert_true(metadata, "capture_complete");
    assert_true(metadata, "capture_generation_stable");
    TEST_ASSERT_EQUAL_STRING(mode, jce_json_get_string(metadata, "consumer_mode", ""));
    TEST_ASSERT_TRUE(jce_json_is_bool(jce_json_get(metadata, "worker_available")));
    TEST_ASSERT_EQUAL_INT(strcmp(mode, "threaded") == 0,
        jce_json_get_bool(metadata, "worker_available", false));
    for (i = 0; i < (int)(sizeof phases / sizeof phases[0]); ++i) {
        int begin = event_index(events, phases[i], "B");
        int end = event_index(events, phases[i], "E");
        TEST_ASSERT_TRUE_MESSAGE(begin >= 0 && end > begin, phases[i]);
        assert_number(jce_json_get(jce_json_array_at(events, end), "args"), "state", JCE_TRACE_TASK_SUCCEEDED);
    }
    tail_end = event_index(events, "integration.tail.shutdown", "E");
    worker_tid = jce_json_get_number(jce_json_array_at(events, tail_end), "tid", 0);
    TEST_ASSERT_TRUE(worker_tid > 0);
    for (i = tail_end + 1; i < count; ++i) {
        JceJson *event = jce_json_array_at(events, i);
        if (strcmp(jce_json_get_string(event, "name", ""), "thread-exit") == 0 &&
            jce_json_get_number(event, "tid", 0) == worker_tid)
            worker_unregister = i;
    }
    TEST_ASSERT_TRUE(worker_unregister > tail_end);
    TEST_ASSERT_TRUE(event_index(events, "engine.app_exit", "E") > worker_unregister);
    TEST_ASSERT_TRUE(event_index(events, "engine.renderer_destroy", "E") > worker_unregister);
    owner_tid = jce_json_get_number(jce_json_array_at(events,
        event_index(events, "engine.app_init", "B")), "tid", 0);
    TEST_ASSERT_TRUE(owner_tid > 0 && owner_tid != worker_tid);
    for (i = event_index(events, "engine.renderer_destroy", "E") + 1; i < count; ++i) {
        JceJson *event = jce_json_array_at(events, i);
        if (strcmp(jce_json_get_string(event, "name", ""), "thread-exit") == 0 &&
            jce_json_get_number(event, "tid", 0) == owner_tid)
            owner_unregister = i;
    }
    TEST_ASSERT_TRUE(owner_unregister > event_index(events, "engine.renderer_destroy", "E"));
    int log_begin = event_index(events, "engine.log_quiesce", "B");
    int log_end = event_index(events, "engine.log_quiesce", "E");
    int cleanup_end = event_index(events, "engine.cleanup_attempt", "E");
    int pool_begin = event_index(events, "engine.shared_pool_shutdown", "B");
    int pool_end = event_index(events, "engine.shared_pool_shutdown", "E");
    TEST_ASSERT_TRUE(pool_begin > event_index(events, "engine.renderer_destroy", "E"));
    TEST_ASSERT_TRUE(pool_end > pool_begin && log_begin > pool_end);
    TEST_ASSERT_TRUE(log_end > log_begin && cleanup_end > log_end && owner_unregister > cleanup_end);
    if (strcmp(mode, "threaded") == 0) {
        int join_begin = event_index(events, "trace.capture.join", "B");
        int join_end = event_index(events, "trace.capture.join", "E");
        TEST_ASSERT_TRUE(join_begin > pool_end);
        TEST_ASSERT_TRUE(join_end > join_begin && log_begin > join_end);
        JceJson *begin_args = jce_json_get(jce_json_array_at(events, join_begin), "args");
        JceJson *end_args = jce_json_get(jce_json_array_at(events, join_end), "args");
        double join_parent = jce_json_get_number(begin_args, "parent", 0);
        TEST_ASSERT_TRUE(join_parent > 0);
        assert_number(end_args, "parent", join_parent);
        assert_number(end_args, "id", jce_json_get_number(begin_args, "id", 0));
        assert_number(jce_json_get(jce_json_array_at(events, cleanup_end), "args"),
                      "id", join_parent);
    } else {
        TEST_ASSERT_EQUAL_INT(-1, event_index(events, "trace.capture.join", "B"));
        TEST_ASSERT_EQUAL_INT(-1, event_index(events, "trace.capture.join", "E"));
    }
    jce_json_free(document);
}

static void capture_mode(const char *mode)
{
    char path[1024];
    snprintf(path, sizeof path, "%s/%s.json", JCE_TRACE_TEST_OUTPUT, mode);
    jce_fs_host_remove_file(path);
    TEST_ASSERT_EQUAL_STRING(path, getenv("JCE_TRACE_STREAM"));
    TEST_ASSERT_EQUAL_STRING(mode, getenv("JCE_TRACE_STREAM_MODE"));
    run_engine();
    verify_capture(path, mode);
}

static void test_threaded_stream_captures_full_engine_and_thread_tail(void)
{
    capture_mode("threaded");
}

static void test_cooperative_stream_captures_same_tail_without_consumer_worker(void)
{
    capture_mode("cooperative");
}

#ifndef NDEBUG
/* Inspect the public ring from the actual completed headless engine session.
 * Debug auto-enablement is useful only if teardown preserves a balanced tail. */
static void verify_default_trace_tail(void)
{
    const char *names[] = {"engine.app_update", "integration.tail.shutdown",
                           "engine.renderer_destroy"};
    uint32_t begins[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint32_t ends[3] = {UINT32_MAX, UINT32_MAX, UINT32_MAX};
    uint64_t spans[3] = {0, 0, 0};
    uint64_t worker_tid = 0;
    uint32_t worker_unregister = UINT32_MAX;
    uint32_t ordinal = 0, count, i, phase;
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    jce_trace_cursor_init(&cursor);
    while ((count = jce_trace_read(&cursor, events, 32)) != 0) {
        for (i = 0; i < count; ++i, ++ordinal) {
            const JceTraceEvent *event = &events[i];
            for (phase = 0; phase < 3; ++phase) {
                if (strcmp(event->name, names[phase]) != 0) continue;
                if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
                    begins[phase] = ordinal;
                    spans[phase] = event->id;
                } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
                    ends[phase] = ordinal;
                    TEST_ASSERT_EQUAL_UINT64(spans[phase], event->id);
                    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED,
                                             event->value_u32[0]);
                    if (phase == 1) worker_tid = event->thread_id;
                }
            }
            if (worker_tid && event->type == JCE_TRACE_EVENT_THREAD_UNREGISTER &&
                event->thread_id == worker_tid)
                worker_unregister = ordinal;
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    for (phase = 0; phase < 3; ++phase) {
        TEST_ASSERT_TRUE_MESSAGE(begins[phase] != UINT32_MAX &&
            ends[phase] != UINT32_MAX && ends[phase] > begins[phase], names[phase]);
    }
    TEST_ASSERT_TRUE(worker_tid != 0);
    TEST_ASSERT_TRUE(worker_unregister != UINT32_MAX &&
                     worker_unregister > ends[1] && worker_unregister < ends[2]);
}
#endif

static void test_default_trace_follows_engine_build_policy(void)
{
    JceTraceStats stats;
    /* Validate process isolation, rather than allowing a capture/perf flag to
     * make this an accidental opt-in test. Debug follows the engine's NDEBUG
     * build contract; Release must remain off without an explicit request. */
    TEST_ASSERT_TRUE(!getenv("JCE_TRACE") || !getenv("JCE_TRACE")[0]);
    TEST_ASSERT_TRUE(!getenv("JCE_PERF_LOG") || !getenv("JCE_PERF_LOG")[0]);
    TEST_ASSERT_TRUE(!getenv("JCE_TRACE_EXPORT") || !getenv("JCE_TRACE_EXPORT")[0]);
    TEST_ASSERT_TRUE(!getenv("JCE_TRACE_STREAM") || !getenv("JCE_TRACE_STREAM")[0]);
    run_engine();
    jce_trace_get_stats(&stats);
#ifndef NDEBUG
    TEST_ASSERT_TRUE(stats.enabled);
    TEST_ASSERT_TRUE(stats.recorded_events > 0);
    TEST_ASSERT_TRUE(stats.work_items_started > 0);
    TEST_ASSERT_EQUAL_UINT64(stats.work_items_started, stats.work_items_completed);
    verify_default_trace_tail();
#else
    TEST_ASSERT_FALSE(stats.enabled);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
#endif
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
}

static void test_explicit_trace_off_has_no_trace_activity(void)
{
    JceTraceStats stats;
    TEST_ASSERT_EQUAL_STRING("0", getenv("JCE_TRACE"));
    TEST_ASSERT_EQUAL_STRING("1", getenv("JCE_PERF_LOG"));
    TEST_ASSERT_TRUE(!getenv("JCE_TRACE_EXPORT") || !getenv("JCE_TRACE_EXPORT")[0]);
    TEST_ASSERT_TRUE(!getenv("JCE_TRACE_STREAM") || !getenv("JCE_TRACE_STREAM")[0]);
    run_engine();
    jce_trace_get_stats(&stats);
    TEST_ASSERT_FALSE(stats.enabled);
    TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT32(0, stats.resident_events);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
    /* No claim about unobservable allocations from a disabled public trace. */
}

static void test_invalid_output_path_does_not_abort_engine_lifecycle(void)
{
    char blocker[1024], path[1100];
    snprintf(blocker, sizeof blocker, "%s/blocker", JCE_TRACE_TEST_OUTPUT);
    snprintf(path, sizeof path, "%s/trace.json", blocker);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(blocker, "x", 1));
    TEST_ASSERT_EQUAL_STRING(path, getenv("JCE_TRACE_STREAM"));
    run_engine();
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(path));
    TEST_ASSERT_NULL(jce_json_parse_file(path));
}

int main(void)
{
    const char *mode = getenv("JCE_TRACE_INTEGRATION_CASE");
    if (!mode) {
        fputs("missing test process case environment\n", stderr);
        return 2;
    }
    UNITY_BEGIN();
    if (strcmp(mode, "threaded") == 0)
        RUN_TEST(test_threaded_stream_captures_full_engine_and_thread_tail);
    else if (strcmp(mode, "cooperative") == 0)
        RUN_TEST(test_cooperative_stream_captures_same_tail_without_consumer_worker);
    else if (strcmp(mode, "default") == 0)
        RUN_TEST(test_default_trace_follows_engine_build_policy);
    else if (strcmp(mode, "disabled") == 0)
        RUN_TEST(test_explicit_trace_off_has_no_trace_activity);
    else if (strcmp(mode, "invalid") == 0)
        RUN_TEST(test_invalid_output_path_does_not_abort_engine_lifecycle);
    else {
        fputs("unknown test process case environment\n", stderr);
        return 2;
    }
    return UNITY_END();
}
