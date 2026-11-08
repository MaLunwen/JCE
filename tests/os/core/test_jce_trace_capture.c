/* Real ring/Chrome encoder plus production streaming consumer and IO seam. */
#include "unity.h"
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <SDL3/SDL.h>
#include <string.h>
#include <stdio.h>
#include "os/core/jce_trace_capture_internal.h"
#include "os/core/jce_memory.h"
#include "os/core/jce_trace_scope_internal.h"

static SDL_AtomicInt fail_append;
static SDL_AtomicInt fail_suppression;
static SDL_AtomicInt fail_encode;
static SDL_AtomicInt short_append;
static SDL_AtomicInt write_calls;
static SDL_AtomicInt fail_footer_close;
static SDL_AtomicInt footer_close_armed;
static SDL_AtomicInt fail_rename;
static unsigned rename_calls, capture_heap_frees;
static unsigned reserve_calls, active_reservation, reservation_closes;
static unsigned fail_reserve_write_call, fail_reserve_close_call;
static bool fail_path_query;
static const char *path;
static char output_paths[16][1200];
static unsigned output_count;
static char partial_paths[16][1200];
static unsigned partial_count;
static bool partial_path_overflow;
static SDL_IOStream *partial_stream;
static unsigned partial_stream_reservation, partial_open_calls, partial_close_calls, partial_reopen_calls;
static bool partial_header_pending;
static void *observed_staging;
static unsigned staging_frees;
/* Deterministic full-owner timing ports do not certify native scheduling.
 * Other cases retain the real worker/clock and Engine integration stays real. */
static bool policy_thread_port, policy_clock;
static unsigned policy_joins;
static char policy_thread_token;
static uint64_t policy_now, policy_write_ns;
enum CaptureClockFault { CAPTURE_CLOCK_OK, CAPTURE_CLOCK_FREQUENCY,
                         CAPTURE_CLOCK_OVERFLOW, CAPTURE_CLOCK_BACKWARDS };
static enum CaptureClockFault writer_clock_fault;
static unsigned writer_clock_reads, writer_clock_fault_at;
static uint64_t capture_perf_frequency_port(void)
{
    ++writer_clock_reads;
    if (writer_clock_reads == writer_clock_fault_at) {
        if (writer_clock_fault == CAPTURE_CLOCK_FREQUENCY) return 0;
        if (writer_clock_fault == CAPTURE_CLOCK_OVERFLOW) return 1;
    }
    return policy_clock ? UINT64_C(1000000000) : jce_time_perf_freq();
}
static uint64_t capture_perf_counter_port(void)
{
    if (writer_clock_reads == writer_clock_fault_at) {
        if (writer_clock_fault == CAPTURE_CLOCK_OVERFLOW) return UINT64_MAX;
        if (writer_clock_fault == CAPTURE_CLOCK_BACKWARDS) return 0;
    }
    return policy_clock ? policy_now : jce_time_perf_counter();
}
/* Use the actual conversion owner; only its provider is controlled. The linked
 * Trace producer retains its real clock, independently of these writer ports. */
#define jce_time_perf_freq capture_perf_frequency_port
#define jce_time_perf_counter capture_perf_counter_port
#include "os/core/jce_trace_clock_internal.h"
#undef jce_time_perf_freq
#undef jce_time_perf_counter
static JceThread *capture_create_port(JceThreadFn fn, void *arg, const char *name)
{
    if (policy_thread_port) return (JceThread *)(void *)&policy_thread_token;
    return jce_thread_create(fn, arg, name);
}
static void capture_join_port(JceThread *thread)
{
    if (thread == (JceThread *)(void *)&policy_thread_token) { ++policy_joins; return; }
    jce_thread_join(thread);
}
static uint64_t capture_clock_port(void)
{
    return policy_clock ? policy_now : jce_time_ticks_ns();
}

/* Exact public host-FS owner once, with real SDL write/close. The only close
 * failure is injected after SDL actually closes the final footer's stream. */
static bool counted_fs_close(SDL_IOStream *io)
{
    bool was_partial = io == partial_stream && partial_stream != NULL;
    bool closed = SDL_CloseIO(io);
    if (was_partial) { ++partial_close_calls; partial_stream = NULL; }
    if (active_reservation) {
        ++reservation_closes;
        if (active_reservation == fail_reserve_close_call) return false;
    }
    if (SDL_GetAtomicInt(&footer_close_armed)) {
        SDL_SetAtomicInt(&footer_close_armed, 0);
        return false;
    }
    return closed;
}
static size_t counted_fs_write(SDL_IOStream *io, const void *data, size_t size)
{
    if (active_reservation == fail_reserve_write_call && active_reservation && size)
        --size;
    return SDL_WriteIO(io, data, size);
}
static SDL_IOStream *counted_fs_open(const char *file, const char *mode)
{
    SDL_IOStream *io = SDL_IOFromFile(file, mode);
    if (strstr(file, ".partial")) {
        if (strcmp(mode, "wbx") == 0 && io) { ++partial_open_calls; partial_stream = io; }
        else if (strcmp(mode, "ab") == 0) ++partial_reopen_calls;
    }
    return io;
}
static bool counted_fs_query(const char *query, SDL_PathInfo *info)
{
    if (fail_path_query && path && strcmp(query, path) == 0) return false;
    return SDL_GetPathInfo(query, info);
}
#define SDL_IOFromFile counted_fs_open
#define SDL_CloseIO counted_fs_close
#define SDL_WriteIO counted_fs_write
#define SDL_GetPathInfo counted_fs_query
#include "../../../engine/src/os/core/jce_filesystem_host.c"
#undef SDL_IOFromFile
#undef SDL_CloseIO
#undef SDL_WriteIO
#undef SDL_GetPathInfo

static void counted_capture_free(void *pointer)
{
    if (pointer) ++capture_heap_frees;
    if (pointer && pointer == observed_staging) ++staging_frees;
    jce__free_tracked(pointer);
}
static bool controlled_encode(const JceTraceEvent *events, uint32_t count,
                              bool *first, TraceJsonBuffer *json)
{
    if (SDL_GetAtomicInt(&fail_encode)) return false;
    return jce_trace_chrome_encode_batch_into(events, count, first, json);
}
static bool controlled_suppress_begin(void)
{
    if (SDL_GetAtomicInt(&fail_suppression)) return false;
    return jce_trace_suppress_begin();
}
static bool controlled_reserve_new(const char *path, const void *data, uint64_t size,
                                   bool *created)
{
    SDL_AddAtomicInt(&write_calls, 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "capture-io-recursion", 1);
    if (strstr(path, ".partial")) {
        if (partial_count >= 16 || strlen(path) >= sizeof(partial_paths[0])) {
            partial_path_overflow = true;
            return false;
        }
        strcpy(partial_paths[partial_count++], path);
    }
    active_reservation = ++reserve_calls;
    bool ok = jce__fs_host_reserve_new(path, data, size, created);
    active_reservation = 0;
    return ok;
}
static SDL_IOStream *controlled_stream_new(const char *file, bool *created)
{
    SDL_AddAtomicInt(&write_calls, 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "capture-io-recursion", 1);
    if (partial_count >= 16 || strlen(file) >= sizeof(partial_paths[0])) {
        partial_path_overflow = true;
        return NULL;
    }
    strcpy(partial_paths[partial_count++], file);
    active_reservation = ++reserve_calls;
    partial_stream_reservation = active_reservation;
    SDL_IOStream *io = jce__fs_host_stream_new(file, created);
    active_reservation = 0;
    partial_header_pending = io != NULL;
    return io;
}
static bool controlled_stream_write(SDL_IOStream *io, const void *data, uint64_t size)
{
    SDL_AddAtomicInt(&write_calls, 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "capture-io-recursion", 1);
    if (SDL_GetAtomicInt(&fail_append)) return false;
    if (SDL_GetAtomicInt(&short_append)) {
        if (size) (void)jce__fs_host_stream_write(io, data, 1);
        return false;
    }
    if (SDL_GetAtomicInt(&fail_footer_close) && size >= 13 &&
        memcmp(data, "],\"jceTrace\":", 13) == 0)
        SDL_SetAtomicInt(&footer_close_armed, 1);
    if (partial_header_pending) active_reservation = partial_stream_reservation;
    bool payload = !partial_header_pending &&
        !(size >= 13 && memcmp(data, "],\"jceTrace\":", 13) == 0);
    if (policy_clock && payload) policy_now += policy_write_ns;
    bool ok = jce__fs_host_stream_write(io, data, size);
    active_reservation = 0;
    partial_header_pending = false;
    return ok;
}
static bool controlled_stream_close(SDL_IOStream **io)
{
    active_reservation = partial_stream_reservation;
    bool ok = jce__fs_host_stream_close(io);
    active_reservation = 0;
    return ok;
}
static bool controlled_rename(const char *source, const char *destination)
{
    ++rename_calls;
    if (SDL_GetAtomicInt(&fail_rename)) return false;
    return jce_fs_host_rename(source, destination);
}
#define jce__fs_host_stream_new controlled_stream_new
#define jce__fs_host_stream_write controlled_stream_write
#define jce__fs_host_stream_close controlled_stream_close
#define jce__fs_host_reserve_new controlled_reserve_new
#define jce_fs_host_rename controlled_rename
#define jce_trace_suppress_begin controlled_suppress_begin
#define jce_trace_chrome_encode_batch_into controlled_encode
#undef JCE_FREE
#define JCE_FREE(pointer) counted_capture_free(pointer)
#define jce_trace_capture_start controlled_capture_start
#define jce_trace_capture_stop controlled_capture_stop
#define jce_trace_capture_quiesce controlled_capture_quiesce
#define jce_trace_capture_start_mode controlled_capture_start_mode
#define jce_trace_capture_poll controlled_capture_poll
JceTraceCapture *controlled_capture_start_mode(const char *path, uint32_t poll_ms,
                                              JceTraceCaptureMode mode);
