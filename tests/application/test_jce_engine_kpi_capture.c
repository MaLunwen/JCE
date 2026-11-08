/* Production capture owner, encoder and writer; CPU-only cooperative IO. */
#include "unity.h"
#include "application/jce_engine_kpi_capture.h"
#include "application/jce_engine_perf_hitch.h"
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <SDL3/SDL_timer.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* This is the only stub: public latest renderer stats, without a GPU/window. */
static unsigned getter_calls;
static JceGpuStats getter_stats;
bool jce_renderer_get_gpu_stats(JceGpuStats *out)
{
    ++getter_calls;
    if (out) *out = getter_stats;
    return getter_stats.valid;
}

typedef struct {
    char bytes[16384];
    size_t size;
    unsigned writes, flushes, closes, delayed_data_calls;
    unsigned short_write_call, failed_flush_call, delay_ms;
    bool failed_close;
    JceEngineKpiCapture *capture;
    bool request_reentry, nested_poll, nested_export, nested_finish, nested_destroy;
    bool writer_retained, failed_retained, reported_retained, index_retained;
    bool closed_during_reentry, getter_retained, buffer_retained;
    bool profile_retained, reason_retained, record_totals_retained;
    bool nested_interval_valid, nested_interval_invalid, clock_context_retained;
    unsigned reentry_calls;
    JceAppResult nested_result;
} CaptureSink;

typedef struct {
    JceEngineKpiCapture capture;
    CaptureSink sink;
    JceAsyncExecutor *executor;
} CaptureFixture;

static size_t SDLCALL capture_sink_write(void *user, const void *bytes,
                                         size_t size, SDL_IOStatus *status)
{
    CaptureSink *sink = user;
    ++sink->writes;
    if (sink->writes > 1u && sink->request_reentry) {
        sink->request_reentry = false;
        ++sink->reentry_calls;
        JceEngineKpiCapture *capture = sink->capture;
        JceFrameKpiWriter *writer = capture->writer;
        uint64_t index = capture->index;
        bool failed = capture->failed, reported = capture->reported;
        const char *reason = capture->failure_reason;
        char *encoded = capture->buffer.bytes;
        size_t capacity = capture->buffer.capacity;
        JceFrameKpiProfile profile;
        memcpy(&profile, &capture->buffer.last_profile, sizeof profile);
        unsigned getters = getter_calls;
        char prefix[256];
        size_t encoded_size = encoded ? strlen(encoded) : 0;
        bool can_copy = encoded_size < sizeof prefix;
        if (can_copy && encoded_size) memcpy(prefix, encoded, encoded_size);
        JceFrameKpiWriterStatus before;
        jce_frame_kpi_writer_get_status(writer, &before);
        if (capture->context_version == 3) {
            JceFrameKpiContext context = capture->context;
            sink->nested_interval_valid = jce_engine_kpi_capture_interval(capture, 1000, 20, 22);
            sink->nested_interval_invalid = jce_engine_kpi_capture_interval(capture, 0, 99, 1);
            sink->clock_context_retained = memcmp(&context, &capture->context, sizeof context) == 0;
        }
        sink->nested_poll = jce_engine_kpi_capture_poll(capture);
        sink->nested_export = jce_engine_kpi_capture_export(capture, 999, false);
        sink->nested_finish = jce_engine_kpi_capture_finish(capture);
        sink->nested_result = jce_engine_kpi_capture_result(capture, JCE_APP_SUCCESS);
        sink->nested_destroy = jce_engine_kpi_capture_destroy(capture);
        sink->writer_retained = capture->writer == writer;
        sink->failed_retained = capture->failed == failed;
        sink->reported_retained = capture->reported == reported;
        sink->index_retained = capture->index == index;
        sink->getter_retained = getter_calls == getters;
        sink->reason_retained = capture->failure_reason == reason;
        sink->profile_retained = memcmp(&capture->buffer.last_profile, &profile,
                                       sizeof profile) == 0;
        sink->buffer_retained = capture->buffer.bytes == encoded &&
            capture->buffer.capacity == capacity && can_copy &&
            (!encoded || (strlen(encoded) == encoded_size &&
                          memcmp(encoded, prefix, encoded_size) == 0));
        if (sink->writer_retained) {
            JceFrameKpiWriterStatus status;
            jce_frame_kpi_writer_get_status(writer, &status);
            sink->closed_during_reentry = status.closed;
            sink->record_totals_retained =
                status.accepted_records == before.accepted_records &&
                status.written_records == before.written_records &&
                status.accepted_bytes == before.accepted_bytes &&
                status.written_bytes == before.written_bytes;
        } else sink->closed_during_reentry = true;
    }
    if (sink->writes > 1u && sink->delay_ms) {
        ++sink->delayed_data_calls;
        SDL_Delay(sink->delay_ms);
    }
    if (sink->writes == sink->short_write_call ||
        size >= sizeof sink->bytes - sink->size) {
        *status = SDL_IO_STATUS_ERROR;
        SDL_SetError("Capture fixture incomplete write");
        return 0;
    }
    memcpy(sink->bytes + sink->size, bytes, size);
    sink->size += size;
    sink->bytes[sink->size] = 0;
    *status = SDL_IO_STATUS_READY;
    return size;
}

static bool SDLCALL capture_sink_flush(void *user, SDL_IOStatus *status)
{
    CaptureSink *sink = user;
    ++sink->flushes;
    bool succeeded = sink->flushes != sink->failed_flush_call;
    *status = succeeded ? SDL_IO_STATUS_READY : SDL_IO_STATUS_ERROR;
    if (!succeeded) SDL_SetError("Capture fixture final flush failure");
    return succeeded;
}

static bool SDLCALL capture_sink_close(void *user)
{
    CaptureSink *sink = user;
    ++sink->closes;
    if (sink->failed_close) SDL_SetError("Capture fixture close failure");
    return !sink->failed_close;
}

static void fixture_attach_mode(CaptureFixture *fixture, uint32_t limit, bool context_json)
{
    /* Private grouped-owner seam: attach a REAL writer over REAL SDL IO.
     * This avoids replacing production open/result/export/finish methods. */
    JceAsyncExecutorConfig async;
    jce_async_executor_config_init(&async);
    async.mode = JCE_ASYNC_EXECUTION_COOPERATIVE;
    async.worker_count = 0;
    async.max_tasks = 8;
    async.debug_name = "test-engine-kpi-capture";
    fixture->executor = jce_async_executor_create(&async);
    TEST_ASSERT_NOT_NULL(fixture->executor);

    SDL_IOStreamInterface interface;
    SDL_INIT_INTERFACE(&interface);
    interface.write = capture_sink_write;
    interface.flush = capture_sink_flush;
    interface.close = capture_sink_close;
    SDL_IOStream *stream = SDL_OpenIO(&interface, &fixture->sink);
    TEST_ASSERT_NOT_NULL(stream);

    JceFrameKpiWriterConfig config;
    jce_frame_kpi_writer_config_init(&config, true);
    config.stream = stream;
    config.executor = fixture->executor;
    config.byte_capacity = 65536;
    config.record_capacity = 8;
    config.drain_bytes = 16384;
    config.drain_records = 1;
    config.durable_interval = 0;
    config.context_json = context_json;
    fixture->capture.writer = jce_frame_kpi_writer_create(&config);
    if (!fixture->capture.writer) SDL_CloseIO(stream);
    TEST_ASSERT_NOT_NULL(fixture->capture.writer);
    fixture->capture.limit = limit;
    fixture->capture.context_enabled = context_json;
    fixture->sink.capture = &fixture->capture;
}

static void fixture_attach(CaptureFixture *fixture, uint32_t limit)
{
    fixture_attach_mode(fixture, limit, false);
}

