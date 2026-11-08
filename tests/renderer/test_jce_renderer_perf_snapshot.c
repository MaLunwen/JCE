/* Actual wrapper + snapshot entry, typed providers and the real Core ring.
 * No graphics device, environment mutation or logger worker is created. */
#include "unity.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "renderer/jce_bgfx_allocator_trace_internal.h"
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_log.h>
#include <bgfx/c99/bgfx.h>
#include <stdarg.h>
#include <stdio.h>
#include <string.h>

static void log_port(JceLogLevel, const char *, const char *, int, const char *, ...);
#define jce_log_write log_port
#include "renderer/jce_renderer_snapshot_internal.h"

struct JceRenderer {
    bool is_fallback, headless;
    JceSubmittedPayloadStats submitted_payload;
};
static JceRenderer renderer;
static struct {
    uint32_t frame_index, offscreen_submission_index;
    JceBgfxAllocatorTrace allocation_trace;
} s_device;
static bgfx_stats_t stats;
static bgfx_caps_t caps;
static bgfx_view_stats_t views[1];
static unsigned stats_queries, caps_queries, environment_reads, allocation_summaries;
static unsigned provenance_logs, names_logs, frame_calls, ring_events;
static bool submission_allowed, stats_available;
static char *perf_value;
static char provenance[512], names[512];

static const bgfx_stats_t *stats_port(void)
{ ++stats_queries; return stats_available ? &stats : NULL; }
static const bgfx_caps_t *caps_port(void)
{ ++caps_queries; return &caps; }
static char *environment_port(const char *name)
{
    ++environment_reads;
    if (strcmp(name, "JCE_PERF_LOG") != 0) return NULL;
    return perf_value;
}
static void allocation_port(JceBgfxAllocatorTrace *state, uint32_t reason)
{
    TEST_ASSERT_EQUAL_PTR(&s_device.allocation_trace, state);
    TEST_ASSERT_EQUAL_UINT32(JCE_BGFX_ALLOC_SUMMARY_SNAPSHOT, reason);
    ++allocation_summaries;
}
static void log_port(JceLogLevel level, const char *tag, const char *file, int line,
                     const char *format, ...)
{
    (void)tag; (void)file; (void)line;
    TEST_ASSERT_EQUAL_INT(JCE_LOG_LEVEL_INFO, level);
    va_list args; va_start(args, format);
    if (strncmp(format, "gpu-provenance:", 15) == 0) {
        ++provenance_logs; vsnprintf(provenance, sizeof provenance, format, args);
    } else if (strncmp(format, "gpu-view-names:", 15) == 0) {
        ++names_logs; vsnprintf(names, sizeof names, format, args);
    } else {
        va_end(args); TEST_FAIL_MESSAGE("Unexpected snapshot log"); return;
    }
    va_end(args);
}
bool jce_renderer_end_frame_impl(const JceRenderer *r)
{ ++frame_calls; return r && submission_allowed; }

#define bgfx_get_stats stats_port
#define bgfx_get_caps caps_port
#define getenv environment_port
#define jce_bgfx_trace_summary allocation_port
#define LOG_TAG "jce_renderer"
#include JCE_TEST_PERF_SNAPSHOT_OWNER
#include JCE_TEST_PERF_END_FRAME_OWNER
#undef LOG_TAG
#undef jce_bgfx_trace_summary
#undef getenv
#undef bgfx_get_caps
#undef bgfx_get_stats
#undef jce_log_write

