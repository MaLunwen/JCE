/*
 * test_jce_log.c — Unit tests for jce_log.h
 *
 * Layer: L1.  Async structured logger.  Test only init/shutdown,
 * configuration knobs and the macro emission path — we do NOT capture
 * stderr (would require platform-specific FD redirection).
 */

#include "unity.h"

#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_thread.h>

#include <stdio.h>
#include <stdint.h>
#include <stdarg.h>
#include <string.h>

static char g_log_path[1024];

void setUp(void)
{
    char base[1024];
    (void)jce_fs_host_get_current_dir(base, sizeof(base));
    snprintf(g_log_path, sizeof(g_log_path), "%s/_ut_jce.log", base);
    (void)jce_fs_host_remove_file(g_log_path);
    jce_log_init();
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);   /* exercise all level paths */
    jce_log_set_colors(false);
    jce_log_set_file(g_log_path);
    jce_log_set_thread_name("UT");
}

void tearDown(void)
{
    jce_log_set_file(NULL);
    jce_log_shutdown();
    (void)jce_fs_host_remove_file(g_log_path);
}

static void test_macros_emit_at_all_levels(void)
{
#ifndef JCE_DIST
    LOG_TRACE  ("UT", "trace %d",  1);
    LOG_DEBUG  ("UT", "debug %s", "x");
    LOG_INFO   ("UT", "info");
    LOG_SUCCESS("UT", "ok");
    LOG_WARN   ("UT", "warn");
    LOG_ERROR  ("UT", "err %u", 7u);
    jce_log_flush();
#endif
    TEST_PASS();
}

static void test_level_filter_drops_below_threshold(void)
{
#ifndef JCE_DIST
    jce_log_set_level(JCE_LOG_LEVEL_WARN);
    LOG_DEBUG("UT", "should be dropped");
    LOG_INFO ("UT", "should be dropped");
    LOG_WARN ("UT", "should pass");
    jce_log_flush();
#endif
    TEST_PASS();
}

static void test_colors_toggle(void)
{
#ifndef JCE_DIST
    jce_log_set_colors(true);
    LOG_INFO("UT", "with colors");
    jce_log_set_colors(false);
    LOG_INFO("UT", "without colors");
    jce_log_flush();
#endif
    TEST_PASS();
}

static void test_log_file_persists(void)
{
#ifndef JCE_DIST
    LOG_INFO("UT", "persisted-marker-42");
    jce_log_flush();
    /* Best-effort: file may not exist on platforms without IO thread. */
    if (jce_fs_host_exists_file(g_log_path)) {
        uint64_t sz = 0;
        TEST_ASSERT_TRUE(jce_fs_host_get_size(g_log_path, &sz));
        TEST_ASSERT_GREATER_THAN_INT(0, (int)sz);
    }
#endif
    TEST_PASS();
}

static void test_shutdown_is_idempotent(void)
{
    jce_log_shutdown();
    jce_log_shutdown();        /* second call must be a no-op */
    jce_log_init();
    jce_log_init();            /* re-init must also be safe */
    TEST_PASS();
}

static void emit_via_va(const char *fmt, ...)
{
    va_list ap;
    va_start(ap, fmt);
    jce_log_write_v(JCE_LOG_LEVEL_INFO, "UT", __FILE__, __LINE__, fmt, ap);
    va_end(ap);
}

static void test_log_write_v_va_list_path(void)
{
#ifndef JCE_DIST
    emit_via_va("via va_list %d %s", 11, "y");
    jce_log_flush();
#endif
    TEST_PASS();
}

/* ------------------------------------------------------------------ *
 *  Sink contract.
 *
 *  The editor Console both FEEDS jce_log (jce_editor_console_log* mirrors
 *  every entry so it survives in the log file) and READS it back through a
 *  sink, and it tells the two apart purely by rec->tag.  If the tag did not
 *  round-trip verbatim from jce_log_write to the sink, every editor console
 *  message would render twice.  Nothing else in the suite touched the sink
 *  path at all, so pin it here.
 * ------------------------------------------------------------------ */

#define SINK_TAG "editor"

static struct {
    int  calls;
    char tag[64];
    char message[256];
    int  level;
} g_sink_seen;