bool controlled_capture_stop(JceTraceCapture *capture,
                             JceTraceCaptureResult *result);
void controlled_capture_quiesce(JceTraceCapture *capture);
#ifndef JCE_TEST_TRACE_CAPTURE_OWNER
#define JCE_TEST_TRACE_CAPTURE_OWNER "os/core/jce_trace_capture.c"
#endif
#define jce_thread_create capture_create_port
#define jce_thread_join capture_join_port
#define jce_time_ticks_ns capture_clock_port
#include JCE_TEST_TRACE_CAPTURE_OWNER
#undef jce_thread_create
#undef jce_thread_join
#undef jce_time_ticks_ns
#undef JCE_FREE
#define JCE_FREE(pointer) jce__free_tracked(pointer)
#undef jce_trace_chrome_encode_batch_into

#ifndef JCE_TEST_TRACE_CAPTURE_FILE_PREFIX
#define JCE_TEST_TRACE_CAPTURE_FILE_PREFIX "trace-continuous-control"
#endif
static void next_output(void)
{
    TEST_ASSERT_TRUE(output_count < 16);
    snprintf(output_paths[output_count], sizeof(output_paths[0]),
        "%s-%u.json", JCE_TEST_TRACE_CAPTURE_FILE_PREFIX, output_count);
    path = output_paths[output_count++];
}
static void expect_pending(void)
{
    uint64_t size = 0;
    char *bytes = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_TRUE(size >= 17);
    TEST_ASSERT_EQUAL_MEMORY("JCE_TRACE_PENDING", bytes, 17);
    TEST_ASSERT_NULL(strstr(bytes, "\"capture_complete\":true"));
    jce_fs_buffer_free(bytes);
}
void setUp(void)
{
    SDL_SetAtomicInt(&fail_append, 0);
    SDL_SetAtomicInt(&fail_suppression, 0);
    SDL_SetAtomicInt(&fail_encode, 0);
    SDL_SetAtomicInt(&short_append, 0);
    SDL_SetAtomicInt(&write_calls, 0);
    SDL_SetAtomicInt(&fail_footer_close, 0);
    SDL_SetAtomicInt(&footer_close_armed, 0);
    SDL_SetAtomicInt(&fail_rename, 0);
    rename_calls = capture_heap_frees = partial_count = 0;
    reserve_calls = active_reservation = reservation_closes = 0;
    fail_reserve_write_call = fail_reserve_close_call = 0;
    fail_path_query = false;
    partial_stream = NULL;
    partial_stream_reservation = partial_open_calls = partial_close_calls = partial_reopen_calls = 0;
    partial_header_pending = false;
    policy_thread_port = policy_clock = false;
    policy_joins = 0; policy_now = UINT64_C(1000000000); policy_write_ns = 0;
    writer_clock_fault = CAPTURE_CLOCK_OK;
    writer_clock_reads = writer_clock_fault_at = 0;
    output_count = 0;
    next_output();
    partial_path_overflow = false;
    observed_staging = NULL;
    staging_frees = 0;
    jce_trace_set_enabled(true);
    jce_trace_reset();
}
void tearDown(void)
{
    jce_trace_set_enabled(false);
    fail_path_query = false;
    for (unsigned i = 0; i < output_count; ++i) SDL_RemovePath(output_paths[i]);
    for (unsigned i = 0; i < partial_count; ++i)
        jce_fs_host_remove_file(partial_paths[i]);
    TEST_ASSERT_FALSE(partial_path_overflow);
}

static void test_more_than_ring_capacity_preserves_startup_and_tail(void)
{
    JceTraceStats stats;
    JceTraceCaptureResult result;
    jce_trace_get_stats(&stats);
    uint64_t startup_task = jce_trace_next_id();
    uint64_t startup_span = startup_task;
    jce_trace_task_submit(startup_task, 0, JCE_TRACE_TASK_SERVICE, "lifecycle.startup");
    jce_trace_task_begin(startup_span, startup_task, "lifecycle.startup", 0);
    jce_trace_task_end(startup_span, startup_task, "lifecycle.startup", JCE_TRACE_TASK_SUCCEEDED, 1);
    JceTraceCapture *capture = controlled_capture_start(path, 1);
    TEST_ASSERT_NOT_NULL(capture);
    for (unsigned i = 0; i < stats.capacity * 2; ++i) {
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "steady-control", i);
        if (i % 64 == 0) jce_thread_sleep_ms(2);
    }
    uint64_t shutdown_task = jce_trace_next_id();
    uint64_t shutdown_span = shutdown_task;
    jce_trace_task_submit(shutdown_task, 0, JCE_TRACE_TASK_SERVICE, "lifecycle.shutdown");
    jce_trace_task_begin(shutdown_span, shutdown_task, "lifecycle.shutdown", 0);
    jce_trace_task_end(shutdown_span, shutdown_task, "lifecycle.shutdown", JCE_TRACE_TASK_SUCCEEDED, 1);
    bool complete = controlled_capture_stop(capture, &result);
    if (!complete) {
        uint64_t failed_size = 0;
        const char *failed_path = result.published ? path :
            partial_count ? partial_paths[partial_count - 1] : path;
        char *failed_json = jce_fs_host_read_all(failed_path, &failed_size);
        if (failed_json) {
            jce_fs_host_write_all("trace-continuous-failed.json", failed_json, failed_size);
            jce_fs_buffer_free(failed_json);
        }
        printf("capture incomplete: recorded=%llu captured=%llu lost=%llu unexported=%llu threads=%u work=%u waits=%u write=%d generation=%d\n",
               (unsigned long long)result.final_stats.recorded_events,
               (unsigned long long)result.captured_events,
               (unsigned long long)result.lost_events,
               (unsigned long long)result.unexported_events,
               result.final_stats.active_threads, result.final_stats.active_work_items,
               result.final_stats.active_waits, result.write_ok, result.generation_stable);
    }
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(result.captured_events > stats.capacity);
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.unexported_events);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_threads);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_waits);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NULL(strstr(json, "capture-io-recursion"));
    TEST_ASSERT_NOT_NULL(strstr(json, "lifecycle.startup"));
    TEST_ASSERT_NOT_NULL(strstr(json, "lifecycle.shutdown"));
    TEST_ASSERT_NOT_NULL(strstr(json, "thread-exit"));
    TEST_ASSERT_NOT_NULL(strstr(json, "trace.capture.join"));
    char capacity[64];
    snprintf(capacity, sizeof capacity, "\"capacity\":%u", stats.capacity);
    TEST_ASSERT_NOT_NULL(strstr(json, capacity));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"gpu_completion_observed\":false"));
    TEST_ASSERT_TRUE(jce_fs_host_write_all("trace-continuous-observed.json", json, size));
    jce_fs_buffer_free(json);
}