static bool fixture_dispose(CaptureFixture *fixture)
{
    /* False retains capture and borrowed executor; never free either early. */
    if (!jce_engine_kpi_capture_destroy(&fixture->capture)) return false;
    if (fixture->executor) {
        jce_async_executor_shutdown(fixture->executor,
            JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
        jce_async_executor_destroy(fixture->executor);
        fixture->executor = NULL;
    }
    return true;
}

static JceFrameKpiWriterStatus capture_status(CaptureFixture *fixture)
{
    JceFrameKpiWriterStatus status;
    jce_frame_kpi_writer_get_status(fixture->capture.writer, &status);
    return status;
}

void setUp(void)
{
    getter_calls = 0;
    memset(&getter_stats, 0, sizeof getter_stats);
    getter_stats.valid = true;
    getter_stats.cpu_frame_ms = 2.5;
    getter_stats.gpu_ms = 0.125;
    jce_perf_phase_set_enabled(0);
}
void tearDown(void) { jce_perf_phase_set_enabled(0); }

static void test_context_selector_is_exact_and_defaults_preserve_header(void)
{
    static const char *selectors[] = {NULL, "", "0", "1", "2", "3"};
    JceAsyncExecutorConfig config; jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_COOPERATIVE; config.worker_count = 0;
    config.max_tasks = 8;
    JceAsyncExecutor *executor = jce_async_executor_create(&config);
    TEST_ASSERT_NOT_NULL(executor);
    for (unsigned i = 0; i < sizeof selectors / sizeof selectors[0]; ++i) {
        JceEngineKpiCapture capture = {0};
        jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
        bool opened = jce_engine_kpi_capture_open_with_context(&capture,
            JCE_KPI_CONTEXT_SELECTOR_PATH, executor, true, 0, 0, selectors[i]);
        bool enabled = capture.context_enabled;
        uint32_t version = capture.context_version;
        bool active = jce_engine_kpi_capture_context_active(&capture);
        bool finished = jce_engine_kpi_capture_finish(&capture);
        bool released = jce_engine_kpi_capture_destroy(&capture);
        uint64_t size = 0; void *bytes = jce_fs_host_read_all(JCE_KPI_CONTEXT_SELECTOR_PATH, &size);
        const char *expected = jce_frame_kpi_csv_header(i >= 3);
        bool exact = bytes && size == strlen(expected) && memcmp(bytes, expected, (size_t)size) == 0;
        jce_fs_buffer_free(bytes);
        bool removed = jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
        TEST_ASSERT_TRUE(opened); TEST_ASSERT_TRUE(finished); TEST_ASSERT_TRUE(released);
        TEST_ASSERT_EQUAL_INT(i >= 3, enabled); TEST_ASSERT_EQUAL_INT(i >= 3, active);
        TEST_ASSERT_EQUAL_UINT32(i >= 3 ? i - 2 : 0, version);
        TEST_ASSERT_TRUE(exact); TEST_ASSERT_TRUE(removed);
    }
    static const char *invalid[] = {"true", "01", "1 ", "4", "03", "3 ", "unknown"};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        JceEngineKpiCapture capture = {0};
        bool opened = jce_engine_kpi_capture_open_with_context(&capture,
            JCE_KPI_CONTEXT_SELECTOR_PATH, executor, true, 0, 0, invalid[i]);
        bool failed = capture.failed;
        bool had_writer = capture.writer != NULL;
        uint64_t size = 0; void *bytes = jce_fs_host_read_all(JCE_KPI_CONTEXT_SELECTOR_PATH, &size);
        bool absent = bytes == NULL; jce_fs_buffer_free(bytes);
        if (!absent) jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
        jce_engine_kpi_capture_destroy(&capture);
        TEST_ASSERT_FALSE(opened); TEST_ASSERT_TRUE(failed); TEST_ASSERT_FALSE(had_writer);
        TEST_ASSERT_TRUE_MESSAGE(absent, "invalid context selector performed stream IO");
    }
    jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    jce_async_executor_destroy(executor);
    JceEngineKpiCapture disabled = {0};
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_open_with_context(&disabled, "", NULL,
        true, 0, 0, "invalid-but-disabled"));
    TEST_ASSERT_FALSE(jce_engine_kpi_capture_context_active(&disabled));
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_destroy(&disabled));
}

/* Match the production C99 getenv reader. SDL's Win32 environment setter can
 * update the OS/SDL table without updating this executable's CRT environment. */
static int capture_backend_environment(const char *value)
{
#ifdef _WIN32
    return _putenv_s("JCE_KPI_BACKEND_TIMINGS", value ? value : "");
#else
    return value ? setenv("JCE_KPI_BACKEND_TIMINGS", value, 1) :
                   unsetenv("JCE_KPI_BACKEND_TIMINGS");
#endif
}

/* Reuse the production CRT environment boundary, not SDL's separate table. */
static int capture_queue_environment(const char *value)
{
#ifdef _WIN32
    return _putenv_s("JCE_KPI_WRITER_QUEUE", value ? value : "");
#else
    return value ? setenv("JCE_KPI_WRITER_QUEUE", value, 1) :
                   unsetenv("JCE_KPI_WRITER_QUEUE");
#endif
}

static void test_queue_selector_reaches_real_writer_and_latches_each_session(void)
{
    static const struct { const char *selector; size_t bytes; uint32_t records; } cases[] = {
        {NULL, 0, 0}, {"", 0, 0}, {"33554432,16384", 33554432u, 16384u},
        {"1,1", 1u, 1u}, {"67108864,65536", 67108864u, 65536u}, {"0002,0003", 2u, 3u}
    };
    const char *old = getenv("JCE_KPI_WRITER_QUEUE");
    char *saved = old ? SDL_strdup(old) : NULL;
    if (old) TEST_ASSERT_NOT_NULL(saved);
    JceAsyncExecutorConfig config; jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_COOPERATIVE; config.worker_count = 0;
    config.max_tasks = 8;
    JceAsyncExecutor *executor = jce_async_executor_create(&config);
    if (!executor) { SDL_free(saved); TEST_FAIL_MESSAGE("cannot create queue-selection executor"); }
    struct { bool env, opened, finished, released, latched; size_t bytes; uint32_t records; } seen[12] = {0};
    for (unsigned low = 0; low < 2; ++low) {
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            unsigned index = low * 6u + i;
            JceEngineKpiCapture capture = {0};
            seen[index].env = capture_queue_environment(cases[i].selector) == 0;
            seen[index].opened = jce_engine_kpi_capture_open(&capture,
                JCE_KPI_CONTEXT_SELECTOR_PATH, executor, low != 0, 0, 0);
            JceFrameKpiWriterStatus status;
            jce_frame_kpi_writer_get_status(capture.writer, &status);
            seen[index].bytes = status.byte_capacity; seen[index].records = status.record_capacity;
            seen[index].env = capture_queue_environment("invalid-after-admission") == 0 && seen[index].env;
            JceFrameKpiWriterStatus unchanged;
            jce_frame_kpi_writer_get_status(capture.writer, &unchanged);
            seen[index].latched = unchanged.byte_capacity == status.byte_capacity &&
                unchanged.record_capacity == status.record_capacity &&
                capture.buffer.max_capacity == status.byte_capacity;
            seen[index].finished = jce_engine_kpi_capture_finish(&capture);
            seen[index].released = jce_engine_kpi_capture_destroy(&capture);
            jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
        }
    }
    int restored = capture_queue_environment(saved); SDL_free(saved);
    jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    jce_async_executor_destroy(executor);
    TEST_ASSERT_EQUAL_INT(0, restored);
    for (unsigned low = 0; low < 2; ++low) {
        for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
            unsigned index = low * 6u + i;
            TEST_ASSERT_TRUE(seen[index].env); TEST_ASSERT_TRUE(seen[index].opened);
            TEST_ASSERT_TRUE(seen[index].finished); TEST_ASSERT_TRUE(seen[index].released);
            TEST_ASSERT_TRUE(seen[index].latched);
            TEST_ASSERT_EQUAL_UINT64_MESSAGE(cases[i].bytes ? cases[i].bytes : low ? 262144u : 1048576u,
                seen[index].bytes, cases[i].selector ? cases[i].selector : "<unset>");
            TEST_ASSERT_EQUAL_UINT32(cases[i].records ? cases[i].records : low ? 256u : 1024u,
                seen[index].records);
        }
    }
}