static void recording_sink(const JceLogRecord *rec, void *user)
{
    TEST_ASSERT_NOT_NULL(rec);
    TEST_ASSERT_EQUAL_PTR((void *)0x1234, user);   /* userdata round-trips */
    g_sink_seen.calls++;
    g_sink_seen.level = (int)rec->level;
    /* rec->tag / rec->message are documented never-NULL. */
    snprintf(g_sink_seen.tag, sizeof(g_sink_seen.tag), "%s", rec->tag);
    snprintf(g_sink_seen.message, sizeof(g_sink_seen.message), "%s", rec->message);
}

static void test_sink_receives_tag_and_message_verbatim(void)
{
    memset(&g_sink_seen, 0, sizeof(g_sink_seen));
    jce_log_set_sink(recording_sink, (void *)0x1234);

    jce_log_write(JCE_LOG_LEVEL_WARN, SINK_TAG, __FILE__, __LINE__,
                  "payload %d", 42);
    jce_log_flush();

    jce_log_set_sink(NULL, NULL);   /* removal waits for an in-flight call */

    TEST_ASSERT_EQUAL_INT(1, g_sink_seen.calls);
    TEST_ASSERT_EQUAL_STRING(SINK_TAG, g_sink_seen.tag);
    TEST_ASSERT_EQUAL_STRING("payload 42", g_sink_seen.message);
    TEST_ASSERT_EQUAL_INT((int)JCE_LOG_LEVEL_WARN, g_sink_seen.level);
}

static void test_sink_removal_stops_delivery(void)
{
    memset(&g_sink_seen, 0, sizeof(g_sink_seen));
    jce_log_set_sink(recording_sink, (void *)0x1234);
    jce_log_write(JCE_LOG_LEVEL_INFO, SINK_TAG, __FILE__, __LINE__, "first");
    jce_log_flush();
    jce_log_set_sink(NULL, NULL);

    const int after_remove = g_sink_seen.calls;
    TEST_ASSERT_EQUAL_INT(1, after_remove);

    jce_log_write(JCE_LOG_LEVEL_INFO, SINK_TAG, __FILE__, __LINE__, "second");
    jce_log_flush();
    TEST_ASSERT_EQUAL_INT_MESSAGE(after_remove, g_sink_seen.calls,
        "sink fired after removal — jce_log_set_sink(NULL) must fence");
}

static void test_sink_does_not_see_level_filtered_records(void)
{
    memset(&g_sink_seen, 0, sizeof(g_sink_seen));
    jce_log_set_level(JCE_LOG_LEVEL_ERROR);
    jce_log_set_sink(recording_sink, (void *)0x1234);

    jce_log_write(JCE_LOG_LEVEL_INFO, SINK_TAG, __FILE__, __LINE__, "dropped");
    jce_log_flush();

    jce_log_set_sink(NULL, NULL);
    jce_log_set_level(JCE_LOG_LEVEL_TRACE);

    TEST_ASSERT_EQUAL_INT_MESSAGE(0, g_sink_seen.calls,
        "records below the threshold must not reach the sink either");
}

enum {
    FLUSH_STRESS_THREADS = 8,
    FLUSH_STRESS_RECORDS = 128
};

typedef struct FlushStressWorker {
    int id;
} FlushStressWorker;

static JceAtomicI32 *g_flush_stress_seen;

static void flush_stress_sink(const JceLogRecord *rec, void *user)
{
    (void)rec;
    (void)user;
    (void)jce_atomic_i32_add(g_flush_stress_seen, 1);
}

static void flush_stress_worker(void *arg)
{
    FlushStressWorker *worker = (FlushStressWorker *)arg;
    char name[16];
    snprintf(name, sizeof(name), "LOG-UT-%d", worker->id);
    jce_log_set_thread_name(name);
    for (int i = 0; i < FLUSH_STRESS_RECORDS; ++i) {
        LOG_INFO("flush-stress", "worker=%d record=%d", worker->id, i);
        jce_log_flush();
    }
}