/* Protocol/writer fixture only: these records do not invoke PNG callbacks. */
typedef struct ScreenshotProtocolRequest {
    uint64_t identity;
    bool readback;
    JceTraceTaskState outcome;
} ScreenshotProtocolRequest;
static const ScreenshotProtocolRequest screenshot_protocol[] = {
    {UINT64_C(9007199254740993), false, JCE_TRACE_TASK_SUCCEEDED},
    {UINT64_C(9007199254741007), true, JCE_TRACE_TASK_FAILED},
    {UINT64_C(9007199254741033), false, JCE_TRACE_TASK_CANCELLED},
    {UINT64_C(9007199254741099), true, JCE_TRACE_TASK_SUCCEEDED},
    {UINT64_C(9007199254741121), false, JCE_TRACE_TASK_FAILED},
    {UINT64_MAX, true, JCE_TRACE_TASK_CANCELLED}
};
static const unsigned screenshot_terminal_order[] = {1, 0, 3, 2, 5, 4};
static void screenshot_protocol_terminal_worker(void *argument)
{
    (void)argument;
    for (unsigned i = 0; i < 6; ++i) {
        const ScreenshotProtocolRequest *request =
            &screenshot_protocol[screenshot_terminal_order[i]];
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
            "renderer.screenshot.terminal", (int64_t)request->outcome,
            request->identity, 0);
    }
}
static void test_screenshot_protocol_retains_exact_ids_through_complete_writer(void)
{
    JceTraceCaptureResult result;
    JceTraceCursor cursor;
    JceTraceEvent events[32];
    unsigned admits = 0, terminals = 0;
    uint64_t admit_thread = 0, terminal_thread = 0;
    JceTraceCapture *capture = controlled_capture_start(path, 1);
    TEST_ASSERT_NOT_NULL(capture);
    uint64_t service = jce_trace_next_id();
    jce_trace_task_submit(service, 0, JCE_TRACE_TASK_SERVICE,
                          "lifecycle.screenshot_protocol");
    jce_trace_task_begin(service, service, "lifecycle.screenshot_protocol", 0);
    for (unsigned i = 0; i < 6; ++i) {
        const ScreenshotProtocolRequest *request = &screenshot_protocol[i];
        jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_RENDER,
            "renderer.screenshot.admit", request->readback ? 1 : 0,
            request->identity, 0);
    }
    JceThread *worker = jce_thread_create(screenshot_protocol_terminal_worker,
                                         NULL, "trace.screenshot.protocol");
    if (worker) jce_thread_join(worker);
    jce_trace_task_end(service, service, "lifecycle.screenshot_protocol",
                       worker ? JCE_TRACE_TASK_SUCCEEDED : JCE_TRACE_TASK_FAILED, 0);
    bool complete = controlled_capture_stop(capture, &result);
    TEST_ASSERT_NOT_NULL(worker);
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(result.capture_complete);
    TEST_ASSERT_TRUE(result.published);
    TEST_ASSERT_TRUE(result.write_ok);
    TEST_ASSERT_TRUE(result.generation_stable);
    TEST_ASSERT_FALSE(result.sequence_exhausted);
    TEST_ASSERT_EQUAL_UINT64(0, result.overwritten_before_capture);
    TEST_ASSERT_EQUAL_UINT64(0, result.cursor_lost);
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.unexported_events);
    TEST_ASSERT_EQUAL_UINT64(result.final_stats.recorded_events, result.captured_events);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_threads);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_waits);
    jce_trace_cursor_init(&cursor);
    for (;;) {
        uint32_t count = jce_trace_read(&cursor, events, 32);
        if (!count) break;
        for (uint32_t i = 0; i < count; ++i) {
            const JceTraceEvent *event = &events[i];
            bool admit = strcmp(event->name, "renderer.screenshot.admit") == 0;
            bool terminal = strcmp(event->name, "renderer.screenshot.terminal") == 0;
            if (!admit && !terminal) continue;
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_COUNTER, event->type);
            TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, event->category);
            TEST_ASSERT_EQUAL_UINT64(0, event->parent_id);
            if (admit) {
                TEST_ASSERT_TRUE(admits < 6);
                TEST_ASSERT_EQUAL_UINT64(screenshot_protocol[admits].identity, event->id);
                TEST_ASSERT_EQUAL_UINT64(screenshot_protocol[admits].readback ? 1 : 0,
                                         event->value_u64);
                if (!admits) admit_thread = event->thread_id;
                TEST_ASSERT_EQUAL_UINT64(admit_thread, event->thread_id);
                ++admits;
            } else {
                TEST_ASSERT_EQUAL_UINT(6, admits);
                TEST_ASSERT_TRUE(terminals < 6);
                const ScreenshotProtocolRequest *request =
                    &screenshot_protocol[screenshot_terminal_order[terminals]];
                TEST_ASSERT_EQUAL_UINT64(request->identity, event->id);
                TEST_ASSERT_EQUAL_UINT64((uint64_t)request->outcome, event->value_u64);
                if (!terminals) terminal_thread = event->thread_id;
                TEST_ASSERT_EQUAL_UINT64(terminal_thread, event->thread_id);
                ++terminals;
            }
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT(6, admits);
    TEST_ASSERT_EQUAL_UINT(6, terminals);
    TEST_ASSERT_TRUE(admit_thread != 0 && terminal_thread != 0 &&
                     admit_thread != terminal_thread);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"capture_complete\":true"));
    TEST_ASSERT_NOT_NULL(strstr(json, "lifecycle.screenshot_protocol"));
    TEST_ASSERT_TRUE(jce_fs_host_write_all("trace-screenshot-requests-observed.json",
                                          json, size));
    jce_fs_buffer_free(json);
}

static void test_cooperative_pump_uses_the_same_cursor_without_worker(void)
{
    JceTraceStats stats;
    JceTraceCaptureResult result;
    jce_trace_get_stats(&stats);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    controlled_capture_poll(NULL);
    for (unsigned i = 0; i < stats.capacity * 2; ++i) {
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "cooperative-control", i);
        if (i % 256 == 0) controlled_capture_poll(capture);
    }
    TEST_ASSERT_TRUE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_EQUAL_UINT(JCE_TRACE_CAPTURE_COOPERATIVE, result.mode);
    TEST_ASSERT_FALSE(result.worker_available);
    TEST_ASSERT_EQUAL_UINT64(stats.capacity * 2, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
}

static void external_producer(void *arg)
{
    (void)arg;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "other-thread-visible", 1);
}

static void test_suppression_is_thread_local_nested_and_footer_does_not_emit(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    TEST_ASSERT_FALSE(jce_trace_enabled());
    TEST_ASSERT_TRUE(jce_trace_suppress_begin());
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "suppressed-control", 1);
    JceThread *producer = jce_thread_create(external_producer, NULL, "external-producer");
    TEST_ASSERT_NOT_NULL(producer);
    jce_thread_join(producer);
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_FALSE(jce_trace_enabled());
    TEST_ASSERT_TRUE(jce_trace_suppress_end());
    TEST_ASSERT_TRUE(jce_trace_enabled());
    TEST_ASSERT_TRUE(controlled_capture_stop(capture, &result));
    JceTraceStats after;
    jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(result.final_stats.recorded_events, after.recorded_events);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "other-thread-visible"));
    TEST_ASSERT_NULL(strstr(json, "suppressed-control"));
    TEST_ASSERT_NULL(strstr(json, "capture-io-recursion"));
    jce_fs_buffer_free(json);
}

