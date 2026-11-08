/* Actual logger TU/native join and actual continuous capture; only the two
 * shared Engine call ports observe ordering. No window or GPU is created. */
#include "unity.h"
#include <SDL3/SDL.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_trace.h>
#include "os/core/jce_log_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static unsigned diagnostic_log_joins;
static bool diagnostic_check_order, diagnostic_capture_joined;
static bool diagnostic_bad_order;
static unsigned diagnostic_order[4], diagnostic_order_count;
static char diagnostic_accepted_bytes[16384];
static size_t diagnostic_accepted_size;
static bool diagnostic_accepted_overflow;
static bool diagnostic_fail_flush, diagnostic_fail_close;
static unsigned diagnostic_final_closes;
static size_t SDLCALL diagnostic_log_write_port(SDL_IOStream *io,
                                               const void *data, size_t size);
static bool SDLCALL diagnostic_log_flush_port(SDL_IOStream *io);
static bool SDLCALL diagnostic_log_close_port(SDL_IOStream *io);

static void SDLCALL diagnostic_log_join(SDL_Thread *thread, int *status)
{
    if (diagnostic_check_order && !diagnostic_capture_joined)
        diagnostic_bad_order = true;
    SDL_WaitThread(thread, status); /* Count a completed native join, not intent. */
    ++diagnostic_log_joins;
}
#define SDL_WaitThread diagnostic_log_join
#define SDL_WriteIO diagnostic_log_write_port
#define SDL_FlushIO diagnostic_log_flush_port
#define SDL_CloseIO diagnostic_log_close_port
#ifndef JCE_TEST_LOG_OWNER
#define JCE_TEST_LOG_OWNER "os/core/jce_log.c"
#endif
#include JCE_TEST_LOG_OWNER
#undef SDL_WaitThread
#undef SDL_WriteIO
#undef SDL_FlushIO
#undef SDL_CloseIO

static bool SDLCALL diagnostic_log_flush_port(SDL_IOStream *io)
{
    const bool actual = SDL_FlushIO(io);
    return actual && !(io == g_log_file && diagnostic_fail_flush);
}
static bool SDLCALL diagnostic_log_close_port(SDL_IOStream *io)
{
    const bool target = io == g_log_file;
    const bool actual = SDL_CloseIO(io); /* Even failures retire the real IO. */
    if (target) ++diagnostic_final_closes;
    return actual && !(target && diagnostic_fail_close);
}

static size_t SDLCALL diagnostic_log_write_port(SDL_IOStream *io,
                                               const void *data, size_t size)
{
    size_t accepted = SDL_WriteIO(io, data, size);
    if (io == g_log_file) {
        if (accepted > sizeof(diagnostic_accepted_bytes) - diagnostic_accepted_size - 1)
            diagnostic_accepted_overflow = true;
        else {
            memcpy(diagnostic_accepted_bytes + diagnostic_accepted_size, data, accepted);
            diagnostic_accepted_size += accepted;
            diagnostic_accepted_bytes[diagnostic_accepted_size] = '\0';
        }
    }
    return accepted;
}

static void diagnostic_capture_quiesce(JceTraceCapture *capture)
{
    if (diagnostic_order_count < 4) diagnostic_order[diagnostic_order_count++] = 1;
    jce_trace_capture_quiesce(capture);
    diagnostic_capture_joined = true;
}
static void diagnostic_log_quiesce(void)
{
    if (diagnostic_order_count < 4) diagnostic_order[diagnostic_order_count++] = 2;
    /* Missing/reordered-wire controls must fail safely, without racing owners. */
    if (!diagnostic_capture_joined) { diagnostic_bad_order = true; return; }
    jce_log_quiesce_internal();
}
#define jce_trace_capture_quiesce diagnostic_capture_quiesce
#define jce_log_quiesce_internal diagnostic_log_quiesce
#ifndef JCE_TEST_DIAGNOSTICS_RETIREMENT_OWNER
#define JCE_TEST_DIAGNOSTICS_RETIREMENT_OWNER "application/jce_engine_diagnostics_retirement.h"
#endif
#include JCE_TEST_DIAGNOSTICS_RETIREMENT_OWNER
#undef jce_trace_capture_quiesce
#undef jce_log_quiesce_internal

#ifndef JCE_TEST_DIAGNOSTICS_PREFIX
#define JCE_TEST_DIAGNOSTICS_PREFIX "diagnostics-retirement"
#endif
static JceTraceCapture *diagnostic_capture;
static JceEngineTraceScope diagnostic_cleanup_scope;
static bool diagnostic_main_registered;
static char diagnostic_log_path[256], diagnostic_trace_path[256];
static SDL_AtomicInt diagnostic_before_count, diagnostic_after_count;
static SDL_AtomicInt diagnostic_vafter_count, diagnostic_resumed_count;
static SDL_AtomicInt diagnostic_after_on_owner;
static SDL_ThreadID diagnostic_owner_tid;