static void test_queue_selector_rejects_invalid_before_open_or_owned_allocation(void)
{
    static const char *invalid[] = {"0,1", "1,0", "-1,1", "+1,1", " 1,1", "1, 1", "1,1 ",
        "1,1\n", "1", "1,1,1", ",1", "1,", "1.0,1", "1e3,1", "0x10,1", "67108865,1",
        "1,65537", "18446744073709551616,1", "1,4294967296",
        "999999999999999999999999999999,1", "1,999999999999999999999999999999", "1,,1", "\t1,1", "1,\t1", "1;1"};
    static const char sentinel[] = "Rejected queue selection must not truncate an existing file.\n";
    const char *old = getenv("JCE_KPI_WRITER_QUEUE");
    char *saved = old ? SDL_strdup(old) : NULL;
    if (old) TEST_ASSERT_NOT_NULL(saved);
    struct { bool env, rejected, failed, no_owner, unchanged, no_alloc, released; } seen[25] = {0};
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        JceEngineKpiCapture capture = {0};
        seen[i].env = capture_queue_environment(invalid[i]) == 0;
        bool sentinel_written = jce_fs_host_write_all(JCE_KPI_CONTEXT_SELECTOR_PATH, sentinel, sizeof sentinel - 1u);
        JceAllocTrack before = {0}, after = {0};
        bool tracked = jce_alloc_track_snapshot(&before);
        seen[i].rejected = !jce_engine_kpi_capture_open(&capture,
            JCE_KPI_CONTEXT_SELECTOR_PATH, NULL, false, 0, 0);
        bool after_tracked = jce_alloc_track_snapshot(&after);
        seen[i].no_alloc = !tracked || (after_tracked && before.total_allocs == after.total_allocs);
        seen[i].failed = capture.failed && capture.failure_reason &&
            strstr(capture.failure_reason, "JCE_KPI_WRITER_QUEUE");
        seen[i].no_owner = !capture.writer && !capture.buffer.bytes && !capture.index;
        seen[i].released = jce_engine_kpi_capture_destroy(&capture);
        uint64_t size = 0; void *bytes = jce_fs_host_read_all(JCE_KPI_CONTEXT_SELECTOR_PATH, &size);
        seen[i].unchanged = sentinel_written && bytes && size == sizeof sentinel - 1u &&
            memcmp(bytes, sentinel, sizeof sentinel - 1u) == 0;
        jce_fs_buffer_free(bytes); jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
    }
    JceEngineKpiCapture disabled = {0};
    bool disabled_ok = jce_engine_kpi_capture_open(&disabled, "", NULL, false, 0, 0);
    bool disabled_clean = !disabled.failed && !disabled.writer && jce_engine_kpi_capture_destroy(&disabled);
    int restored = capture_queue_environment(saved); SDL_free(saved);
    TEST_ASSERT_EQUAL_INT(0, restored); TEST_ASSERT_TRUE(disabled_ok); TEST_ASSERT_TRUE(disabled_clean);
    for (unsigned i = 0; i < sizeof invalid / sizeof invalid[0]; ++i) {
        TEST_ASSERT_TRUE_MESSAGE(seen[i].env, invalid[i]);
        TEST_ASSERT_TRUE_MESSAGE(seen[i].rejected && seen[i].failed, invalid[i]);
        TEST_ASSERT_TRUE_MESSAGE(seen[i].no_owner && seen[i].no_alloc, invalid[i]);
        TEST_ASSERT_TRUE_MESSAGE(seen[i].unchanged, invalid[i]);
        TEST_ASSERT_TRUE_MESSAGE(seen[i].released, invalid[i]);
    }
}

static void test_backend_selector_rejects_before_io_and_is_latched_per_reused_session(void)
{
    const char *old_value = getenv("JCE_KPI_BACKEND_TIMINGS");
    char *saved = old_value ? SDL_strdup(old_value) : NULL;
    if (old_value) TEST_ASSERT_NOT_NULL(saved);
    static const char *selectors[] = {NULL, "", "0", "1", "0", NULL,
                                      "01", "true", "1 ", "-1"};
    const unsigned count = sizeof selectors / sizeof selectors[0];
    static const char sentinel[] = "Existing capture must survive rejected initialization.\n";
    struct {
        bool env_set, opened, enabled, exported, finished, released, file_ok, getter_ok;
        bool env_observed, sentinel_written, failed_before_writer, reset;
    } observed[10] = {0};
    JceAsyncExecutorConfig config; jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_COOPERATIVE; config.worker_count = 0;
    config.max_tasks = 8;
    JceAsyncExecutor *executor = jce_async_executor_create(&config);
    if (!executor) { SDL_free(saved); TEST_FAIL_MESSAGE("cannot create cooperative executor"); }
    JceEngineKpiCapture capture = {0};
    getter_stats.cpu_submit_ms = 1.5; getter_stats.wait_submit_ms = 0;
    getter_stats.wait_render_ms = 3.5;
    for (unsigned i = 0; i < count; ++i) {
        observed[i].env_set = capture_backend_environment(selectors[i]) == 0;
        const char *current = getenv("JCE_KPI_BACKEND_TIMINGS");
        /* Windows CRT removes an empty assignment; both are disabled selectors. */
        observed[i].env_observed = selectors[i] && selectors[i][0] ?
            current && strcmp(current, selectors[i]) == 0 : !current || !current[0];
        jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
        if (i >= 6)
            observed[i].sentinel_written = jce_fs_host_write_all(
                JCE_KPI_CONTEXT_SELECTOR_PATH, sentinel, sizeof sentinel - 1u);
        unsigned before = getter_calls;
        observed[i].opened = jce_engine_kpi_capture_open_with_context(&capture,
            JCE_KPI_CONTEXT_SELECTOR_PATH, executor, true, 0, 0, NULL);
        observed[i].enabled = capture.buffer.backend_timings;
        observed[i].failed_before_writer = capture.failed && !capture.writer &&
            !capture.index && capture.failure_reason &&
            strstr(capture.failure_reason, "JCE_KPI_BACKEND_TIMINGS") != NULL;
        /* Neither enabling nor disabling the environment can change a session
         * already admitted. Reuse after destroy must read the new selection. */
        if (i == 2 || i == 3) observed[i].env_set = observed[i].env_set &&
            capture_backend_environment(i == 2 ? "1" : "0") == 0;
        if (observed[i].opened)
            observed[i].exported = jce_engine_kpi_capture_export(&capture, 4, false);
        observed[i].finished = jce_engine_kpi_capture_finish(&capture);
        observed[i].released = jce_engine_kpi_capture_destroy(&capture);
        observed[i].reset = !capture.writer && !capture.buffer.bytes &&
            !capture.buffer.backend_timings && !capture.failed && !capture.reported &&
            !capture.failure_reason && !capture.index;
        observed[i].getter_ok = getter_calls - before == (i < 6 ? 1u : 0u);
        uint64_t size = 0;
        void *bytes = jce_fs_host_read_all(JCE_KPI_CONTEXT_SELECTOR_PATH, &size);
        const char *legacy = "frame_index,phase,ms\n0,frame_wall,4.0000\n"
                             "0,frame_cpu,2.5000\n0,frame_gpu,0.1250\n";
        if (i < 6 && i != 3) observed[i].file_ok = bytes && size == strlen(legacy) &&
            memcmp(bytes, legacy, (size_t)size) == 0;
        else if (i == 3) observed[i].file_ok = bytes &&
            strstr(bytes, "0,frame_backend_latched_cpu_submit,1.5000\n") &&
            strstr(bytes, "0,frame_backend_latched_wait_submit,0.0000\n") &&
            strstr(bytes, "0,frame_backend_latched_wait_render,3.5000\n");
        else observed[i].file_ok = bytes && size == sizeof sentinel - 1u &&
            memcmp(bytes, sentinel, sizeof sentinel - 1u) == 0;
        jce_fs_buffer_free(bytes); jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
    }
    /* A disabled capture does not read or reject an irrelevant selector. */
    bool disabled_ok = jce_engine_kpi_capture_open_with_context(&capture, "", NULL,
        true, 0, 0, NULL) && !capture.buffer.backend_timings;
    bool disabled_released = jce_engine_kpi_capture_destroy(&capture);
    int restored = capture_backend_environment(saved);
    SDL_free(saved);
    jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    jce_async_executor_destroy(executor);
    TEST_ASSERT_EQUAL_INT(0, restored);
    TEST_ASSERT_TRUE(disabled_ok); TEST_ASSERT_TRUE(disabled_released);
    for (unsigned i = 0; i < count; ++i) {
        const char *selector = selectors[i] ? selectors[i] : "<unset>";
        TEST_ASSERT_TRUE_MESSAGE(observed[i].env_set, selector);
        TEST_ASSERT_TRUE_MESSAGE(observed[i].env_observed,
            "fixture environment must match the production libc reader before opening capture");
        TEST_ASSERT_EQUAL_INT_MESSAGE(i < 6, observed[i].opened, selector);
        TEST_ASSERT_EQUAL_INT_MESSAGE(i == 3, observed[i].enabled, selector);
        TEST_ASSERT_EQUAL_INT(i < 6, observed[i].exported);
        TEST_ASSERT_EQUAL_INT(i < 6, observed[i].finished);
        TEST_ASSERT_TRUE(observed[i].released); TEST_ASSERT_TRUE(observed[i].file_ok);
        TEST_ASSERT_TRUE(observed[i].reset);
        if (i >= 6) {
            TEST_ASSERT_TRUE(observed[i].sentinel_written);
            TEST_ASSERT_TRUE(observed[i].failed_before_writer);
        }
        TEST_ASSERT_TRUE_MESSAGE(observed[i].getter_ok, "option added or skipped the stats query");
    }
}