static void test_suppression_failure_never_performs_unguarded_capture_io(void)
{
    SDL_SetAtomicInt(&fail_suppression, 1);
    TEST_ASSERT_NULL(controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE));
    TEST_ASSERT_EQUAL_INT(0, SDL_GetAtomicInt(&write_calls));
    SDL_SetAtomicInt(&fail_suppression, 0);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    int writes = SDL_GetAtomicInt(&write_calls);
    SDL_SetAtomicInt(&fail_suppression, 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "guard-failure-control", 1);
    JceTraceCaptureResult result;
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_EQUAL_INT(writes, SDL_GetAtomicInt(&write_calls));
    SDL_SetAtomicInt(&fail_suppression, 0);
}

static void test_resident_export_does_not_emit_its_own_filesystem_trace(void)
{
    JceTraceStats before, after;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "resident-observer-control", 1);
    jce_trace_get_stats(&before);
    TEST_ASSERT_TRUE(jce_trace_export_chrome_json(path));
    jce_trace_get_stats(&after);
    TEST_ASSERT_EQUAL_UINT64(before.recorded_events, after.recorded_events);
    TEST_ASSERT_EQUAL_UINT(before.active_work_items, after.active_work_items);
    TEST_ASSERT_EQUAL_UINT(before.active_waits, after.active_waits);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "resident-observer-control"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"recorded\":1,\"captured\":1"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"lost\":0"));
    jce_fs_buffer_free(json);
}

static void test_lag_reset_unclosed_and_write_failure_are_explicit(void)
{
    JceTraceStats stats;
    JceTraceCaptureResult result;
    jce_trace_get_stats(&stats);
    for (unsigned i = 0; i < stats.capacity + 100; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "overwritten-prefix", i);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_TRUE(result.write_ok);
    TEST_ASSERT_TRUE(result.overwritten_before_capture >= 100);
    TEST_ASSERT_EQUAL_UINT64(result.overwritten_before_capture + result.cursor_lost, result.lost_events);
    TEST_ASSERT_EQUAL_UINT64(result.final_stats.recorded_events,
        result.captured_events + result.lost_events + result.unexported_events);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_waits);
    next_output();
    jce_trace_reset();
    capture = controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    uint64_t id = jce_trace_next_id();
    jce_trace_wait_begin(id, 0, "intentionally-unclosed");
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
    TEST_ASSERT_EQUAL_UINT(1, result.final_stats.active_waits);
    TEST_ASSERT_FALSE(result.capture_complete);
    jce_trace_wait_end(id, 0, "intentionally-unclosed", 0);
    next_output();
    capture = controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_reset();
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.generation_stable);
    next_output();
    capture = controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    SDL_SetAtomicInt(&fail_append, 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "writer-failure", 1);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_NULL(controlled_capture_start("", 1));
    TEST_ASSERT_NULL(controlled_capture_start(path, 0));
}

static void test_batches_coalesce_and_reuse_one_owned_buffer(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    for (unsigned pass = 0; pass < 2; ++pass) {
        for (unsigned i = 0; i < 4096; ++i)
            jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "merged-control", i);
        int before = SDL_GetAtomicInt(&write_calls);
        controlled_capture_poll(capture);
        TEST_ASSERT_EQUAL_INT(before + 1, SDL_GetAtomicInt(&write_calls));
        TEST_ASSERT_EQUAL_UINT64((pass + 1) * 4096, capture->captured);
        TEST_ASSERT_EQUAL_UINT64(0, capture->staged_events);
        TEST_ASSERT_EQUAL_UINT(0, capture->staging.size);
        TEST_ASSERT_TRUE(capture->staging.capacity <= CAPTURE_TEXT_LIMIT);
        if (!pass) observed_staging = capture->staging.data;
        else TEST_ASSERT_EQUAL_PTR(observed_staging, capture->staging.data);
        TEST_ASSERT_EQUAL_UINT(0, staging_frees);
    }
    TEST_ASSERT_EQUAL_UINT64(16, capture->encode_batches);
    TEST_ASSERT_EQUAL_UINT64(2, capture->append_calls);
    TEST_ASSERT_TRUE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_EQUAL_UINT(1, staging_frees);
    TEST_ASSERT_EQUAL_UINT64(8192, result.captured_events);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "\"writer_encode_batches\":16"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"writer_append_calls\":2"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"writer_max_drain_ns\":"));
    TEST_ASSERT_NULL(strstr(json, "capture-io-recursion"));
    jce_fs_buffer_free(json);
}

static void test_full_drain_budget_is_busy_then_empty_read_allows_sleep(void)
{
    JceTraceCaptureResult result;
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    for (unsigned i = 0; i < stats.capacity; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "bounded-budget", i);
    controlled_capture_poll(capture);
    TEST_ASSERT_TRUE(capture->budget_exhausted);
    TEST_ASSERT_EQUAL_UINT64(stats.capacity, capture->captured);
    TEST_ASSERT_EQUAL_UINT64(0, capture->cursor.lost_events);
    controlled_capture_poll(capture);
    TEST_ASSERT_FALSE(capture->budget_exhausted);
    TEST_ASSERT_TRUE(controlled_capture_stop(capture, &result));
}

static void test_stop_drains_full_resident_ring_and_reports_actual_capacity(void)
{
    JceTraceCaptureResult result;
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    /* No intermediate poll: stop's one final drain must cover the actual ring. */
    for (uint32_t i = 0; i < stats.capacity; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "full-stop-tail", i);
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_EQUAL_UINT64(stats.capacity, result.captured_events);
    TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_TRUE(result.capture_complete);
    TEST_ASSERT_TRUE(result.published);
    TEST_ASSERT_FALSE(result.sequence_exhausted);
    TEST_ASSERT_EQUAL_UINT(stats.capacity, result.final_stats.capacity);
    TEST_ASSERT_EQUAL_UINT64(stats.capacity, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.unexported_events);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    char capacity[64];
    snprintf(capacity, sizeof capacity, "\"capacity\":%u", stats.capacity);
    TEST_ASSERT_NOT_NULL(strstr(json, capacity));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"sequence_exhausted\":false"));
    jce_fs_buffer_free(json);
}

static void test_encoder_limit_failure_and_retry_have_no_published_success(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    capture->staging.limit = 1;
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "bounded-encode-rejection", 1);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_EQUAL_UINT64(0, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(1, result.unexported_events);
    next_output();
    jce_trace_reset();
    capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "retry-new-owner", 1);
    TEST_ASSERT_TRUE(controlled_capture_stop(capture, &result));
}

static void test_partial_append_and_encoder_failure_keep_owned_bytes_until_close(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "allocated-staging", 1);
    controlled_capture_poll(capture);
    observed_staging = capture->staging.data;
    TEST_ASSERT_NOT_NULL(observed_staging);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "partial-append", 2);
    SDL_SetAtomicInt(&short_append, 1);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_EQUAL_UINT64(1, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(1, result.unexported_events);
    TEST_ASSERT_EQUAL_UINT(1, staging_frees);
    SDL_SetAtomicInt(&short_append, 0);
    observed_staging = NULL;
    staging_frees = 0;
    next_output();
    jce_trace_reset();
    capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "allocated-staging", 1);
    controlled_capture_poll(capture);
    observed_staging = capture->staging.data;
    TEST_ASSERT_NOT_NULL(observed_staging);
    SDL_SetAtomicInt(&fail_encode, 1);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "encoder-failure", 2);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_EQUAL_UINT64(1, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(1, result.unexported_events);
    TEST_ASSERT_EQUAL_UINT(1, staging_frees);
}

static void test_actual_encoder_enforces_event_bound_and_transactional_limit(void)
{
    JceTraceEvent event = {0};
    TraceJsonBuffer json = {0};
    bool first = true;
    memset(event.name, 1, sizeof(event.name) - 1);
    event.thread_id = event.id = event.parent_id = event.value_u64 = UINT64_MAX;
    event.timestamp_ns = UINT64_MAX;
    event.value_u32[0] = event.value_u32[1] = UINT32_MAX;
    for (unsigned type = JCE_TRACE_EVENT_THREAD_REGISTER;
         type <= JCE_TRACE_EVENT_COUNTER; ++type) {
        event.type = (JceTraceEventType)type;
        json.size = 0;
        first = true;
        TEST_ASSERT_TRUE(jce_trace_chrome_encode_batch_into(&event, 1, &first, &json));
        TEST_ASSERT_TRUE(json.size <= JCE_TRACE_CHROME_EVENT_MAX_BYTES);
    }
    size_t previous = json.size;
    json.limit = previous + 1;
    TEST_ASSERT_FALSE(jce_trace_chrome_encode_batch_into(&event, 1, &first, &json));
    TEST_ASSERT_EQUAL_UINT(previous, json.size);
    TEST_ASSERT_FALSE(first);
    json.limit = 0;
    memset(event.name, 'x', sizeof(event.name));
    TEST_ASSERT_FALSE(jce_trace_chrome_encode_batch_into(&event, 1, &first, &json));
    TEST_ASSERT_EQUAL_UINT(previous, json.size);
    JCE_FREE(json.data);
}