static void diagnostic_sink(const JceLogRecord *record, void *userdata)
{
    (void)userdata;
    if (strcmp(record->message, "before-retirement") == 0)
        SDL_AddAtomicInt(&diagnostic_before_count, 1);
    if (strcmp(record->message, "after-retirement") == 0) {
        SDL_AddAtomicInt(&diagnostic_after_count, 1);
        SDL_SetAtomicInt(&diagnostic_after_on_owner,
                        SDL_GetCurrentThreadID() == diagnostic_owner_tid);
    }
    if (strcmp(record->message, "vafter-retirement") == 0)
        SDL_AddAtomicInt(&diagnostic_vafter_count, 1);
    if (strcmp(record->message, "resumed-retirement") == 0)
        SDL_AddAtomicInt(&diagnostic_resumed_count, 1);
}
static void diagnostic_write(const char *message)
{
    jce_log_write(JCE_LOG_LEVEL_INFO, "retirement", __FILE__, __LINE__, "%s", message);
}
static void diagnostic_write_v(const char *format, ...)
{
    va_list ap;
    va_start(ap, format);
    jce_log_write_v(JCE_LOG_LEVEL_INFO, "retirement", __FILE__, __LINE__, format, ap);
    va_end(ap);
}
static unsigned diagnostic_occurrences(const char *text, const char *needle)
{
    unsigned count = 0;
    while ((text = strstr(text, needle)) != NULL) { ++count; text += strlen(needle); }
    return count;
}
static void diagnostic_expect_file_once(const char *marker)
{
    uint64_t size = 0;
    char *text = jce_fs_host_read_all(diagnostic_log_path, &size);
    TEST_ASSERT_NOT_NULL(text);
    unsigned count = diagnostic_occurrences(text, marker);
    jce_fs_buffer_free(text);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1, count, marker);
}
static void diagnostic_expect_accepted_once(const char *marker)
{
    /* Windows SDL writable streams deny concurrent opens. Inspect actual
     * accepted IO now; independently read the persisted file after final close. */
    TEST_ASSERT_FALSE(diagnostic_accepted_overflow);
    TEST_ASSERT_EQUAL_UINT_MESSAGE(1,
        diagnostic_occurrences(diagnostic_accepted_bytes, marker), marker);
}
static void diagnostic_expect_join_parent(uint64_t parent)
{
    JceTraceCursor cursor;
    JceTraceEvent batch[64];
    uint64_t begin_id = 0, end_id = 0;
    unsigned begins = 0, ends = 0, count;
    jce_trace_cursor_init(&cursor);
    cursor.next_sequence = 1;
    while ((count = jce_trace_read(&cursor, batch, 64)) != 0) {
        for (unsigned i = 0; i < count; ++i) {
            if (strcmp(batch[i].name, "trace.capture.join") != 0) continue;
            TEST_ASSERT_EQUAL_UINT64(parent, batch[i].parent_id);
            if (batch[i].type == JCE_TRACE_EVENT_WAIT_BEGIN) { begin_id = batch[i].id; ++begins; }
            if (batch[i].type == JCE_TRACE_EVENT_WAIT_END) { end_id = batch[i].id; ++ends; }
        }
    }
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    TEST_ASSERT_EQUAL_UINT(1, begins);
    TEST_ASSERT_EQUAL_UINT(1, ends);
    TEST_ASSERT_TRUE(begin_id != 0);
    TEST_ASSERT_EQUAL_UINT64(begin_id, end_id);
}
void setUp(void)
{
    diagnostic_capture = NULL;
    memset(&diagnostic_cleanup_scope, 0, sizeof(diagnostic_cleanup_scope));
    diagnostic_main_registered = false;
    diagnostic_check_order = diagnostic_capture_joined = diagnostic_bad_order = false;
    diagnostic_order_count = diagnostic_log_joins = 0;
    diagnostic_accepted_size = 0;
    diagnostic_accepted_bytes[0] = '\0';
    diagnostic_accepted_overflow = false;
    diagnostic_fail_flush = diagnostic_fail_close = false;
    diagnostic_final_closes = 0;
    SDL_SetAtomicInt(&diagnostic_before_count, 0);
    SDL_SetAtomicInt(&diagnostic_after_count, 0);
    SDL_SetAtomicInt(&diagnostic_vafter_count, 0);
    SDL_SetAtomicInt(&diagnostic_resumed_count, 0);
    SDL_SetAtomicInt(&diagnostic_after_on_owner, 0);
    diagnostic_owner_tid = SDL_GetCurrentThreadID();
    snprintf(diagnostic_log_path, sizeof(diagnostic_log_path), "%s.log", JCE_TEST_DIAGNOSTICS_PREFIX);
    snprintf(diagnostic_trace_path, sizeof(diagnostic_trace_path), "%s.json", JCE_TEST_DIAGNOSTICS_PREFIX);
    SDL_RemovePath(diagnostic_log_path);
    SDL_RemovePath(diagnostic_trace_path);
    jce_trace_set_enabled(true);
    jce_trace_reset();
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);
    jce_log_set_colors(false);
    jce_log_set_file(diagnostic_log_path);
    jce_log_set_sink(diagnostic_sink, NULL);
    TEST_ASSERT_NOT_NULL(g_backend_thread);
    TEST_ASSERT_NOT_NULL(g_log_file);
}
void tearDown(void)
{
    diagnostic_check_order = false;
    if (diagnostic_cleanup_scope.id) {
        engine_trace_end(diagnostic_cleanup_scope, JCE_TRACE_TASK_FAILED);
        diagnostic_cleanup_scope.id = 0;
    }
    if (diagnostic_main_registered) jce_trace_thread_unregister();
    if (diagnostic_capture) {
        JceTraceCaptureResult ignored;
        jce_trace_capture_stop(diagnostic_capture, &ignored);
        diagnostic_capture = NULL;
    }
    jce_trace_set_enabled(false);
    jce_log_shutdown();
    jce_log_set_sink(NULL, NULL);
    SDL_RemovePath(diagnostic_log_path);
    SDL_RemovePath(diagnostic_trace_path);
}