static void test_context_requires_seal_and_consumes_each_completed_record_once(void)
{
    CaptureFixture missing = {0}; fixture_attach_mode(&missing, 0, true);
    bool unsealed = jce_engine_kpi_capture_export(&missing.capture, 1.0, false);
    uint64_t index = missing.capture.index;
    JceFrameKpiWriterStatus refused = capture_status(&missing);
    bool failed = missing.capture.failed;
    fixture_dispose(&missing);
    TEST_ASSERT_FALSE(unsealed); TEST_ASSERT_TRUE(failed);
    TEST_ASSERT_EQUAL_UINT64(0, index); TEST_ASSERT_EQUAL_UINT64(0, refused.accepted_records);
    TEST_ASSERT_EQUAL_UINT(0, getter_calls);

    CaptureFixture fixture = {0}; fixture_attach_mode(&fixture, 1, true);
    fixture.capture.context.window_present = true;
    fixture.capture.context.flags_begin_valid = fixture.capture.context.flags_end_valid = true;
    fixture.capture.context.flags_begin = 17; fixture.capture.context.flags_end = 19;
    fixture.capture.context.backend_frame_begin_valid = true;
    fixture.capture.context.backend_frame_begin = 411;
    fixture.capture.context_sealed = true;
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_context_active(&fixture.capture));
    bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 5.0, false);
    bool consumed = !fixture.capture.context_sealed;
    fixture.capture.context.flags_begin = 999; /* Accepted queue bytes must remain the old sample. */
    bool limited = jce_engine_kpi_capture_export(&fixture.capture, NAN, true);
    bool active = jce_engine_kpi_capture_context_active(&fixture.capture);
    JceAppResult terminal = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    uint64_t admitted = fixture.capture.index;
    fixture_dispose(&fixture);
    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(consumed); TEST_ASSERT_TRUE(limited);
    TEST_ASSERT_FALSE(active); TEST_ASSERT_EQUAL_UINT64(1, admitted);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, terminal);
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_EQUAL_UINT64(fixture.sink.size - strlen(jce_frame_kpi_csv_header(true)), status.written_bytes);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_wall,5.0000,\"{\"\"v\"\":1"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"flags_begin\"\":17,\"\"flags_end\"\":19"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "\"\"backend_frame_begin\"\":411"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, ":999"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_cpu,2.5000,\n"));
}

static void test_context_encoding_failure_retains_seal_and_rejects_terminal_success(void)
{
    CaptureFixture fixture = {0}; fixture_attach_mode(&fixture, 0, true);
    fixture.capture.context_sealed = true;
    bool accepted = jce_engine_kpi_capture_export(&fixture.capture, NAN, false);
    bool sealed = fixture.capture.context_sealed;
    uint64_t index = fixture.capture.index;
    JceAppResult terminal = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    fixture_dispose(&fixture);
    TEST_ASSERT_FALSE(accepted); TEST_ASSERT_TRUE(sealed);
    TEST_ASSERT_EQUAL_UINT64(0, index); TEST_ASSERT_EQUAL_UINT64(0, status.accepted_records);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, terminal);
}

static void test_disabled_owner_has_no_allocation_getter_or_stream(void)
{
    JceEngineKpiCapture capture = {0};
    uint64_t allocations, bytes;
    /* This consuming counter is scoped to this isolated CPU test process.
     * Establish a clean window immediately before disabled owner calls;
     * never copy this observation into production or a parallel benchmark. */
    jce_alloc_frame_delta(NULL, NULL);
    bool opened = jce_engine_kpi_capture_open(&capture, "", NULL, true, 1, 0);
    bool exported = jce_engine_kpi_capture_export(&capture, NAN, true);
    bool polled = jce_engine_kpi_capture_poll(&capture);
    JceAppResult continued = jce_engine_kpi_capture_result(&capture, JCE_APP_CONTINUE);
    JceAppResult result = jce_engine_kpi_capture_result(&capture, JCE_APP_SUCCESS);
    bool finished = jce_engine_kpi_capture_finish(&capture);
    jce_engine_kpi_capture_destroy(&capture);
    jce_engine_kpi_capture_destroy(&capture);
    jce_alloc_frame_delta(&allocations, &bytes);

    TEST_ASSERT_TRUE(opened); TEST_ASSERT_TRUE(exported);
    TEST_ASSERT_TRUE(polled); TEST_ASSERT_TRUE(finished);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, continued);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result);
    TEST_ASSERT_EQUAL_UINT(0, getter_calls);
    TEST_ASSERT_EQUAL_UINT64(0, allocations);
    TEST_ASSERT_EQUAL_UINT64(0, bytes);
    TEST_ASSERT_NULL(capture.writer); TEST_ASSERT_NULL(capture.buffer.bytes);
    TEST_ASSERT_FALSE(capture.failed); TEST_ASSERT_FALSE(capture.reported);
}

static void test_terminal_success_drains_before_return_with_exact_accounting(void)
{
    CaptureFixture fixture = {0}; fixture.sink.delay_ms = 2;
    fixture_attach(&fixture, 0);
    bool first = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
    bool second = jce_engine_kpi_capture_export(&fixture.capture, 2, false);
    JceFrameKpiWriterStatus queued = capture_status(&fixture);
    JceAppResult continued = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_CONTINUE);
    JceFrameKpiWriterStatus after_continue = capture_status(&fixture);
    unsigned writes_before_terminal = fixture.sink.writes;
    bool reported_before_terminal = fixture.capture.reported;
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus drained = capture_status(&fixture);
    bool repeated = jce_engine_kpi_capture_finish(&fixture.capture);
    bool reported = fixture.capture.reported;
    fixture_dispose(&fixture);

    TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(second);
    TEST_ASSERT_EQUAL_INT(JCE_APP_CONTINUE, continued);
    TEST_ASSERT_EQUAL_UINT64(2, queued.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(0, queued.written_records);
    TEST_ASSERT_EQUAL_UINT64(0, after_continue.written_records);
    TEST_ASSERT_FALSE(after_continue.closed);
    TEST_ASSERT_TRUE(after_continue.task_outstanding);
    TEST_ASSERT_FALSE(reported_before_terminal);
    TEST_ASSERT_EQUAL_UINT(1, writes_before_terminal); /* Header only. */
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result); TEST_ASSERT_TRUE(repeated);
    TEST_ASSERT_TRUE(reported); TEST_ASSERT_TRUE(drained.closed);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_COMPLETE, drained.state);
    TEST_ASSERT_FALSE(drained.off_thread);
    TEST_ASSERT_EQUAL_INT(JCE_ASYNC_EXECUTION_COOPERATIVE, drained.mode);
    TEST_ASSERT_EQUAL_UINT64(2, drained.written_records);
    TEST_ASSERT_EQUAL_UINT64(drained.accepted_bytes, drained.written_bytes);
    TEST_ASSERT_EQUAL_UINT64(2, drained.completion_callbacks);
    TEST_ASSERT_FALSE(drained.task_outstanding);
    TEST_ASSERT_EQUAL_UINT(0, drained.queued_records);
    TEST_ASSERT_EQUAL_UINT(0, drained.queued_bytes);
    TEST_ASSERT_EQUAL_UINT(2, fixture.sink.delayed_data_calls);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
    TEST_ASSERT_EQUAL_UINT64(fixture.sink.size - strlen("frame_index,phase,ms\n"),
                             drained.written_bytes);
}

static void test_header_failures_convert_requested_success_to_failure(void)
{
    for (unsigned flush_case = 0; flush_case < 2; ++flush_case) {
        CaptureFixture fixture = {0};
        fixture.sink.short_write_call = flush_case ? 0 : 1;
        fixture.sink.failed_flush_call = flush_case ? 1 : 0;
        fixture_attach(&fixture, 0);
        bool polled = jce_engine_kpi_capture_poll(&fixture.capture);
        JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
        JceFrameKpiWriterStatus status = capture_status(&fixture);
        bool repeated = jce_engine_kpi_capture_finish(&fixture.capture);
        fixture_dispose(&fixture);

        TEST_ASSERT_FALSE(polled); TEST_ASSERT_FALSE(repeated);
        TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result);
        TEST_ASSERT_EQUAL_INT(flush_case ? JCE_KPI_WRITER_ERROR_HEADER_FLUSH
                                        : JCE_KPI_WRITER_ERROR_HEADER_WRITE, status.error);
        TEST_ASSERT_EQUAL_UINT64(0, status.accepted_records);
        TEST_ASSERT_TRUE(status.closed);
        TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
        TEST_ASSERT_EQUAL_UINT(0, getter_calls);
    }
}

static void test_final_flush_error_overrides_fully_drained_success(void)
{
    CaptureFixture fixture = {0}; fixture.sink.failed_flush_call = 2;
    fixture_attach(&fixture, 0);
    bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    JceAppResult repeated = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    fixture_dispose(&fixture);

    TEST_ASSERT_TRUE(accepted);
    TEST_ASSERT_EQUAL_UINT64(1, status.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, repeated);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FLUSH, status.error);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_FAILED, status.state);
    TEST_ASSERT_TRUE(status.closed);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
}