static void test_final_footer_close_failure_keeps_positive_bytes_unpublished(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    char partial[1200];
    TEST_ASSERT_TRUE(strlen(capture->path) < sizeof(partial));
    strcpy(partial, capture->path);
    TEST_ASSERT_NOT_NULL(strstr(partial, ".partial"));
    expect_pending();
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "footer-close-owner", 1);
    unsigned before = capture_heap_frees;
    SDL_SetAtomicInt(&fail_footer_close, 1);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_FALSE(result.published);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    TEST_ASSERT_EQUAL_UINT(before + 4, capture_heap_frees);
    expect_pending();
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(partial));
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(partial, &size);
    TEST_ASSERT_NOT_NULL(json);
    /* This is precisely the old false-positive trigger. Complete bytes in
     * the retained partial are not a formally published capture. */
    TEST_ASSERT_NOT_NULL(strstr(json, "\"capture_complete\":true"));
    jce_fs_buffer_free(json);
}

static void test_publication_rename_failure_retains_partial_and_releases_once(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    char partial[1200];
    TEST_ASSERT_TRUE(strlen(capture->path) < sizeof(partial));
    strcpy(partial, capture->path);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "rename-failure-owner", 1);
    unsigned before = capture_heap_frees;
    SDL_SetAtomicInt(&fail_rename, 1);
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_FALSE(result.published);
    TEST_ASSERT_EQUAL_UINT(1, rename_calls);
    TEST_ASSERT_EQUAL_UINT(before + 4, capture_heap_frees);
    expect_pending();
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(partial));
}

static void test_existing_final_target_is_refused_without_touching_old_bytes(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "PREVIOUS", 8));
    unsigned writes = (unsigned)SDL_GetAtomicInt(&write_calls);
    TEST_ASSERT_NULL(controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE));
    TEST_ASSERT_EQUAL_UINT(writes + 1, SDL_GetAtomicInt(&write_calls));
    TEST_ASSERT_EQUAL_UINT(0, reservation_closes);
    TEST_ASSERT_EQUAL_UINT(0, partial_count);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    uint64_t size = 0;
    char *old = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_UINT64(8, size);
    TEST_ASSERT_EQUAL_MEMORY("PREVIOUS", old, 8);
    jce_fs_buffer_free(old);
}

static void test_target_created_during_capture_is_not_replaced(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    char partial[1200];
    TEST_ASSERT_TRUE(strlen(capture->path) < sizeof(partial));
    strcpy(partial, capture->path);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "late-target-owner", 1);
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "LATE_OWNER", 10));
    TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_FALSE(result.published);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(partial));
    uint64_t size = 0;
    char *old = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(old);
    TEST_ASSERT_EQUAL_UINT64(10, size);
    TEST_ASSERT_EQUAL_MEMORY("LATE_OWNER", old, 10);
    jce_fs_buffer_free(old);
}

static void test_success_publishes_only_after_footer_and_frees_every_owned_buffer(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    char partial[1200];
    TEST_ASSERT_TRUE(strlen(capture->path) < sizeof(partial));
    strcpy(partial, capture->path);
    expect_pending();
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "successful-publication-owner", 1);
    unsigned before = capture_heap_frees;
    TEST_ASSERT_TRUE(controlled_capture_stop(capture, &result));
    TEST_ASSERT_TRUE(result.write_ok);
    TEST_ASSERT_TRUE(result.capture_complete);
    TEST_ASSERT_TRUE(result.published);
    TEST_ASSERT_EQUAL_UINT(1, rename_calls);
    TEST_ASSERT_EQUAL_UINT(before + 4, capture_heap_frees);
    TEST_ASSERT_TRUE(jce_fs_host_exists_file(path));
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(partial));
}

static void test_existing_directory_is_not_reserved_or_replaced(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_create_directory(path));
    unsigned before = capture_heap_frees;
    TEST_ASSERT_NULL(controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE));
    TEST_ASSERT_TRUE(jce_fs_host_exists_dir(path));
    TEST_ASSERT_EQUAL_UINT(0, partial_count);
    TEST_ASSERT_EQUAL_UINT(0, reservation_closes);
    TEST_ASSERT_EQUAL_UINT(before + 3, capture_heap_frees);
}

static void test_failed_stat_cannot_authorize_overwriting_existing_file(void)
{
    TEST_ASSERT_TRUE(jce_fs_host_write_all(path, "EXISTING_PROOF", 14));
    fail_path_query = true;
    TEST_ASSERT_FALSE(jce_fs_host_exists_file(path)); /* actual injected query failure */
    unsigned before = capture_heap_frees;
    TEST_ASSERT_NULL(controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE));
    fail_path_query = false;
    TEST_ASSERT_EQUAL_UINT(0, reservation_closes);
    TEST_ASSERT_EQUAL_UINT(0, partial_count);
    TEST_ASSERT_EQUAL_UINT(before + 3, capture_heap_frees);
    uint64_t size = 0;
    char *bytes = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(bytes);
    TEST_ASSERT_EQUAL_UINT64(14, size);
    TEST_ASSERT_EQUAL_MEMORY("EXISTING_PROOF", bytes, 14);
    jce_fs_buffer_free(bytes);
}

static void test_reservation_short_write_keeps_invalid_evidence_and_frees_once(void)
{
    for (unsigned failing_call = 1; failing_call <= 2; ++failing_call) {
        if (failing_call == 2) next_output();
        reserve_calls = reservation_closes = 0;
        fail_reserve_write_call = failing_call;
        unsigned before = capture_heap_frees;
        TEST_ASSERT_NULL(controlled_capture_start_mode(path, 1,
            JCE_TRACE_CAPTURE_COOPERATIVE));
        TEST_ASSERT_EQUAL_UINT(failing_call, reservation_closes);
        TEST_ASSERT_EQUAL_UINT(before + 3, capture_heap_frees);
        TEST_ASSERT_EQUAL_UINT(0, rename_calls);
        expect_pending();
        if (failing_call == 2) {
            TEST_ASSERT_EQUAL_UINT(1, partial_count);
            TEST_ASSERT_TRUE(jce_fs_host_exists_file(partial_paths[0]));
        }
    }
}