static void test_native_diagnostic_join_order_retains_tail_and_sync_logs(void)
{
    JceTraceCaptureResult result;
    jce_trace_thread_register("retirement-main");
    diagnostic_main_registered = true;
    diagnostic_capture = jce_trace_capture_start_mode(diagnostic_trace_path, 1,
                                                      JCE_TRACE_CAPTURE_THREADED);
    TEST_ASSERT_NOT_NULL(diagnostic_capture);
    diagnostic_write("before-retirement");
    diagnostic_cleanup_scope = engine_trace_begin("retirement.cleanup");
    TEST_ASSERT_TRUE(diagnostic_cleanup_scope.id != 0);
    diagnostic_check_order = true;
    engine_quiesce_diagnostics(diagnostic_capture);
    TEST_ASSERT_FALSE(diagnostic_bad_order);
    TEST_ASSERT_EQUAL_UINT(2, diagnostic_order_count);
    TEST_ASSERT_EQUAL_UINT(1, diagnostic_order[0]);
    TEST_ASSERT_EQUAL_UINT(2, diagnostic_order[1]);
    TEST_ASSERT_EQUAL_UINT(1, diagnostic_log_joins);
    TEST_ASSERT_NULL(g_backend_thread);
    TEST_ASSERT_NOT_NULL(g_ring);
    TEST_ASSERT_NOT_NULL(g_log_file);
    diagnostic_expect_join_parent(diagnostic_cleanup_scope.id);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_before_count));
    /* These complete immediately on the owner, with no second collector. */
    diagnostic_write("after-retirement");
    diagnostic_write_v("%s", "vafter-retirement");
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_after_count));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_vafter_count));
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_after_on_owner));
    jce_log_flush();
    diagnostic_expect_accepted_once("before-retirement");
    diagnostic_expect_accepted_once("retirement: after-retirement at");
    diagnostic_expect_accepted_once("vafter-retirement");
    jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "late-final-retirement", 1);
    engine_trace_end(diagnostic_cleanup_scope, JCE_TRACE_TASK_SUCCEEDED);
    diagnostic_cleanup_scope.id = 0;
    jce_trace_thread_unregister();
    diagnostic_main_registered = false;
    bool complete = jce_trace_capture_stop(diagnostic_capture, &result);
    diagnostic_capture = NULL;
    TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_TRUE(result.worker_available);
    TEST_ASSERT_EQUAL_UINT64(0, result.lost_events);
    TEST_ASSERT_EQUAL_UINT64(0, result.unexported_events);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_threads);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT(0, result.final_stats.active_waits);
    jce_trace_set_enabled(false);
    uint64_t size = 0;
    char *json = jce_fs_host_read_all(diagnostic_trace_path, &size);
    TEST_ASSERT_NOT_NULL(json);
    bool late = strstr(json, "late-final-retirement") != NULL;
    bool scope = strstr(json, "engine.log_quiesce") != NULL;
    bool worker = strstr(json, "\"worker_available\":true") != NULL;
    jce_fs_buffer_free(json);
    TEST_ASSERT_TRUE(late);
    TEST_ASSERT_TRUE(scope);
    TEST_ASSERT_TRUE(worker);
    /* Final close diagnostics still use the retained file and sink. */
    diagnostic_write("resumed-retirement");
    jce_log_flush();
    diagnostic_expect_accepted_once("resumed-retirement");
    jce_log_shutdown();
    TEST_ASSERT_EQUAL_UINT(1, diagnostic_log_joins);
    diagnostic_expect_file_once("before-retirement");
    diagnostic_expect_file_once("retirement: after-retirement at");
    diagnostic_expect_file_once("vafter-retirement");
    diagnostic_expect_file_once("resumed-retirement");
}