static void test_close_error_overrides_fully_drained_success(void)
{
    CaptureFixture fixture = {0}; fixture.sink.failed_close = true;
    fixture_attach(&fixture, 0);
    bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    bool repeated = jce_engine_kpi_capture_finish(&fixture.capture);
    fixture_dispose(&fixture);

    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_FALSE(repeated);
    TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_CLOSE, status.error);
    TEST_ASSERT_TRUE(status.closed);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
}

static void test_record_limit_keeps_complete_csv_and_no_extra_stats_call(void)
{
    static const char expected[] = "frame_index,phase,ms\n"
        "0,frame_wall,1.0000\n0,frame_cpu,2.5000\n0,frame_gpu,0.1250\n"
        "1,frame_wall,2.0000\n1,frame_cpu,2.5000\n1,frame_gpu,0.1250\n";
    CaptureFixture fixture = {0}; fixture_attach(&fixture, 2);
    bool first = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
    bool second = jce_engine_kpi_capture_export(&fixture.capture, 2, false);
    /* At the admitted limit, even an invalid next wall is not a new record. */
    bool limited = jce_engine_kpi_capture_export(&fixture.capture, NAN, true);
    uint64_t index = fixture.capture.index;
    unsigned getters = getter_calls;
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    fixture_dispose(&fixture);

    TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(second); TEST_ASSERT_TRUE(limited);
    TEST_ASSERT_EQUAL_UINT64(2, index); TEST_ASSERT_EQUAL_UINT(2, getters);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result);
    TEST_ASSERT_EQUAL_STRING(expected, fixture.sink.bytes);
    TEST_ASSERT_EQUAL_UINT64(2, status.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(2, status.written_records);
    TEST_ASSERT_EQUAL_UINT64(sizeof expected - 1u - strlen("frame_index,phase,ms\n"),
                             status.accepted_bytes);
    TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
}

static void test_destroy_drains_real_callbacks_before_borrowed_executor_teardown(void)
{
    CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
    bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
    JceAsyncPumpBudget budget;
    jce_async_pump_budget_init(&budget);
    budget.max_work_items = 1;
    budget.max_completions = 0;
    budget.max_time_us = 0;
    jce_async_executor_pump(fixture.executor, &budget);
    JceFrameKpiWriterStatus before = capture_status(&fixture);
    jce_engine_kpi_capture_destroy(&fixture.capture);
    JceAsyncExecutorStats drained;
    jce_async_executor_get_stats(fixture.executor, &drained);
    unsigned writes = fixture.sink.writes;
    unsigned flushes = fixture.sink.flushes;
    unsigned closes = fixture.sink.closes;
    uint32_t late_work = jce_async_executor_pump(fixture.executor, NULL);
    jce_engine_kpi_capture_destroy(&fixture.capture);
    fixture_dispose(&fixture);

    TEST_ASSERT_TRUE(accepted);
    TEST_ASSERT_TRUE(before.task_outstanding);
    TEST_ASSERT_EQUAL_UINT64(0, before.completion_callbacks);
    TEST_ASSERT_EQUAL_UINT64(1, before.written_records);
    TEST_ASSERT_EQUAL_UINT(0, drained.live_tasks);
    TEST_ASSERT_EQUAL_UINT(0, drained.pending_completions);
    TEST_ASSERT_EQUAL_UINT(0, late_work);
    TEST_ASSERT_EQUAL_UINT(writes, fixture.sink.writes);
    TEST_ASSERT_EQUAL_UINT(flushes, fixture.sink.flushes);
    TEST_ASSERT_EQUAL_UINT(closes, fixture.sink.closes);
    TEST_ASSERT_EQUAL_UINT(1, closes);
    TEST_ASSERT_NULL(fixture.capture.writer);
    TEST_ASSERT_NULL(fixture.capture.buffer.bytes);
}

static void test_sink_reentry_defers_owner_mutation_until_outer_operation_finishes(void)
{
    /* Exercise both real owner entry paths; callbacks use no mocked writer. */
    for (unsigned during_finish = 0; during_finish < 2; ++during_finish) {
        CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
        fixture.sink.request_reentry = true;
        bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
        bool outer_poll = during_finish ? true
            : jce_engine_kpi_capture_poll(&fixture.capture);
        JceAppResult outer_result = jce_engine_kpi_capture_result(&fixture.capture,
                                                                 JCE_APP_SUCCESS);
        JceFrameKpiWriterStatus status = capture_status(&fixture);
        bool failed = fixture.capture.failed;
        bool reported = fixture.capture.reported;
        bool released = jce_engine_kpi_capture_destroy(&fixture.capture);
        uint32_t late_work = jce_async_executor_pump(fixture.executor, NULL);
        bool repeated_release = jce_engine_kpi_capture_destroy(&fixture.capture);
        bool disposed = fixture_dispose(&fixture);

        TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(outer_poll);
        TEST_ASSERT_EQUAL_UINT(1, fixture.sink.reentry_calls);
        TEST_ASSERT_FALSE(fixture.sink.nested_poll);
        TEST_ASSERT_FALSE(fixture.sink.nested_export);
        TEST_ASSERT_FALSE(fixture.sink.nested_finish);
        TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, fixture.sink.nested_result);
        TEST_ASSERT_FALSE(fixture.sink.nested_destroy);
        TEST_ASSERT_TRUE(fixture.sink.writer_retained);
        TEST_ASSERT_TRUE(fixture.sink.failed_retained);
        TEST_ASSERT_TRUE(fixture.sink.reported_retained);
        TEST_ASSERT_TRUE(fixture.sink.index_retained);
        TEST_ASSERT_TRUE(fixture.sink.getter_retained);
        TEST_ASSERT_TRUE(fixture.sink.buffer_retained);
        TEST_ASSERT_TRUE(fixture.sink.profile_retained);
        TEST_ASSERT_TRUE(fixture.sink.reason_retained);
        TEST_ASSERT_TRUE(fixture.sink.record_totals_retained);
        TEST_ASSERT_FALSE(fixture.sink.closed_during_reentry);
        TEST_ASSERT_FALSE(failed); TEST_ASSERT_TRUE(reported);
        TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, outer_result);
        TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_COMPLETE, status.state);
        TEST_ASSERT_EQUAL_UINT64(1, status.accepted_records);
        TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
        TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
        TEST_ASSERT_EQUAL_UINT64(1, status.completion_callbacks);
        TEST_ASSERT_FALSE(status.task_outstanding);
        TEST_ASSERT_TRUE(released); TEST_ASSERT_TRUE(repeated_release);
        TEST_ASSERT_TRUE(disposed); TEST_ASSERT_EQUAL_UINT(0, late_work);
        TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
        TEST_ASSERT_NULL(fixture.capture.writer);
    }
}


static void test_v3_sink_reentry_refuses_clock_mutation_then_recovers(void)
{
    CaptureFixture fixture = {0}; fixture_attach_mode(&fixture, 0, true);
    fixture.capture.context_version = fixture.capture.context.version = 3;
    fixture.capture.context_begun = fixture.capture.context_ended = fixture.capture.context_sealed = true;
    bool bound = jce_engine_kpi_capture_interval(&fixture.capture, 1000, 10, 11);
    fixture.sink.request_reentry = true;
    bool first = jce_engine_kpi_capture_export(&fixture.capture, 1.0, false);
    bool polled = jce_engine_kpi_capture_poll(&fixture.capture);
    bool rebound = jce_engine_kpi_capture_interval(&fixture.capture, 1000, 11, 13);
    bool recovered = fixture.capture.context.clock_valid &&
        fixture.capture.context.clock_frequency == 1000 &&
        fixture.capture.context.clock_begin == 11 && fixture.capture.context.clock_end == 13;
    fixture.capture.context_sealed = true;
    bool second = jce_engine_kpi_capture_export(&fixture.capture, 2.0, false);
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    bool disposed = fixture_dispose(&fixture);
    TEST_ASSERT_TRUE(bound); TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(polled);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.reentry_calls);
    TEST_ASSERT_FALSE(fixture.sink.nested_interval_valid);
    TEST_ASSERT_FALSE(fixture.sink.nested_interval_invalid);
    TEST_ASSERT_TRUE(fixture.sink.clock_context_retained);
    TEST_ASSERT_TRUE(fixture.sink.writer_retained); TEST_ASSERT_TRUE(fixture.sink.failed_retained);
    TEST_ASSERT_TRUE(fixture.sink.reported_retained); TEST_ASSERT_TRUE(fixture.sink.index_retained);
    TEST_ASSERT_TRUE(fixture.sink.reason_retained); TEST_ASSERT_TRUE(fixture.sink.getter_retained);
    TEST_ASSERT_TRUE(fixture.sink.profile_retained); TEST_ASSERT_TRUE(fixture.sink.record_totals_retained);
    TEST_ASSERT_FALSE(fixture.sink.closed_during_reentry);
    TEST_ASSERT_TRUE(rebound); TEST_ASSERT_TRUE(recovered); TEST_ASSERT_TRUE(second);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result); TEST_ASSERT_TRUE(disposed);
    TEST_ASSERT_EQUAL_UINT64(2, status.accepted_records); TEST_ASSERT_EQUAL_UINT64(2, status.written_records);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_wall,1.0000,"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "1,frame_wall,2.0000,"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes,
        "\"\"begin\"\":\"\"11\"\",\"\"end\"\":\"\"13\"\""));
}