void setUp(void)
{
    jce_trace_set_enabled(false); jce_trace_task_restore_id(0); jce_trace_reset();
    memset(&renderer, 0, sizeof renderer); memset(&s_device, 0, sizeof s_device);
    memset(&stats, 0, sizeof stats); memset(&caps, 0, sizeof caps);
    memset(views, 0, sizeof views);
    stats_queries = caps_queries = environment_reads = allocation_summaries = 0;
    provenance_logs = names_logs = frame_calls = ring_events = 0;
    provenance[0] = names[0] = '\0'; perf_value = NULL;
    submission_allowed = stats_available = true;
    s_device.frame_index = 60; s_device.offscreen_submission_index = 60;
    renderer.submitted_payload.valid = true;
    renderer.submitted_payload.context_generation = 7;
    stats.numDraw = 7; stats.gpuTimerFreq = 1000000;
    stats.gpuTimeBegin = 100; stats.gpuTimeEnd = 150; stats.gpuFrameNum = 42;
    stats.numViews = 1; stats.viewStats = views;
    views[0].view = 162; views[0].gpuFrameNum = 41;
    views[0].gpuTimeBegin = 101; views[0].gpuTimeEnd = 149;
    strcpy(views[0].name, "PostFX/FXAA");
    caps.vendorId = 0x10de; caps.deviceId = 0x28e0;
    caps.rendererType = BGFX_RENDERER_TYPE_DIRECT3D11; caps.limits.maxEncoders = 8;
}
void tearDown(void)
{ jce_trace_set_enabled(false); jce_trace_task_restore_id(0); }