static void test_quiescence_is_idempotent_and_restart_reuses_owned_storage(void)
{
    JceLogRing *ring = g_ring;
    SDL_IOStream *file = g_log_file;
    SDL_Mutex *file_mutex = g_file_mtx, *sink_mutex = g_sink_mtx;
    jce_log_quiesce_internal();
    jce_log_quiesce_internal();
    jce_trace_capture_quiesce(NULL);
    TEST_ASSERT_EQUAL_UINT(1, diagnostic_log_joins);
    TEST_ASSERT_NULL(g_backend_thread);
    diagnostic_write("after-retirement");
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_after_count));
    jce_log_init();
    jce_log_init();
    TEST_ASSERT_NOT_NULL(g_backend_thread);
    TEST_ASSERT_EQUAL_PTR(ring, g_ring);
    TEST_ASSERT_EQUAL_PTR(file, g_log_file);
    TEST_ASSERT_EQUAL_PTR(file_mutex, g_file_mtx);
    TEST_ASSERT_EQUAL_PTR(sink_mutex, g_sink_mtx);
    diagnostic_write("resumed-retirement");
    jce_log_shutdown();
    jce_log_shutdown();
    TEST_ASSERT_EQUAL_UINT(2, diagnostic_log_joins);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_resumed_count));
    TEST_ASSERT_NULL(g_ring);
    TEST_ASSERT_NULL(g_log_file);
    diagnostic_expect_file_once("after-retirement");
    diagnostic_expect_file_once("resumed-retirement");
}

static void test_ordinary_shutdown_still_joins_and_drains_once(void)
{
    diagnostic_write("before-retirement");
    jce_log_shutdown();
    TEST_ASSERT_EQUAL_UINT(1, diagnostic_log_joins);
    TEST_ASSERT_EQUAL_INT(1, SDL_GetAtomicInt(&diagnostic_before_count));
    TEST_ASSERT_NULL(g_ring);
    diagnostic_expect_file_once("before-retirement");
    TEST_ASSERT_EQUAL_INT(JCE_LOG_FINAL_CLOSE_API_OK, jce_log_final_close_internal());
}
static void test_final_log_flush_and_close_failures_are_observed_without_replay(void)
{
    for (unsigned mode = 0; mode < 2; ++mode) {
        if (mode) { jce_log_init(); jce_log_set_file(diagnostic_log_path); }
        TEST_ASSERT_EQUAL_INT(JCE_LOG_FINAL_CLOSE_UNOBSERVED, jce_log_final_close_internal());
        jce_log_quiesce_internal();
        diagnostic_fail_flush = mode == 0;
        diagnostic_fail_close = mode == 1;
        unsigned closes = diagnostic_final_closes;
        jce_log_shutdown();
        TEST_ASSERT_EQUAL_INT(JCE_LOG_FINAL_CLOSE_API_FAILED, jce_log_final_close_internal());
        TEST_ASSERT_EQUAL_UINT(closes + 1, diagnostic_final_closes);
        TEST_ASSERT_NULL(g_log_file);
        jce_log_shutdown();
        TEST_ASSERT_EQUAL_UINT(closes + 1, diagnostic_final_closes);
        TEST_ASSERT_EQUAL_INT(JCE_LOG_FINAL_CLOSE_API_FAILED, jce_log_final_close_internal());
        diagnostic_fail_flush = diagnostic_fail_close = false;
    }
    jce_log_init();
    jce_log_shutdown();
    TEST_ASSERT_EQUAL_INT(JCE_LOG_FINAL_CLOSE_NO_FILE, jce_log_final_close_internal());
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_native_diagnostic_join_order_retains_tail_and_sync_logs);
    RUN_TEST(test_quiescence_is_idempotent_and_restart_reuses_owned_storage);
    RUN_TEST(test_ordinary_shutdown_still_joins_and_drains_once);
    RUN_TEST(test_final_log_flush_and_close_failures_are_observed_without_replay);
    return UNITY_END();
}