static void test_invalid_backend_sample_preserves_v3_rows_and_terminal_success(void)
{
    CaptureFixture fixture = {0}; fixture_attach_mode(&fixture, 0, true);
    fixture.capture.context_version = fixture.capture.context.version = 3;
    fixture.capture.context_begun = fixture.capture.context_ended = true;
    const double cpu_ms[] = {2.5, 2.5, NAN};
    static const double gpu_ms[] = {0.125, -1.0, 0.25};
    bool bound[3], accepted[3], consumed[3];
    for (uint32_t i = 0; i < 3; ++i) {
        getter_stats.cpu_frame_ms = cpu_ms[i];
        getter_stats.gpu_ms = gpu_ms[i];
        fixture.capture.context_sealed = true;
        bound[i] = jce_engine_kpi_capture_interval(&fixture.capture, 1000, 10 + i, 11 + i);
        accepted[i] = jce_engine_kpi_capture_export(&fixture.capture, 1.0, false);
        consumed[i] = !fixture.capture.context_sealed;
    }
    uint64_t index = fixture.capture.index;
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    bool disposed = fixture_dispose(&fixture);

    for (uint32_t i = 0; i < 3; ++i) {
        TEST_ASSERT_TRUE(bound[i]);
        TEST_ASSERT_TRUE(accepted[i]);
        TEST_ASSERT_TRUE(consumed[i]);
    }
    TEST_ASSERT_EQUAL_UINT(3, getter_calls);
    TEST_ASSERT_EQUAL_UINT64(3, index);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result);
    TEST_ASSERT_TRUE(disposed);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_COMPLETE, status.state);
    TEST_ASSERT_EQUAL_UINT64(3, status.accepted_records);
    TEST_ASSERT_EQUAL_UINT64(3, status.written_records);
    TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);

    const char *first = strstr(fixture.sink.bytes, "0,frame_wall,1.0000,");
    const char *second = strstr(fixture.sink.bytes, "1,frame_wall,1.0000,");
    const char *third = strstr(fixture.sink.bytes, "2,frame_wall,1.0000,");
    TEST_ASSERT_NOT_NULL(first);
    TEST_ASSERT_NOT_NULL(second);
    TEST_ASSERT_NOT_NULL(third);
    TEST_ASSERT_TRUE(first < second && second < third);
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "3,frame_wall,"));
    TEST_ASSERT_NOT_NULL(strstr(first, "\"\"begin\"\":\"\"10\"\",\"\"end\"\":\"\"11\"\""));
    TEST_ASSERT_NOT_NULL(strstr(second, "\"\"begin\"\":\"\"11\"\",\"\"end\"\":\"\"12\"\""));
    TEST_ASSERT_NOT_NULL(strstr(third, "\"\"begin\"\":\"\"12\"\",\"\"end\"\":\"\"13\"\""));
    TEST_ASSERT_NOT_NULL(strstr(first, "0,frame_cpu,2.5000,\n0,frame_gpu,0.1250,\n"));
    TEST_ASSERT_NOT_NULL(strstr(second, "1,frame_cpu,2.5000,\n"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "1,frame_gpu,"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "2,frame_cpu,"));
    TEST_ASSERT_NOT_NULL(strstr(third, "2,frame_gpu,0.2500,\n"));
}

static void test_unavailable_gpu_stats_is_successful_observed_getter_without_gpu_rows(void)
{
    static const char expected[] = "frame_index,phase,ms\n0,frame_wall,1.0000\n";
    CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
    getter_stats.valid = false;
    jce_perf_phase_set_enabled(1);
    bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 1, false);
    JceFrameKpiProfile profile = fixture.capture.buffer.last_profile;
    bool failed = fixture.capture.failed;
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    bool disposed = fixture_dispose(&fixture);

    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_FALSE(failed);
    TEST_ASSERT_EQUAL_UINT(1, getter_calls);
    TEST_ASSERT_TRUE(profile.sampled); TEST_ASSERT_TRUE(profile.phase_requested);
    TEST_ASSERT_EQUAL_UINT(1, profile.stats_calls);
    TEST_ASSERT_EQUAL_UINT(1, profile.encode_calls);
    TEST_ASSERT_TRUE(isfinite(profile.stats_ms) && profile.stats_ms >= 0);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result); TEST_ASSERT_TRUE(disposed);
    TEST_ASSERT_EQUAL_STRING(expected, fixture.sink.bytes);
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "frame_cpu"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "frame_gpu"));
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_COMPLETE, status.state);
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
    TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
}

/* Exact production state transition plus real encoder/writer controls. These
 * synthetic phase/wall inputs do not certify an actual engine/window clock. */
static void assert_completed_rows(const CaptureSink *sink, unsigned count,
                                  const double *walls, const double *work)
{
    unsigned wall_rows[4] = {0}, work_rows[4] = {0};
    const char *line = strchr(sink->bytes, '\n');
    TEST_ASSERT_NOT_NULL(line);
    while (*++line) {
        unsigned long long index = 0; char phase[128]; double ms = 0.0;
        TEST_ASSERT_EQUAL_INT(3, sscanf(line, "%llu,%127[^,],%lf", &index, phase, &ms));
        TEST_ASSERT_TRUE(index < count);
        if (strcmp(phase, "frame_wall") == 0) {
            ++wall_rows[index];
            TEST_ASSERT_DOUBLE_WITHIN(0.00001, walls[index], ms);
        }
        if (strcmp(phase, "completed_capture_work") == 0) {
            ++work_rows[index];
            TEST_ASSERT_DOUBLE_WITHIN(0.00001, work[index], ms);
        }
        line = strchr(line, '\n');
        TEST_ASSERT_NOT_NULL(line);
    }
    for (unsigned i = 0; i < count; ++i) {
        TEST_ASSERT_EQUAL_UINT(1, wall_rows[i]);
        TEST_ASSERT_EQUAL_UINT(1, work_rows[i]);
    }
}

static void test_terminal_completed_snapshot_has_exact_render_count_and_full_tail(void)
{
    static const double walls[4] = {1.0, 2.0, 3.0, 700.0};
    static const double work[4] = {0.25, 1.25, 2.25, 699.25};
    for (unsigned count = 1; count <= 4; count += 3) {
        CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
        JceEnginePerfHitch state;
        jce_engine_perf_hitch_init(&state, false, 4.0); /* PERF_LOG=0 policy. */
        jce_perf_phase_set_enabled(1);
        TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
        jce_perf_phase_frame_tick();
        TEST_ASSERT_FALSE(state.interval_completed);
        TEST_ASSERT_EQUAL_UINT64(0, fixture.capture.index); /* No init pseudo-row. */
        for (unsigned i = 0; i < count; ++i) {
            jce_perf_phase_add("completed_capture_work", work[i]);
            if (i + 1u == count) {
                TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&state));
                TEST_ASSERT_TRUE(state.iterating);
            } else {
                jce_engine_perf_hitch_end_interval(&state, true);
                TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
                jce_perf_phase_frame_tick();
            }
            TEST_ASSERT_TRUE(state.interval_completed);
            TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.capture, walls[i],
                                                          state.interval_eligible));
            TEST_ASSERT_EQUAL_UINT64(i + 1u, fixture.capture.index);
        }
        TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,
            jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS));
        JceFrameKpiWriterStatus status = capture_status(&fixture);
        jce_engine_perf_hitch_end_interval(&state, true);
        TEST_ASSERT_FALSE(state.previous_full);
        TEST_ASSERT_TRUE(fixture_dispose(&fixture));
        TEST_ASSERT_EQUAL_UINT64(count, status.accepted_records);
        TEST_ASSERT_EQUAL_UINT64(count, status.written_records);
        TEST_ASSERT_EQUAL_UINT64(status.accepted_bytes, status.written_bytes);
        TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
        assert_completed_rows(&fixture.sink, count, walls, work);
    }
}