static unsigned counter_count(const char *name)
{
    JceTraceCursor cursor; JceTraceEvent events[128];
    jce_trace_cursor_init(&cursor);
    ring_events = jce_trace_read(&cursor, events, 128);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    unsigned count = 0;
    for (unsigned i = 0; i < ring_events; ++i)
        if (events[i].type == JCE_TRACE_EVENT_COUNTER && strcmp(events[i].name, name) == 0)
            ++count;
    return count;
}
static void test_both_disabled_skip_statistics_and_logs(void)
{
    jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, frame_calls); TEST_ASSERT_EQUAL_UINT(1, environment_reads);
    TEST_ASSERT_EQUAL_UINT(0, stats_queries); TEST_ASSERT_EQUAL_UINT(0, caps_queries);
    TEST_ASSERT_EQUAL_UINT(0, provenance_logs); TEST_ASSERT_EQUAL_UINT(0, names_logs);
    TEST_ASSERT_EQUAL_UINT(0, allocation_summaries);
    TEST_ASSERT_EQUAL_UINT(0, counter_count("renderer.draw_count"));
    TEST_ASSERT_EQUAL_UINT(0, ring_events);
}
static void test_perf_only_has_bounded_logs_without_ring(void)
{
    perf_value = "1"; jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, stats_queries); TEST_ASSERT_EQUAL_UINT(1, caps_queries);
    TEST_ASSERT_EQUAL_UINT(1, environment_reads); TEST_ASSERT_EQUAL_UINT(0, allocation_summaries);
    TEST_ASSERT_EQUAL_UINT(1, provenance_logs); TEST_ASSERT_EQUAL_UINT(1, names_logs);
    TEST_ASSERT_NOT_NULL(strstr(provenance, "source_frame=42"));
    TEST_ASSERT_NOT_NULL(strstr(names, "162@41=PostFX/FXAA"));
    TEST_ASSERT_EQUAL_UINT(0, counter_count("renderer.draw_count"));
    TEST_ASSERT_EQUAL_UINT(0, ring_events);
}
static void test_trace_only_has_canonical_ring_without_logs(void)
{
    jce_trace_set_enabled(true); jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, stats_queries); TEST_ASSERT_EQUAL_UINT(1, caps_queries);
    TEST_ASSERT_EQUAL_UINT(1, allocation_summaries);
    TEST_ASSERT_EQUAL_UINT(0, provenance_logs); TEST_ASSERT_EQUAL_UINT(0, names_logs);
    TEST_ASSERT_EQUAL_UINT(1, counter_count("renderer.draw_count"));
    TEST_ASSERT_EQUAL_UINT(1, counter_count("gpu.view.162.source_frame"));
}
static void test_both_enabled_share_one_snapshot(void)
{
    perf_value = "1"; jce_trace_set_enabled(true); jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, stats_queries); TEST_ASSERT_EQUAL_UINT(1, caps_queries);
    TEST_ASSERT_EQUAL_UINT(1, environment_reads); TEST_ASSERT_EQUAL_UINT(1, allocation_summaries);
    TEST_ASSERT_EQUAL_UINT(1, provenance_logs); TEST_ASSERT_EQUAL_UINT(1, names_logs);
    TEST_ASSERT_EQUAL_UINT(1, counter_count("renderer.draw_count"));
    TEST_ASSERT_EQUAL_UINT(1, counter_count("gpu.view.162.source_frame"));
}
static void test_first_submission_and_existing_cadence_precede_environment(void)
{
    perf_value = "1"; s_device.frame_index = 17; s_device.offscreen_submission_index = 2;
    jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(0, environment_reads); TEST_ASSERT_EQUAL_UINT(0, stats_queries);
    s_device.offscreen_submission_index = 1; jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, environment_reads); TEST_ASSERT_EQUAL_UINT(1, stats_queries);
}
static void test_refused_and_null_submission_never_sample(void)
{
    perf_value = "1"; jce_trace_set_enabled(true); submission_allowed = false;
    jce_renderer_end_frame(&renderer); jce_renderer_end_frame(NULL);
    TEST_ASSERT_EQUAL_UINT(2, frame_calls); TEST_ASSERT_EQUAL_UINT(0, environment_reads);
    TEST_ASSERT_EQUAL_UINT(0, stats_queries); TEST_ASSERT_EQUAL_UINT(0, caps_queries);
    TEST_ASSERT_EQUAL_UINT(0, counter_count("renderer.draw_count"));
}
static void test_fallback_and_headless_never_query(void)
{
    perf_value = "1"; jce_trace_set_enabled(true); renderer.is_fallback = true;
    jce_renderer_end_frame(&renderer); renderer.is_fallback = false; renderer.headless = true;
    jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(2, frame_calls); TEST_ASSERT_EQUAL_UINT(0, environment_reads);
    TEST_ASSERT_EQUAL_UINT(0, stats_queries); TEST_ASSERT_EQUAL_UINT(0, caps_queries);
    TEST_ASSERT_EQUAL_UINT(0, counter_count("renderer.draw_count"));
}
static void test_perf_flag_preserves_existing_nonempty_nonzero_semantics(void)
{
    perf_value = "0disabled"; jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(0, stats_queries);
    perf_value = "false"; jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, stats_queries); TEST_ASSERT_EQUAL_UINT(1, provenance_logs);
    TEST_ASSERT_EQUAL_UINT(0, counter_count("renderer.draw_count"));
    TEST_ASSERT_EQUAL_UINT(0, ring_events);
}
static void test_unavailable_provider_does_not_invent_logs_or_counters(void)
{
    perf_value = "1"; stats_available = false; jce_renderer_end_frame(&renderer);
    TEST_ASSERT_EQUAL_UINT(1, stats_queries); TEST_ASSERT_EQUAL_UINT(0, caps_queries);
    TEST_ASSERT_EQUAL_UINT(0, provenance_logs); TEST_ASSERT_EQUAL_UINT(0, names_logs);
    TEST_ASSERT_EQUAL_UINT(0, counter_count("renderer.draw_count"));
    TEST_ASSERT_EQUAL_UINT(0, ring_events);
}
int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_both_disabled_skip_statistics_and_logs);
    RUN_TEST(test_perf_only_has_bounded_logs_without_ring);
    RUN_TEST(test_trace_only_has_canonical_ring_without_logs);
    RUN_TEST(test_both_enabled_share_one_snapshot);
    RUN_TEST(test_first_submission_and_existing_cadence_precede_environment);
    RUN_TEST(test_refused_and_null_submission_never_sample);
    RUN_TEST(test_fallback_and_headless_never_query);
    RUN_TEST(test_perf_flag_preserves_existing_nonempty_nonzero_semantics);
    RUN_TEST(test_unavailable_provider_does_not_invent_logs_or_counters);
    return UNITY_END();
}