static void test_reservation_close_failure_keeps_invalid_evidence_and_frees_once(void)
{
    for (unsigned failing_call = 1; failing_call <= 2; ++failing_call) {
        if (failing_call == 2) {
            next_output();
            /* Previous episode's expect_pending is a real traced host read.
             * This independent close-only episode has no payload/staging. */
            jce_trace_reset();
        }
        reserve_calls = reservation_closes = 0;
        fail_reserve_close_call = failing_call;
        unsigned before = capture_heap_frees;
        JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
            JCE_TRACE_CAPTURE_COOPERATIVE);
        if (failing_call == 1) TEST_ASSERT_NULL(capture);
        else {
            /* The partial stream now stays open; its close failure belongs to
             * stop rather than header initialization. The same refusal holds. */
            TEST_ASSERT_NOT_NULL(capture);
            TEST_ASSERT_EQUAL_UINT(1, reservation_closes);
            TEST_ASSERT_NULL(capture->staging.data);
            JceTraceStats initial;
            jce_trace_get_stats(&initial);
            TEST_ASSERT_EQUAL_UINT64(0, initial.recorded_events);
            JceTraceCaptureResult result;
            TEST_ASSERT_FALSE(controlled_capture_stop(capture, &result));
            TEST_ASSERT_FALSE(result.published);
            TEST_ASSERT_EQUAL_UINT64(0, result.captured_events);
        }
        TEST_ASSERT_EQUAL_UINT(failing_call, reservation_closes);
        TEST_ASSERT_EQUAL_UINT(before + 3, capture_heap_frees);
        TEST_ASSERT_EQUAL_UINT(0, rename_calls);
        expect_pending();
        if (failing_call == 2) {
            TEST_ASSERT_EQUAL_UINT(1, partial_count);
            TEST_ASSERT_TRUE(jce_fs_host_exists_file(partial_paths[0]));
        }
    }
}
static void test_header_payload_footer_share_one_exclusive_stream_until_close(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    SDL_IOStream *owned = capture->stream;
    TEST_ASSERT_NOT_NULL(owned);
    TEST_ASSERT_EQUAL_UINT(1, partial_open_calls);
    TEST_ASSERT_EQUAL_UINT(0, partial_close_calls);
    for (unsigned pass = 0; pass < 3; ++pass) {
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "persistent-stream-control", pass);
        controlled_capture_poll(capture);
        TEST_ASSERT_EQUAL_PTR(owned, capture->stream);
        TEST_ASSERT_EQUAL_UINT(0, partial_close_calls);
    }
    bool stopped = controlled_capture_stop(capture, &result);
    /* All owned IO was closed before assertions, also for runtime mutants. */
    TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_EQUAL_UINT(1, partial_open_calls);
    TEST_ASSERT_EQUAL_UINT(0, partial_reopen_calls);
    TEST_ASSERT_EQUAL_UINT(1, partial_close_calls);
    TEST_ASSERT_NULL(partial_stream);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NOT_NULL(strstr(json, "persistent-stream-control"));
    TEST_ASSERT_NOT_NULL(strstr(json, "\"writer_io_model\":\"persistent-exclusive\""));
    jce_fs_buffer_free(json);
}
static void test_failed_payload_still_closes_persistent_stream_once(void)
{
    JceTraceCaptureResult result;
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "failed-persistent-stream", 1);
    SDL_SetAtomicInt(&fail_append, 1);
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_FALSE(stopped);
    TEST_ASSERT_FALSE(result.published);
    TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_EQUAL_UINT(1, partial_close_calls);
    TEST_ASSERT_EQUAL_UINT(0, rename_calls);
    TEST_ASSERT_NULL(partial_stream);
    expect_pending();
}

static JceTraceCapture *start_threaded_timing_owner(void)
{
    policy_thread_port = policy_clock = true;
    return controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_THREADED);
}
static void test_threaded_short_polls_keep_one_pending_batch_until_forced_stop(void)
{
    JceTraceCapture *capture = start_threaded_timing_owner();
    TEST_ASSERT_NOT_NULL(capture);
    bool drained = true;
    for (unsigned i = 0; i < 3; ++i) {
        policy_now = UINT64_C(1000000000) + i * UINT64_C(1000000);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "threaded-small", i);
        drained = capture_drain(capture, false) && drained;
    }
    uint64_t pending = capture->staged_events, calls = capture->append_calls;
    uint64_t committed = capture->captured, first = capture->staging_started_ns;
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_EQUAL_UINT64(3, pending); TEST_ASSERT_EQUAL_UINT64(0, calls);
    TEST_ASSERT_EQUAL_UINT64(0, committed); TEST_ASSERT_EQUAL_UINT64(UINT64_C(1000000000), first);
    TEST_ASSERT_EQUAL_UINT64(5, result.captured_events); /* 3 counters + owner join B/E. */
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events); TEST_ASSERT_EQUAL_UINT(1, policy_joins);
}
static void test_threaded_deadline_is_not_renewed_by_new_events_or_long_poll(void)
{
    policy_now = 0;
    JceTraceCapture *capture = start_threaded_timing_owner();
    TEST_ASSERT_NOT_NULL(capture);
    capture->poll_ms = 1000;
    bool drained = true;
    for (unsigned i = 0; i < 16; ++i) {
        policy_now = i * UINT64_C(1000000);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "deadline-owner", i);
        drained = capture_drain(capture, false) && drained;
    }
    uint64_t pending = capture->staged_events, started = capture->staging_started_ns;
    uint64_t before = capture->captured;
    uint32_t sleep = capture_sleep_ms(capture);
    policy_now = UINT64_C(16000000);
    drained = capture_drain(capture, false) && drained; /* Empty read still flushes on deadline. */
    uint64_t after = capture->captured, calls = capture->append_calls;
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_EQUAL_UINT64(16, pending); TEST_ASSERT_EQUAL_UINT64(0, started);
    TEST_ASSERT_EQUAL_UINT64(0, before); TEST_ASSERT_EQUAL_UINT(1, sleep);
    TEST_ASSERT_EQUAL_UINT64(16, after); TEST_ASSERT_EQUAL_UINT64(1, calls);
    TEST_ASSERT_EQUAL_UINT64(18, result.captured_events); TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
}
static void test_threaded_full_buffer_flushes_without_waiting_for_deadline(void)
{
    JceTraceCapture *capture = start_threaded_timing_owner();
    TEST_ASSERT_NOT_NULL(capture);
    char escaped[JCE_TRACE_NAME_CAP];
    memset(escaped, 1, sizeof(escaped) - 1); escaped[sizeof(escaped) - 1] = 0;
    for (unsigned i = 0; i < 4096; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, escaped, i);
    bool drained = capture_drain(capture, false);
    uint64_t calls = capture->append_calls, flushed = capture->slowest_append_requested_bytes;
    size_t capacity = capture->staging.capacity;
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(stopped); TEST_ASSERT_TRUE(calls > 0);
    TEST_ASSERT_TRUE(flushed >= CAPTURE_FLUSH_BYTES); TEST_ASSERT_TRUE(capacity <= CAPTURE_TEXT_LIMIT);
    TEST_ASSERT_EQUAL_UINT64(4098, result.captured_events); TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
}
static void test_threaded_stop_forces_pending_bytes_before_complete_accounting(void)
{
    JceTraceCapture *capture = start_threaded_timing_owner();
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "forced-tail", 1);
    bool drained = capture_drain(capture, false);
    uint64_t committed = capture->captured;
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_TRUE(drained); TEST_ASSERT_TRUE(stopped); TEST_ASSERT_TRUE(result.published);
    TEST_ASSERT_EQUAL_UINT64(0, committed); TEST_ASSERT_EQUAL_UINT64(3, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.unexported_events); TEST_ASSERT_EQUAL_UINT(1, partial_close_calls);
}
static void test_threaded_deferred_write_failure_keeps_pending_and_unexported_counts(void)
{
    JceTraceCapture *capture = start_threaded_timing_owner();
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "deferred-failure", 1);
    bool drained = capture_drain(capture, false);
    SDL_SetAtomicInt(&fail_append, 1);
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_TRUE(drained); TEST_ASSERT_FALSE(stopped); TEST_ASSERT_FALSE(result.write_ok);
    TEST_ASSERT_FALSE(result.published); TEST_ASSERT_EQUAL_UINT64(0, result.captured_events);
    TEST_ASSERT_EQUAL_UINT64(3, result.unexported_events); TEST_ASSERT_EQUAL_UINT(1, partial_close_calls);
    expect_pending();
}
static void test_write_diagnostics_use_actual_payload_sizes_and_one_timed_sample(void)
{
    JceTraceCapture *capture = start_threaded_timing_owner();
    TEST_ASSERT_NOT_NULL(capture);
    policy_write_ns = UINT64_C(100000);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "timed-first", 1);
    bool first = capture_drain(capture, true);
    policy_write_ns = UINT64_C(2000000);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "timed-slowest", 2);
    bool second = capture_drain(capture, true);
    uint64_t started = capture->slowest_append_started_ns;
    uint64_t finished = capture->slowest_append_finished_ns;
    uint64_t requested = capture->slowest_append_requested_bytes;
    uint64_t size_count = capture->append_size_buckets[0];
    uint64_t fast_count = capture->append_wall_buckets[0], slow_count = capture->append_wall_buckets[2];
    policy_write_ns = 0; /* Stop's join-tail payload must not replace the slowest sample. */
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    TEST_ASSERT_TRUE(first); TEST_ASSERT_TRUE(second); TEST_ASSERT_TRUE(stopped);
    TEST_ASSERT_EQUAL_UINT64(2, size_count); TEST_ASSERT_EQUAL_UINT64(1, fast_count);
    TEST_ASSERT_EQUAL_UINT64(1, slow_count); TEST_ASSERT_TRUE(requested > 0 && requested < 4096);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2000000), finished - started);
    uint64_t size = 0; char *json = jce_fs_host_read_all(path, &size);
    TEST_ASSERT_NOT_NULL(json);
    bool has_counts = strstr(json, "\"size_counts\":[3,0,0,0,0,0]") &&
        strstr(json, "\"wall_ns_counts\":[2,0,1,0,0,0,0]");
    char sample[192];
    snprintf(sample, sizeof(sample), "\"slowest_started_ns\":%llu,\"slowest_finished_ns\":%llu,\"slowest_requested_bytes\":%llu",
        (unsigned long long)started, (unsigned long long)finished, (unsigned long long)requested);
    bool has_sample = strstr(json, sample) != NULL;
    bool no_recursion = strstr(json, "capture-io-recursion") == NULL;
    jce_fs_buffer_free(json);
    TEST_ASSERT_TRUE(has_counts); TEST_ASSERT_TRUE(has_sample); TEST_ASSERT_TRUE(no_recursion);
    static const uint64_t sizes[5] = {4096,16384,65536,262144,1048576};
    static const uint64_t walls[6] = {100000,1000000,4000000,16000000,100000000,1000000000};
    for (unsigned i = 0; i < 5; ++i) {
        TEST_ASSERT_EQUAL_UINT(i, capture_size_bucket(sizes[i]));
        TEST_ASSERT_EQUAL_UINT(i + 1, capture_size_bucket(sizes[i] + 1));
    }
    for (unsigned i = 0; i < 6; ++i) {
        TEST_ASSERT_EQUAL_UINT(i, capture_wall_bucket(walls[i]));
        TEST_ASSERT_EQUAL_UINT(i + 1, capture_wall_bucket(walls[i] + 1));
    }
}

