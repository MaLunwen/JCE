/* Actual royalty-free decoder/worker lifecycle through the public facade.
 * No engine/window, audio device or GPU. Private seek token controls supplement
 * actual owner wiring; they never stand for decoder/presentation acceptance. */
#include "unity.h"
#include "jce_video_lifecycle_trace.h"
#include <jce/middleware/video/jce_video.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_path.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <stdint.h>
#include <string.h>

#ifndef JCE_TEST_VIDEO_TRACE_DIR
#error "Committed procedural video fixtures must be supplied"
#endif
static JceVideo g_video;
static JceTraceCursor g_cursor;
static JceTraceEvent g_events[16384];
static uint32_t g_count;
static JceVideoLifecycleSeek g_token;
static JceTraceScope g_token_owner;

void setUp(void)
{
    g_video = JCE_VIDEO_INVALID; memset(&g_token, 0, sizeof g_token);
    memset(&g_token_owner, 0, sizeof g_token_owner);
    jce_trace_set_enabled(false); jce_trace_reset();
    jce_trace_cursor_init(&g_cursor); g_count = 0;
}
void tearDown(void)
{
    if (g_video) jce_video_unload(g_video);
    jce_video_lifecycle_seek_finish(&g_token, "video_seek_cancelled", JCE_TRACE_TASK_CANCELLED);
    if (g_token_owner.id) jce_trace_scope_end(g_token_owner, JCE_TRACE_TASK_CANCELLED);
    memset(&g_token_owner, 0, sizeof g_token_owner);
    jce_trace_set_enabled(false);
}
static void close_video(void)
{
    jce_video_unload(g_video); g_video = JCE_VIDEO_INVALID;
}
static void load_video(const char *leaf)
{
    char file[1024];
    TEST_ASSERT_TRUE(jce_path_join(file, sizeof file, JCE_TEST_VIDEO_TRACE_DIR, leaf));
    TEST_ASSERT_TRUE_MESSAGE(jce_fs_host_exists_file(file), "Required original procedural fixture missing");
    g_video = jce_video_load_file(file);
    TEST_ASSERT_NOT_EQUAL(JCE_VIDEO_INVALID, g_video);
}
static void collect(void)
{
    g_count = jce_trace_read(&g_cursor, g_events, 16384);
    TEST_ASSERT_EQUAL_UINT64(0, g_cursor.lost_events);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_TRUE(stats.recorded_events <= stats.capacity);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_waits);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_threads);
    TEST_ASSERT_EQUAL_UINT64(stats.work_items_started, stats.work_items_completed);
    TEST_ASSERT_EQUAL_UINT64(0, jce_trace_task_current_id());
}
static uint32_t count_event(JceTraceEventType type, const char *name)
{
    uint32_t n = 0;
    for (uint32_t i = 0; i < g_count; ++i)
        if (g_events[i].type == type && strcmp(g_events[i].name, name) == 0) ++n;
    return n;
}
static const JceTraceEvent *find_event(JceTraceEventType type, const char *name)
{
    for (uint32_t i = 0; i < g_count; ++i)
        if (g_events[i].type == type && strcmp(g_events[i].name, name) == 0) return &g_events[i];
    return NULL;
}
static void expect_seek_terminals(uint32_t submitted)
{
    TEST_ASSERT_EQUAL_UINT32(submitted, count_event(JCE_TRACE_EVENT_TASK_SUBMIT, "video_seek"));
    for (uint32_t i = 0; i < g_count; ++i) {
        const JceTraceEvent *submit = &g_events[i];
        if (submit->type != JCE_TRACE_EVENT_TASK_SUBMIT || strcmp(submit->name, "video_seek") != 0) continue;
        uint32_t starts = 0, ends = 0, terminals = 0;
        for (uint32_t k = 0; k < g_count; ++k) {
            const JceTraceEvent *event = &g_events[k];
            if (event->parent_id == submit->id && strcmp(event->name, "video_seek") == 0) {
                if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) ++starts;
                if (event->type == JCE_TRACE_EVENT_TASK_END) ++ends;
            }
            if (event->type == JCE_TRACE_EVENT_COUNTER && event->id == submit->id
                && (strcmp(event->name, "video_seek_queue_ready") == 0
                    || strcmp(event->name, "video_seek_superseded") == 0
                    || strcmp(event->name, "video_seek_cancelled") == 0
                    || strcmp(event->name, "video_seek_failed") == 0
                    || strcmp(event->name, "video_seek_eof") == 0)) ++terminals;
        }
        TEST_ASSERT_EQUAL_UINT32(1, starts); TEST_ASSERT_EQUAL_UINT32(1, ends);
        TEST_ASSERT_EQUAL_UINT32(1, terminals);
    }
}
static void expect_join_before_release(void)
{
    const JceTraceEvent *join = find_event(JCE_TRACE_EVENT_TASK_END, "video_worker_join");
    const JceTraceEvent *worker = find_event(JCE_TRACE_EVENT_TASK_END, "video_worker");
    const JceTraceEvent *decoder = find_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_decoder_close");
    const JceTraceEvent *source = find_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_source_close");
    TEST_ASSERT_NOT_NULL(join); TEST_ASSERT_NOT_NULL(worker);
    TEST_ASSERT_NOT_NULL(decoder); TEST_ASSERT_NOT_NULL(source);
    TEST_ASSERT_TRUE(worker->sequence < join->sequence);
    TEST_ASSERT_TRUE(join->sequence < decoder->sequence);
    TEST_ASSERT_TRUE(decoder->sequence < source->sequence);
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_THREAD_REGISTER, "jce_video_worker"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_THREAD_UNREGISTER, "thread-exit"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_worker"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_TASK_END, "video_worker"));
}
static void await_real_queue_wait(void)
{
    const uint64_t start = jce_time_ticks_ms();
    JceTraceStats stats;
    do {
        jce_trace_get_stats(&stats);
        if (stats.active_waits) return;
        jce_thread_sleep_ms(1);
    } while (jce_time_ticks_ms() - start < 3000u);
    TEST_FAIL_MESSAGE("Real decode worker never entered full-queue/EOF condition wait");
}
static void await_current_picture(void)
{
    const uint64_t start = jce_time_ticks_ms();
    do {
        jce_video_advance(g_video, 0.0);
        if (jce_video_is_ready_to_play(g_video)) return;
        jce_thread_sleep_ms(1);
    } while (jce_time_ticks_ms() - start < 3000u);
    TEST_FAIL_MESSAGE("Current-generation CPU picture did not become ready");
}
static void test_invalid_open_reports_failure_and_restores_parent(void)
{
    const uint8_t not_video[32] = {0};
    jce_trace_set_enabled(true);
    TEST_ASSERT_EQUAL_UINT32(JCE_VIDEO_INVALID,
        jce_video_load_memory(not_video, sizeof not_video, "not-a-path"));
    collect();
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_open"));
    TEST_ASSERT_EQUAL_UINT32(0, count_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_worker"));
    const JceTraceEvent *end = find_event(JCE_TRACE_EVENT_TASK_END, "video_open");
    TEST_ASSERT_NOT_NULL(end); TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, end->value_u32[0]);
}
static void test_disabled_actual_decode_seek_close_emits_nothing(void)
{
    load_video("odd-av1.mp4");
    jce_video_seek(g_video, 0.5, true);
    await_current_picture(); close_video(); collect();
    TEST_ASSERT_EQUAL_UINT32(0, g_count);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_FALSE(stats.enabled); TEST_ASSERT_EQUAL_UINT64(0, stats.recorded_events);
    TEST_ASSERT_EQUAL_UINT64(1, jce_trace_next_id()); /* no disabled producer allocated IDs */
}
static void test_actual_av1_worker_seek_queue_wait_join_and_release(void)
{
    jce_trace_set_enabled(true); load_video("odd-av1.mp4");
    JceVideoInfo info; TEST_ASSERT_TRUE(jce_video_get_info(g_video, &info));
    TEST_ASSERT_FALSE(info.metadata_only);
    await_real_queue_wait();
    jce_video_seek(g_video, 0.25, true);
    jce_video_seek(g_video, 0.75, true);
    jce_video_seek(g_video, 1.0, true);
    await_current_picture(); close_video(); collect();
    expect_seek_terminals(3); expect_join_before_release();
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_COUNTER, "video_open_decoder"));
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_decode") > 0);
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_COUNTER, "video_decode_picture") > 0);
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_COUNTER, "video_queue_push") > 0);
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_COUNTER, "video_queue_pop") > 0);
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_WAIT_BEGIN, "video_queue_wait") > 0);
    TEST_ASSERT_EQUAL_UINT32(count_event(JCE_TRACE_EVENT_WAIT_BEGIN, "video_queue_wait"),
        count_event(JCE_TRACE_EVENT_WAIT_END, "video_queue_wait"));
    uint32_t ready_final = 0;
    for (uint32_t i = 0; i < g_count; ++i)
        if (g_events[i].type == JCE_TRACE_EVENT_COUNTER
            && strcmp(g_events[i].name, "video_seek_queue_ready") == 0
            && g_events[i].value_u64 == 4u) ++ready_final;
    TEST_ASSERT_EQUAL_UINT32(1, ready_final);
}
static void test_actual_vp9_open_worker_cpu_decode_and_close(void)
{
    jce_trace_set_enabled(true); load_video("odd-vp9.webm");
    JceVideoInfo info; TEST_ASSERT_TRUE(jce_video_get_info(g_video, &info));
    TEST_ASSERT_FALSE(info.metadata_only);
    jce_video_seek(g_video, 1.0, true); await_current_picture();
    close_video(); collect(); expect_seek_terminals(1); expect_join_before_release();
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_COUNTER, "video_open_decoder"));
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_TASK_BEGIN, "video_decoder_open") > 0);
    TEST_ASSERT_TRUE(count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_queue_ready") > 0);
}
static void test_exact_pending_active_and_ready_token_terminals(void)
{
    jce_trace_set_enabled(true);
    g_token_owner = jce_trace_scope_begin("video_fixture_owner");
    g_token = jce_video_lifecycle_seek_submit(g_token_owner.id, 2);
    jce_video_lifecycle_seek_finish(&g_token, "video_seek_superseded", JCE_TRACE_TASK_CANCELLED);
    TEST_ASSERT_EQUAL_UINT64(g_token_owner.id, jce_trace_task_current_id());
    g_token = jce_video_lifecycle_seek_submit(g_token_owner.id, 3);
    jce_video_lifecycle_seek_pickup(&g_token);
    jce_video_lifecycle_seek_finish(&g_token, "video_seek_cancelled", JCE_TRACE_TASK_CANCELLED);
    TEST_ASSERT_EQUAL_UINT64(g_token_owner.id, jce_trace_task_current_id());
    g_token = jce_video_lifecycle_seek_submit(g_token_owner.id, 4);
    jce_video_lifecycle_seek_pickup(&g_token);
    jce_video_lifecycle_seek_finish(&g_token, "video_seek_queue_ready", JCE_TRACE_TASK_SUCCEEDED);
    TEST_ASSERT_EQUAL_UINT64(g_token_owner.id, jce_trace_task_current_id());
    jce_trace_scope_end(g_token_owner, JCE_TRACE_TASK_SUCCEEDED);
    memset(&g_token_owner, 0, sizeof g_token_owner);
    collect(); expect_seek_terminals(3);
    TEST_ASSERT_EQUAL_UINT32(2, count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_pickup"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_cancel_before_pickup"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_superseded"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_cancelled"));
    TEST_ASSERT_EQUAL_UINT32(1, count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_queue_ready"));
}
static void test_actual_close_with_queued_seek_balances_all_owned_work(void)
{
    jce_trace_set_enabled(true); load_video("odd-av1.mp4");
    jce_video_seek(g_video, 1.5, true);
    close_video(); collect(); expect_seek_terminals(1); expect_join_before_release();
    TEST_ASSERT_EQUAL_UINT32(0, count_event(JCE_TRACE_EVENT_COUNTER, "video_seek_failed"));
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_invalid_open_reports_failure_and_restores_parent);
    RUN_TEST(test_disabled_actual_decode_seek_close_emits_nothing);
    RUN_TEST(test_actual_av1_worker_seek_queue_wait_join_and_release);
    RUN_TEST(test_actual_vp9_open_worker_cpu_decode_and_close);
    RUN_TEST(test_exact_pending_active_and_ready_token_terminals);
    RUN_TEST(test_actual_close_with_queued_seek_balances_all_owned_work);
    return UNITY_END();
}