static void test_terminal_zero_wall_and_ineligible_phases_remain_explicit(void)
{
    CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, false, 4.0);
    jce_perf_phase_set_enabled(1);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick();
    jce_perf_phase_add("terminal_unpaired_work", 400.0);
    jce_perf_phase_set_enabled(0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_complete_terminal_interval(&state));
    TEST_ASSERT_TRUE(state.interval_completed);
    TEST_ASSERT_FALSE(state.interval_eligible);
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.capture, 0.0,
                                                  state.interval_eligible));
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,
        jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS));
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    jce_engine_perf_hitch_end_interval(&state, true);
    TEST_ASSERT_TRUE(fixture_dispose(&fixture));
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_wall,0.0000\n"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "terminal_unpaired_work"));
}

static void test_pending_completed_frame_closes_without_incomplete_current_work(void)
{
    CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, false, 4.0);
    jce_perf_phase_set_enabled(1);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick();
    jce_perf_phase_add("pending_completed_work", 80.0);
    jce_engine_perf_hitch_end_interval(&state, true);
    /* Same existing gate/state rotation used by terminal event or host destroy. */
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(state.interval_completed);
    TEST_ASSERT_TRUE(jce_engine_kpi_capture_export(&fixture.capture, 81.0,
                                                  state.interval_eligible));
    jce_perf_phase_add("unfinished_current_work", 500.0);
    jce_engine_perf_hitch_end_interval(&state, false);
    TEST_ASSERT_FALSE(state.previous_full);
    TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS,
        jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS));
    JceFrameKpiWriterStatus status = capture_status(&fixture);
    TEST_ASSERT_TRUE(fixture_dispose(&fixture));
    TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,pending_completed_work,80.0000\n"));
    TEST_ASSERT_NOT_NULL(strstr(fixture.sink.bytes, "0,frame_wall,81.0000\n"));
    TEST_ASSERT_NULL(strstr(fixture.sink.bytes, "unfinished_current_work"));
}

static void test_encoding_failures_preserve_actual_stats_actor_before_failure(void)
{
    for (unsigned invalid_phase = 0; invalid_phase < 2; ++invalid_phase) {
        CaptureFixture fixture = {0}; fixture_attach(&fixture, 0);
        jce_perf_phase_set_enabled(1);
        if (invalid_phase) {
            /* Empty name is reachable through the actual public publisher.
             * Registrations persist; keep this case last. */
            jce_perf_phase_add("", 0);
            jce_perf_phase_frame_tick();
        }
        unsigned before_getter = getter_calls;
        bool accepted = jce_engine_kpi_capture_export(&fixture.capture,
            invalid_phase ? 1 : NAN, invalid_phase != 0);
        JceFrameKpiProfile profile = fixture.capture.buffer.last_profile;
        uint64_t index = fixture.capture.index;
        const char *reason = fixture.capture.failure_reason;
        JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
        JceFrameKpiWriterStatus status = capture_status(&fixture);
        unsigned after_getter = getter_calls;
        fixture_dispose(&fixture);

        TEST_ASSERT_FALSE(accepted);
        TEST_ASSERT_EQUAL_UINT(before_getter + 1u, after_getter);
        TEST_ASSERT_TRUE(profile.sampled); TEST_ASSERT_TRUE(profile.phase_requested);
        TEST_ASSERT_EQUAL_UINT(1, profile.stats_calls);
        TEST_ASSERT_EQUAL_UINT(1, profile.encode_calls);
        TEST_ASSERT_TRUE(isfinite(profile.stats_ms));
        TEST_ASSERT_TRUE(profile.stats_ms >= 0);
        TEST_ASSERT_EQUAL_UINT64(0, index);
        TEST_ASSERT_EQUAL_UINT64(0, status.accepted_records);
        TEST_ASSERT_EQUAL_UINT64(0, status.written_records);
        TEST_ASSERT_EQUAL_UINT(1, fixture.sink.writes); /* Only original header. */
        TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result);
        TEST_ASSERT_EQUAL_STRING("frame KPI snapshot encoding failed", reason);
        TEST_ASSERT_TRUE(status.closed);
        TEST_ASSERT_EQUAL_UINT(1, fixture.sink.closes);
    }
}

/* Actual synchronous core logger before log_init; no replacement logger. */
typedef struct CaptureFooterLog {
    unsigned footers, ordinary_info, incomplete, failure_queues, failure_writes;
    JceLogLevel level;
    int line;
    char file[96], message[512];
    char failure_queue[512], failure_write[512];
} CaptureFooterLog;
static void capture_footer_log(const JceLogRecord *record, void *user)
{
    CaptureFooterLog *log = user;
    if (strcmp(record->message, "ordinary fixture INFO") == 0) ++log->ordinary_info;
    if (strstr(record->message, "frame KPI capture incomplete:") == record->message) ++log->incomplete;
    if (strstr(record->message, "frame KPI first failure:") == record->message) {
        ++log->failure_queues;
        snprintf(log->failure_queue, sizeof log->failure_queue, "%s", record->message);
    }
    if (strstr(record->message, "frame KPI failed write:") == record->message) {
        ++log->failure_writes;
        snprintf(log->failure_write, sizeof log->failure_write, "%s", record->message);
    }
    if (strstr(record->message, "frame KPI capture complete:") != record->message) return;
    ++log->footers; log->level = record->level; log->line = record->line;
    snprintf(log->file, sizeof log->file, "%s", record->file);
    snprintf(log->message, sizeof log->message, "%s", record->message);
}
static void test_failed_capture_reports_frozen_queue_task_and_write_once(void)
{
    CaptureFixture fixture = {0}; CaptureFooterLog log = {0};
    fixture_attach(&fixture, 0);
    jce_log_set_sink(capture_footer_log, &log);
    bool accepted = true;
    for (unsigned index = 0; index < 8; ++index)
        accepted = jce_engine_kpi_capture_export(&fixture.capture, 2.0, false) && accepted;
    bool rejected = !jce_engine_kpi_capture_export(&fixture.capture, 2.0, false);
    JceFrameKpiWriterStatus before = capture_status(&fixture);
    JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
    bool repeated = jce_engine_kpi_capture_finish(&fixture.capture);
    jce_log_flush(); jce_log_set_sink(NULL, NULL);
    bool released = fixture_dispose(&fixture);
    TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(rejected); TEST_ASSERT_TRUE(released);
    TEST_ASSERT_FALSE(repeated); TEST_ASSERT_EQUAL_INT(JCE_APP_FAILURE, result);
    TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_ERROR_FULL, before.error);
    TEST_ASSERT_TRUE(before.first_failure.captured);
    TEST_ASSERT_EQUAL_UINT(8, before.first_failure.queued_records);
    TEST_ASSERT_EQUAL_UINT(1, log.incomplete);
    TEST_ASSERT_EQUAL_UINT(1, log.failure_queues); TEST_ASSERT_EQUAL_UINT(1, log.failure_writes);
    char expected[128];
    snprintf(expected, sizeof expected, "queued_bytes=%llu queued_records=8",
             (unsigned long long)before.first_failure.queued_bytes);
    TEST_ASSERT_NOT_NULL(strstr(log.failure_queue, expected));
    snprintf(expected, sizeof expected, "task_state_available=1 task_state=%u",
             (unsigned)JCE_ASYNC_STATE_QUEUED);
    TEST_ASSERT_NOT_NULL(strstr(log.failure_queue, expected));
    TEST_ASSERT_NOT_NULL(strstr(log.failure_queue, "task_submitted=1 task_started=0 task_completed=0 write_calls=1"));
    TEST_ASSERT_NOT_NULL(strstr(log.failure_queue, "clock=process-monotonic-ns"));
    TEST_ASSERT_NOT_NULL(strstr(log.failure_write, "serial=1 active=0 data=0"));
    TEST_ASSERT_NOT_NULL(strstr(log.failure_write, "return_available=1"));
    TEST_ASSERT_NOT_NULL(strstr(log.failure_write, "return_published_later=0"));
    TEST_ASSERT_NULL(strstr(log.failure_write, "returned_after_failure="));
    snprintf(expected, sizeof expected, "status=%d", (int)SDL_IO_STATUS_READY);
    TEST_ASSERT_NOT_NULL(strstr(log.failure_write, expected));
}
static void test_success_footer_retains_actual_default_low_and_explicit_capacities(void)
{
    static const struct { const char *selector; bool low; size_t bytes; uint32_t records; } cases[] = {
        {NULL, false, 1048576u, 1024u}, {NULL, true, 262144u, 256u},
        {"33554432,16384", false, 33554432u, 16384u}
    };
    const char *old = getenv("JCE_KPI_WRITER_QUEUE");
    char *saved = old ? SDL_strdup(old) : NULL;
    if (old) TEST_ASSERT_NOT_NULL(saved);
    JceAsyncExecutorConfig config; jce_async_executor_config_init(&config);
    config.mode = JCE_ASYNC_EXECUTION_COOPERATIVE; config.worker_count = 0;
    config.max_tasks = 8;
    JceAsyncExecutor *executor = jce_async_executor_create(&config);
    if (!executor) { SDL_free(saved); TEST_FAIL_MESSAGE("cannot create footer-budget executor"); }
    struct { bool env, opened, exported, repeated, released, budget, prefix, footer;
        unsigned footers; JceAppResult result; } seen[3] = {0};
    jce_log_set_level(JCE_LOG_LEVEL_INFO);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        JceEngineKpiCapture capture = {0}; CaptureFooterLog log = {0};
        seen[i].env = capture_queue_environment(cases[i].selector) == 0;
        jce_log_set_sink(capture_footer_log, &log);
        seen[i].opened = jce_engine_kpi_capture_open(&capture,
            JCE_KPI_CONTEXT_SELECTOR_PATH, executor, cases[i].low, 0, 0);
        seen[i].exported = seen[i].opened && jce_engine_kpi_capture_export(&capture, 2.0, false);
        seen[i].result = jce_engine_kpi_capture_result(&capture, JCE_APP_SUCCESS);
        JceFrameKpiWriterStatus status;
        jce_frame_kpi_writer_get_status(capture.writer, &status);
        seen[i].budget = status.byte_capacity == cases[i].bytes &&
            status.record_capacity == cases[i].records &&
            status.accepted_records == 1u && status.written_records == 1u &&
            status.closed && status.state == JCE_KPI_WRITER_COMPLETE;
        seen[i].repeated = jce_engine_kpi_capture_finish(&capture);
        seen[i].released = jce_engine_kpi_capture_destroy(&capture);
        jce_log_flush(); jce_log_set_sink(NULL, NULL);
        seen[i].footers = log.footers;
        seen[i].prefix = strstr(log.message, "frame KPI capture complete: accepted=1 written=1 accepted_bytes=") == log.message &&
            strstr(log.message, " written_bytes=") && strstr(log.message, " peak_bytes=") &&
            strstr(log.message, " peak_records=");
        char expected[128];
        snprintf(expected, sizeof expected, "capacity_bytes=%llu capacity_records=%u",
            (unsigned long long)cases[i].bytes, cases[i].records);
        seen[i].footer = strstr(log.message, expected) != NULL;
        jce_fs_host_remove_file(JCE_KPI_CONTEXT_SELECTOR_PATH);
    }
    int restored = capture_queue_environment(saved); SDL_free(saved);
    jce_async_executor_shutdown(executor, JCE_ASYNC_SHUTDOWN_DRAIN, JCE_ASYNC_WAIT_INFINITE);
    jce_async_executor_destroy(executor);
    TEST_ASSERT_EQUAL_INT(0, restored);
    for (unsigned i = 0; i < sizeof cases / sizeof cases[0]; ++i) {
        TEST_ASSERT_TRUE(seen[i].env); TEST_ASSERT_TRUE(seen[i].opened);
        TEST_ASSERT_TRUE(seen[i].exported); TEST_ASSERT_TRUE(seen[i].budget);
        TEST_ASSERT_TRUE(seen[i].repeated); TEST_ASSERT_TRUE(seen[i].released);
        TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, seen[i].result);
        TEST_ASSERT_EQUAL_UINT(1u, seen[i].footers); TEST_ASSERT_TRUE(seen[i].prefix);
        TEST_ASSERT_TRUE_MESSAGE(seen[i].footer,
            cases[i].selector ? cases[i].selector : cases[i].low ? "low-memory default budget missing" : "normal default budget missing");
    }
}