static void test_reader_diagnostics_are_published_without_loss(void)
{
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1,
        JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "reader-diagnostic-owner", 1);
    controlled_capture_poll(capture);
    JceTraceCaptureResult result;
    bool stopped = controlled_capture_stop(capture, &result);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(path, &size);
    bool present = json && strstr(json, "\"reader_diagnostics\":{\"version\":1") &&
        strstr(json, "\"first_loss\":null") && strstr(json, "\"last_loss\":null");
    jce_fs_buffer_free(json);
    TEST_ASSERT_TRUE(stopped); TEST_ASSERT_TRUE(result.capture_complete);
    TEST_ASSERT_TRUE(present);
}

static bool reader_footer_tail_contains(const char *partial, const char *expected)
{
    char bytes[8192];
    SDL_IOStream *io = SDL_IOFromFile(partial, "rb");
    if (!io) return false;
    Sint64 size = SDL_GetIOSize(io);
    Sint64 begin = size > (Sint64)(sizeof(bytes) - 1u) ?
        size - (Sint64)(sizeof(bytes) - 1u) : 0;
    bool ok = SDL_SeekIO(io, begin, SDL_IO_SEEK_SET) == begin;
    size_t read = ok ? SDL_ReadIO(io, bytes, sizeof(bytes) - 1u) : 0;
    bool closed = SDL_CloseIO(io); bytes[read] = 0;
    return ok && closed && strstr(bytes, expected) != NULL;
}
static void test_reader_loss_retains_first_last_and_previous_progress(void)
{
    JceTraceStats stats; jce_trace_get_stats(&stats);
    JceTraceCapture *capture = controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
    TEST_ASSERT_NOT_NULL(capture);
    for (unsigned i = 0; i < stats.capacity + 3; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "first-observed-loss", i);
    policy_clock = true; policy_now = UINT64_C(1000000000);
    controlled_capture_poll(capture);
    CaptureLossWitness first = capture->first_loss;
    for (unsigned i = 0; i < stats.capacity + 5; ++i)
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "last-observed-loss", i);
    policy_now = UINT64_C(2000000000); controlled_capture_poll(capture);
    CaptureLossWitness retained = capture->first_loss, last = capture->last_loss;
    uint64_t losses = capture->reader_loss_reads, events = capture->reader_loss_events;
    char partial[1200]; strcpy(partial, capture->path);
    JceTraceCaptureResult result; bool stopped = controlled_capture_stop(capture, &result);
    policy_clock = false;
    TEST_ASSERT_FALSE(stopped); TEST_ASSERT_FALSE(result.capture_complete);
    TEST_ASSERT_TRUE(result.write_ok); TEST_ASSERT_EQUAL_UINT64(8, result.lost_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.unexported_events);
    TEST_ASSERT_EQUAL_UINT64(2, losses); TEST_ASSERT_EQUAL_UINT64(8, events);
    TEST_ASSERT_TRUE(first.available); TEST_ASSERT_TRUE(last.available);
    char first_json[1536], retained_json[1536];
    TEST_ASSERT_TRUE(capture_loss_json(first_json, sizeof(first_json), &first));
    TEST_ASSERT_TRUE(capture_loss_json(retained_json, sizeof(retained_json), &retained));
    TEST_ASSERT_EQUAL_STRING(first_json, retained_json);
    TEST_ASSERT_EQUAL_UINT64(1, first.read); TEST_ASSERT_EQUAL_UINT64(3, first.loss_delta);
    TEST_ASSERT_EQUAL_UINT64(3, first.lost_total); TEST_ASSERT_EQUAL_UINT(1, first.phase);
    TEST_ASSERT_TRUE(first.clock_valid); TEST_ASSERT_EQUAL_UINT64(UINT64_C(1000000000), first.observed_ns);
    TEST_ASSERT_FALSE(first.previous_valid);
    TEST_ASSERT_TRUE(last.read > first.read); TEST_ASSERT_TRUE(last.previous_valid);
    TEST_ASSERT_EQUAL_UINT64(5, last.loss_delta); TEST_ASSERT_EQUAL_UINT64(8, last.lost_total);
    TEST_ASSERT_EQUAL_UINT64(UINT64_C(2000000000), last.observed_ns);
    TEST_ASSERT_EQUAL_UINT(stats.capacity + 3, last.previous.write_sequence);
    TEST_ASSERT_TRUE(reader_footer_tail_contains(partial, "\"loss_reads\":2,\"loss_events\":8"));
    TEST_ASSERT_TRUE(reader_footer_tail_contains(partial, "\"first_loss\":{\"read\":1,\"phase\":1"));
}
static void test_reader_loss_phase_and_unavailable_clock_do_not_fabricate_evidence(void)
{
    for (unsigned mode = 0; mode < 3; ++mode) {
        if (mode) { next_output(); jce_trace_reset(); }
        JceTraceStats stats; jce_trace_get_stats(&stats);
        JceTraceCapture *capture = mode == 0 ? start_threaded_timing_owner() :
            controlled_capture_start_mode(path, 1, JCE_TRACE_CAPTURE_COOPERATIVE);
        TEST_ASSERT_NOT_NULL(capture);
        for (unsigned i = 0; i < stats.capacity + 7; ++i)
            jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "phase-loss", i);
        if (mode == 0) {
            bool drained = capture_drain(capture, false);
            TEST_ASSERT_TRUE(drained);
        } else if (mode == 1) {
            writer_clock_reads = 0; writer_clock_fault_at = 1;
            writer_clock_fault = CAPTURE_CLOCK_FREQUENCY;
            controlled_capture_poll(capture);
            writer_clock_fault = CAPTURE_CLOCK_OK; writer_clock_fault_at = 0;
        }
        CaptureLossWitness witness = capture->first_loss;
        if (mode == 2) {
            capture->finalizing = true; // Same actual owner boundary as stop.
            bool drained = capture_drain(capture, true); TEST_ASSERT_TRUE(drained);
            witness = capture->first_loss;
        }
        char partial[1200]; strcpy(partial, capture->path);
        JceTraceCaptureResult result; bool stopped = controlled_capture_stop(capture, &result);
        TEST_ASSERT_FALSE(stopped); TEST_ASSERT_TRUE(result.write_ok);
        TEST_ASSERT_TRUE(witness.available); TEST_ASSERT_EQUAL_UINT(mode == 0 ? 0u : mode == 1 ? 1u : 2u, witness.phase);
        if (mode == 1) {
            TEST_ASSERT_FALSE(witness.clock_valid); TEST_ASSERT_EQUAL_UINT64(0, witness.observed_ns);
            TEST_ASSERT_TRUE(reader_footer_tail_contains(partial, "\"clock_valid\":false,\"observed_ns\":0"));
        }
        expect_pending();
    }
}
static void test_reader_diagnostics_have_fixed_checked_maximum_storage(void)
{
    JceTraceCapture capture = {0}; char json[4096], tiny[8];
    capture.reader_reads = capture.reader_loss_reads = capture.reader_loss_events = UINT64_MAX;
    for (unsigned i = 0; i < JCE_TRACE_READ_STOP_COUNT; ++i) capture.reader_stop_counts[i] = UINT64_MAX;
    capture.reader_has_previous = true;
    CaptureLossWitness *loss = &capture.first_loss;
    loss->available = loss->clock_valid = loss->previous_valid = true;
    loss->read = loss->observed_ns = loss->loss_delta = loss->lost_total = UINT64_MAX;
    loss->phase = UINT32_MAX;
    loss->snapshot.write_sequence = loss->snapshot.earliest_sequence = UINT32_MAX;
    loss->snapshot.next_before = loss->snapshot.next_after = loss->snapshot.count = UINT32_MAX;
    loss->snapshot.stop = JCE_TRACE_READ_INVALID_ARGS;
    loss->snapshot.blocked_sequence = loss->snapshot.published_sequence = UINT32_MAX;
    loss->snapshot.published_valid = true; loss->previous = loss->snapshot;
    capture.last_loss = *loss; capture.reader_previous = loss->snapshot;
    TEST_ASSERT_TRUE(capture_reader_json(json, sizeof(json), &capture));
    TEST_ASSERT_TRUE(strlen(json) < sizeof(json));
    TEST_ASSERT_FALSE(capture_reader_json(tiny, sizeof(tiny), &capture));
    TEST_ASSERT_FALSE(capture_read_json(tiny, sizeof(tiny), &loss->snapshot));
    TEST_ASSERT_FALSE(capture_loss_json(tiny, sizeof(tiny), loss));
    // Invalid maximum-width numeric vectors test storage only, not a valid capture.
}