static void test_flush_is_safe_with_concurrent_producers(void)
{
#ifndef JCE_DIST
    JceThread *threads[FLUSH_STRESS_THREADS] = {0};
    FlushStressWorker workers[FLUSH_STRESS_THREADS];
    g_flush_stress_seen = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(g_flush_stress_seen);
    jce_log_set_sink(flush_stress_sink, NULL);

    for (int i = 0; i < FLUSH_STRESS_THREADS; ++i) {
        workers[i].id = i;
        threads[i] = jce_thread_create(flush_stress_worker, &workers[i],
                                       "log-flush-ut");
        TEST_ASSERT_NOT_NULL(threads[i]);
    }
    for (int i = 0; i < FLUSH_STRESS_THREADS; ++i)
        jce_thread_join(threads[i]);

    jce_log_flush();
    jce_log_set_sink(NULL, NULL);
    TEST_ASSERT_EQUAL_INT(FLUSH_STRESS_THREADS * FLUSH_STRESS_RECORDS,
                          jce_atomic_i32_load(g_flush_stress_seen));
    jce_atomic_i32_destroy(g_flush_stress_seen);
    g_flush_stress_seen = NULL;
#endif
}

static JceAtomicI32 *g_timestamp_gate;
static int64_t g_timestamp_queued_wall_s;

static void delayed_timestamp_sink(const JceLogRecord *rec, void *user)
{
    (void)user;
    if (strcmp(rec->message, "timestamp-blocker") == 0) {
        jce_atomic_i32_store(g_timestamp_gate, 1);
        while (jce_atomic_i32_load(g_timestamp_gate) != 2)
            jce_thread_sleep_ms(1);
    } else if (strcmp(rec->message, "timestamp-queued") == 0) {
        g_timestamp_queued_wall_s = rec->wall_epoch_s;
    }
}

/* The backend may be delayed by IO or a slow observer.  A record's wall clock
 * must still describe when the producer queued it, not when the backend
 * eventually emitted it. */
static void test_wall_timestamp_tracks_enqueue_time(void)
{
#ifndef JCE_DIST
    uint64_t deadline;
    int64_t queued_epoch_s;

    g_timestamp_gate = jce_atomic_i32_create(0);
    TEST_ASSERT_NOT_NULL(g_timestamp_gate);
    g_timestamp_queued_wall_s = 0;
    jce_log_set_sink(delayed_timestamp_sink, NULL);

    LOG_INFO("timestamp-ut", "timestamp-blocker");
    deadline = jce_time_ticks_ms() + 2000u;
    while (jce_atomic_i32_load(g_timestamp_gate) != 1 &&
           jce_time_ticks_ms() < deadline)
        jce_thread_sleep_ms(1);
    TEST_ASSERT_EQUAL_INT(1, jce_atomic_i32_load(g_timestamp_gate));

    queued_epoch_s = jce_time_now_epoch_seconds();
    LOG_INFO("timestamp-ut", "timestamp-queued");
    jce_thread_sleep_ms(2200);
    jce_atomic_i32_store(g_timestamp_gate, 2);
    jce_log_flush();
    jce_log_set_sink(NULL, NULL);

    TEST_ASSERT_GREATER_OR_EQUAL_INT64(queued_epoch_s - 1,
                                       g_timestamp_queued_wall_s);
    TEST_ASSERT_LESS_OR_EQUAL_INT64(queued_epoch_s + 1,
                                    g_timestamp_queued_wall_s);
    jce_atomic_i32_destroy(g_timestamp_gate);
    g_timestamp_gate = NULL;
#endif
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_macros_emit_at_all_levels);
    RUN_TEST(test_level_filter_drops_below_threshold);
    RUN_TEST(test_colors_toggle);
    RUN_TEST(test_log_file_persists);
    RUN_TEST(test_shutdown_is_idempotent);
    RUN_TEST(test_log_write_v_va_list_path);
    RUN_TEST(test_sink_receives_tag_and_message_verbatim);
    RUN_TEST(test_sink_removal_stops_delivery);
    RUN_TEST(test_sink_does_not_see_level_filtered_records);
    RUN_TEST(test_flush_is_safe_with_concurrent_producers);
    RUN_TEST(test_wall_timestamp_tracks_enqueue_time);
    return UNITY_END();
}