static void test_explicit_capture_footer_retains_dist_source_and_respects_runtime_level(void)
{
    for (unsigned filtered = 0; filtered < 2; ++filtered) {
        CaptureFixture fixture = {0}; CaptureFooterLog log = {0};
        fixture_attach(&fixture, 0);
        jce_log_set_level(filtered ? JCE_LOG_LEVEL_ERROR : JCE_LOG_LEVEL_INFO);
        jce_log_set_sink(capture_footer_log, &log);
        LOG_INFO("fixture", "ordinary fixture INFO");
        bool accepted = jce_engine_kpi_capture_export(&fixture.capture, 2.0, false);
        JceAppResult result = jce_engine_kpi_capture_result(&fixture.capture, JCE_APP_SUCCESS);
        JceFrameKpiWriterStatus status = capture_status(&fixture);
        bool repeated = jce_engine_kpi_capture_finish(&fixture.capture);
        jce_log_flush();
        jce_log_set_sink(NULL, NULL);
        jce_log_set_level(JCE_LOG_LEVEL_INFO);
        bool released = fixture_dispose(&fixture);
        /* Clean borrowed owners and the stack sink before any assertion. */
        TEST_ASSERT_TRUE(accepted); TEST_ASSERT_TRUE(repeated); TEST_ASSERT_TRUE(released);
        TEST_ASSERT_EQUAL_INT(JCE_APP_SUCCESS, result);
        TEST_ASSERT_TRUE(status.closed); TEST_ASSERT_EQUAL_INT(JCE_KPI_WRITER_COMPLETE, status.state);
        TEST_ASSERT_EQUAL_UINT64(1, status.accepted_records);
        TEST_ASSERT_EQUAL_UINT64(1, status.written_records);
        TEST_ASSERT_EQUAL_UINT(filtered ? 0 : 1, log.footers);
#if defined(JCE_DIST)
        TEST_ASSERT_EQUAL_UINT(0, log.ordinary_info);
#else
        TEST_ASSERT_EQUAL_UINT(filtered ? 0 : 1, log.ordinary_info);
#endif
        if (!filtered) {
            TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO, log.level);
            TEST_ASSERT_GREATER_THAN(0, log.line);
            TEST_ASSERT_NOT_NULL(strstr(log.file, "jce_engine_kpi_capture.c"));
            TEST_ASSERT_NOT_NULL(strstr(log.message, "accepted=1 written=1"));
            TEST_ASSERT_NOT_NULL(strstr(log.message, "accepted_bytes="));
            TEST_ASSERT_NOT_NULL(strstr(log.message, "written_bytes="));
        }
    }
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_context_selector_is_exact_and_defaults_preserve_header);
    RUN_TEST(test_backend_selector_rejects_before_io_and_is_latched_per_reused_session);
    RUN_TEST(test_queue_selector_reaches_real_writer_and_latches_each_session);
    RUN_TEST(test_queue_selector_rejects_invalid_before_open_or_owned_allocation);
    RUN_TEST(test_context_requires_seal_and_consumes_each_completed_record_once);
    RUN_TEST(test_context_encoding_failure_retains_seal_and_rejects_terminal_success);
    RUN_TEST(test_explicit_capture_footer_retains_dist_source_and_respects_runtime_level);
    RUN_TEST(test_success_footer_retains_actual_default_low_and_explicit_capacities);
    RUN_TEST(test_failed_capture_reports_frozen_queue_task_and_write_once);
    RUN_TEST(test_disabled_owner_has_no_allocation_getter_or_stream);
    RUN_TEST(test_terminal_success_drains_before_return_with_exact_accounting);
    RUN_TEST(test_header_failures_convert_requested_success_to_failure);
    RUN_TEST(test_final_flush_error_overrides_fully_drained_success);
    RUN_TEST(test_close_error_overrides_fully_drained_success);
    RUN_TEST(test_record_limit_keeps_complete_csv_and_no_extra_stats_call);
    RUN_TEST(test_destroy_drains_real_callbacks_before_borrowed_executor_teardown);
    RUN_TEST(test_sink_reentry_defers_owner_mutation_until_outer_operation_finishes);
    RUN_TEST(test_v3_sink_reentry_refuses_clock_mutation_then_recovers);
    RUN_TEST(test_invalid_backend_sample_preserves_v3_rows_and_terminal_success);
    RUN_TEST(test_unavailable_gpu_stats_is_successful_observed_getter_without_gpu_rows);
    RUN_TEST(test_terminal_completed_snapshot_has_exact_render_count_and_full_tail);
    RUN_TEST(test_terminal_zero_wall_and_ineligible_phases_remain_explicit);
    RUN_TEST(test_pending_completed_frame_closes_without_incomplete_current_work);
    RUN_TEST(test_encoding_failures_preserve_actual_stats_actor_before_failure);
    return UNITY_END();
}