#include "trace_capture_clock_cases.h"

int main(void)
{
    UNITY_BEGIN();
#if defined(JCE_TEST_TRACE_CAPTURE_CLOCK_ONLY)
    RUN_TEST(test_payload_clock_stays_with_trace_across_real_sdl_restart);
    RUN_TEST(test_payload_clock_failures_never_replay_or_publish);
#elif defined(JCE_TEST_TRACE_CAPTURE_CLOCK_AXIS_ONLY)
    RUN_TEST(test_payload_clock_stays_with_trace_across_real_sdl_restart);
#elif defined(JCE_TEST_TRACE_CAPTURE_CLOCK_FAILURE_ONLY)
    RUN_TEST(test_payload_clock_failures_never_replay_or_publish);
#elif defined(JCE_TEST_TRACE_CAPTURE_PERSISTENT_ONLY)
    RUN_TEST(test_header_payload_footer_share_one_exclusive_stream_until_close);
#elif defined(JCE_TEST_TRACE_CAPTURE_CLOSE_ONLY)
    RUN_TEST(test_final_footer_close_failure_keeps_positive_bytes_unpublished);
#elif defined(JCE_TEST_TRACE_CAPTURE_SHORT_ONLY)
    RUN_TEST(test_threaded_short_polls_keep_one_pending_batch_until_forced_stop);
#elif defined(JCE_TEST_TRACE_CAPTURE_DEADLINE_ONLY)
    RUN_TEST(test_threaded_deadline_is_not_renewed_by_new_events_or_long_poll);
#elif defined(JCE_TEST_TRACE_CAPTURE_STOP_ONLY)
    RUN_TEST(test_threaded_stop_forces_pending_bytes_before_complete_accounting);
#elif defined(JCE_TEST_TRACE_CAPTURE_DIAGNOSTICS_ONLY)
    RUN_TEST(test_write_diagnostics_use_actual_payload_sizes_and_one_timed_sample);
#elif defined(JCE_TEST_TRACE_CAPTURE_READER_ONLY)
    RUN_TEST(test_reader_diagnostics_are_published_without_loss);
    RUN_TEST(test_reader_loss_retains_first_last_and_previous_progress);
    RUN_TEST(test_reader_loss_phase_and_unavailable_clock_do_not_fabricate_evidence);
    RUN_TEST(test_reader_diagnostics_have_fixed_checked_maximum_storage);
#elif defined(JCE_TEST_TRACE_CAPTURE_FULL_TAIL_ONLY)
    RUN_TEST(test_stop_drains_full_resident_ring_and_reports_actual_capacity);
#else
    RUN_TEST(test_reader_diagnostics_are_published_without_loss);
    RUN_TEST(test_reader_loss_retains_first_last_and_previous_progress);
    RUN_TEST(test_reader_loss_phase_and_unavailable_clock_do_not_fabricate_evidence);
    RUN_TEST(test_reader_diagnostics_have_fixed_checked_maximum_storage);
    RUN_TEST(test_threaded_short_polls_keep_one_pending_batch_until_forced_stop);
    RUN_TEST(test_threaded_deadline_is_not_renewed_by_new_events_or_long_poll);
    RUN_TEST(test_threaded_full_buffer_flushes_without_waiting_for_deadline);
    RUN_TEST(test_threaded_stop_forces_pending_bytes_before_complete_accounting);
    RUN_TEST(test_threaded_deferred_write_failure_keeps_pending_and_unexported_counts);
    RUN_TEST(test_write_diagnostics_use_actual_payload_sizes_and_one_timed_sample);
    RUN_TEST(test_more_than_ring_capacity_preserves_startup_and_tail);
    RUN_TEST(test_screenshot_protocol_retains_exact_ids_through_complete_writer);
    RUN_TEST(test_cooperative_pump_uses_the_same_cursor_without_worker);
    RUN_TEST(test_suppression_is_thread_local_nested_and_footer_does_not_emit);
    RUN_TEST(test_suppression_failure_never_performs_unguarded_capture_io);
    RUN_TEST(test_resident_export_does_not_emit_its_own_filesystem_trace);
    RUN_TEST(test_lag_reset_unclosed_and_write_failure_are_explicit);
    RUN_TEST(test_batches_coalesce_and_reuse_one_owned_buffer);
    RUN_TEST(test_full_drain_budget_is_busy_then_empty_read_allows_sleep);
    RUN_TEST(test_stop_drains_full_resident_ring_and_reports_actual_capacity);
    RUN_TEST(test_encoder_limit_failure_and_retry_have_no_published_success);
    RUN_TEST(test_partial_append_and_encoder_failure_keep_owned_bytes_until_close);
    RUN_TEST(test_actual_encoder_enforces_event_bound_and_transactional_limit);
    RUN_TEST(test_final_footer_close_failure_keeps_positive_bytes_unpublished);
    RUN_TEST(test_publication_rename_failure_retains_partial_and_releases_once);
    RUN_TEST(test_existing_final_target_is_refused_without_touching_old_bytes);
    RUN_TEST(test_target_created_during_capture_is_not_replaced);
    RUN_TEST(test_success_publishes_only_after_footer_and_frees_every_owned_buffer);
    RUN_TEST(test_existing_directory_is_not_reserved_or_replaced);
    RUN_TEST(test_failed_stat_cannot_authorize_overwriting_existing_file);
    RUN_TEST(test_reservation_short_write_keeps_invalid_evidence_and_frees_once);
    RUN_TEST(test_reservation_close_failure_keeps_invalid_evidence_and_frees_once);
    RUN_TEST(test_header_payload_footer_share_one_exclusive_stream_until_close);
    RUN_TEST(test_failed_payload_still_closes_persistent_stream_once);
#endif
    return UNITY_END();
}
