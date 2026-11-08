/* Compile the production adapter with exact lifecycle seams, no GPU. */
#include "unity.h"
#include "renderer/jce_renderer_lifecycle_internal.h"
#include "renderer/jce_renderer_caps_internal.h"
#include "os/core/jce_trace_scope_internal.h"
#include "renderer/jce_renderer_submission_trace_internal.h"
#include "renderer/jce_instance_upload.h"
#include "renderer/jce_render_readback_internal.h"
#include "renderer/jce_offscreen_retirement_internal.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_thread.h>
#include <string.h>

/* The original lifecycle adapter controls below retain their counted impl
 * ports. These additional ports consume the complete actual central-submit
 * body, shared with the readback fixture; no renderer/device loop is copied. */
struct JceRenderer {
    bool is_fallback, headless;
    JceInstanceUploadOwner instance_upload;
    JceSubmittedPayloadStats submitted_payload;
};
static JceRenderer central_renderer;
static struct { uint32_t frame_index, offscreen_submission_index; } s_device;
static unsigned central_frames, central_context_queries, central_collections;
static unsigned central_retirements, central_uploads;
static bool central_current, central_no_progress;
static unsigned central_clock_calls, central_clock_frames[2], central_clock_collections[2];
static uint64_t central_clock_ticks[2];
static bool central_reverse_clock;
bool jce_renderer_caps_api_web_context_current(void)
{ ++central_context_queries; return central_current; }
static uint64_t central_perf_counter(void)
{
    const unsigned edge = central_clock_calls++ % 2u;
    central_clock_frames[edge] = central_frames;
    central_clock_collections[edge] = central_collections;
    central_clock_ticks[edge] = central_reverse_clock ? 2u - edge : jce_time_perf_counter();
    return central_clock_ticks[edge];
}
static uint32_t central_frame(bool capture)
{
    TEST_ASSERT_FALSE(capture);
    ++central_frames;
    return s_device.frame_index + (central_no_progress ? 0u : 1u);
}
static void central_readback(const JceRenderer *r, uint32_t serial)
{
    TEST_ASSERT_EQUAL_PTR(&central_renderer, r);
    TEST_ASSERT_EQUAL_UINT32(s_device.frame_index, serial);
    ++central_collections;
}
static void central_retirement(const JceRenderer *r, uint32_t ordinal)
{
    TEST_ASSERT_EQUAL_PTR(&central_renderer, r);
    TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index, ordinal);
    ++central_retirements;
}
static void central_upload(JceInstanceUploadOwner *owner, uint32_t ordinal)
{
    TEST_ASSERT_EQUAL_PTR(&central_renderer.instance_upload, owner);
    TEST_ASSERT_EQUAL_UINT32(s_device.offscreen_submission_index, ordinal);
    ++central_uploads;
}
#define bgfx_frame central_frame
#define jce_time_perf_counter central_perf_counter
#define jce_render_readback_collect central_readback
#define jce_offscreen_retirement_collect central_retirement
#define jce_instance_upload_owner_advance central_upload
#include JCE_TEST_RENDERER_SUBMISSION_OWNER
#undef bgfx_frame
#undef jce_time_perf_counter
#undef jce_render_readback_collect
#undef jce_offscreen_retirement_collect
#undef jce_instance_upload_owner_advance

static unsigned char renderer_token, window_token, camera_token;
static JceRenderer *result;
static const JceRenderer *seen_renderer;
static JceWindow *seen_window;
static const JceRendererConfig *seen_config;
static const JceCamera *seen_camera;
static uint16_t seen_view;
static unsigned calls, snapshots, admission_begins, admission_finishes, joined_releases;
static bool admission_allowed, last_creation_succeeded, fallback_token, frame_allowed;

/* Count adapter consumption; real admission/Noop controls live separately. */
bool jce_renderer_backend_create_begin_internal(void)
{ ++admission_begins; return admission_allowed; }
void jce_renderer_backend_create_end_internal(bool created)
{ ++admission_finishes; last_creation_succeeded = created; }
void jce_renderer_backend_destroy_end_internal(void)
{ ++joined_releases; }
bool jce_renderer_is_fallback(const JceRenderer *r)
{ return r && fallback_token; }

JceRenderer *jce_renderer_create_impl(JceWindow *w, const JceRendererConfig *cfg)
{ calls++; seen_window = w; seen_config = cfg; return result; }
JceRenderer *jce_renderer_create_headless_impl(void)
{ calls++; return result; }
JceRenderer *jce_renderer_create_fallback_impl(JceWindow *w)
{ calls++; seen_window = w; return result; }
void jce_renderer_destroy_impl(JceRenderer *r)
{ calls++; seen_renderer = r; }
bool jce_renderer_begin_frame_impl(const JceRenderer *r, JceWindow *w)
{ calls++; seen_renderer = r; seen_window = w; return r && w && frame_allowed; }
bool jce_renderer_begin_frame_3d_impl(const JceRenderer *r, JceWindow *w,
                                      const JceCamera *cam, uint16_t view)
{ calls++; seen_renderer = r; seen_window = w; seen_camera = cam; seen_view = view; return r && w && frame_allowed; }
bool jce_renderer_end_frame_impl(const JceRenderer *r)
{ calls++; seen_renderer = r; return r && frame_allowed; }
void jce_renderer_trace_snapshot(const JceRenderer *r)
{ (void)r; ++snapshots; }

void setUp(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    result = (JceRenderer *)&renderer_token;
    seen_renderer = NULL; seen_window = NULL; seen_config = NULL;
    seen_camera = NULL; seen_view = 0; calls = 0; snapshots = 0;
    admission_begins = admission_finishes = joined_releases = 0;
    admission_allowed = frame_allowed = true; last_creation_succeeded = false; fallback_token = false;
    memset(&central_renderer, 0, sizeof central_renderer);
    memset(&s_device, 0, sizeof s_device);
    central_frames = central_context_queries = central_collections = 0;
    central_retirements = central_uploads = 0;
    central_clock_calls = 0;
    central_reverse_clock = false;
    central_current = true; central_no_progress = false;
    jce_perf_phase_set_enabled(false);
}
void tearDown(void) { jce_trace_set_enabled(false); }

static void test_failure_and_nested_service_identity(void)
{
    JceTraceScope parent = jce_trace_scope_begin("host");
    JceTraceCursor cursor;
    JceTraceEvent events[12];
    JceTraceStats stats;
    JceWindow *w = (JceWindow *)&window_token;
    JceRendererConfig cfg;
    memset(&cfg, 0, sizeof(cfg));
    result = NULL;
    TEST_ASSERT_NULL(jce_renderer_create(w, &cfg));
    TEST_ASSERT_EQUAL_PTR(w, seen_window);
    TEST_ASSERT_EQUAL_PTR(&cfg, seen_config);
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    result = (JceRenderer *)&renderer_token;
    TEST_ASSERT_EQUAL_PTR(result, jce_renderer_create_headless());
    TEST_ASSERT_EQUAL_PTR(result, jce_renderer_create_fallback(w));
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(12, jce_trace_read(&cursor, events, 12));
    TEST_ASSERT_EQUAL_STRING("renderer.create", events[2].name);
    TEST_ASSERT_EQUAL_UINT64(parent.id, events[2].parent_id);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, events[4].value_u32[0]);
    TEST_ASSERT_EQUAL_STRING("renderer.create_headless", events[5].name);
    TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, events[7].value_u32[0]);
    TEST_ASSERT_EQUAL_STRING("renderer.create_fallback", events[8].name);
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(4, stats.work_items_completed);
    TEST_ASSERT_EQUAL_UINT32(3, calls);
}

static void test_frame_and_destroy_preserve_arguments_and_balanced_spans(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[12];
    JceTraceStats stats;
    JceWindow *w = (JceWindow *)&window_token;
    const JceCamera *cam = (const JceCamera *)&camera_token;
    jce_renderer_begin_frame(result, w);
    jce_renderer_begin_frame_3d(result, w, cam, 519);
    TEST_ASSERT_EQUAL_PTR(result, seen_renderer);
    TEST_ASSERT_EQUAL_PTR(w, seen_window);
    TEST_ASSERT_EQUAL_PTR(cam, seen_camera);
    TEST_ASSERT_EQUAL_UINT16(519, seen_view);
    jce_renderer_end_frame(result);
    jce_renderer_destroy(result);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(12, jce_trace_read(&cursor, events, 12));
    TEST_ASSERT_EQUAL_STRING("renderer.frame_configure", events[0].name);
    TEST_ASSERT_EQUAL_STRING("renderer.view_configure", events[3].name);
    TEST_ASSERT_EQUAL_STRING("renderer.submit_cpu", events[6].name);
    TEST_ASSERT_EQUAL_STRING("renderer.destroy", events[9].name);
    for (unsigned i = 0; i < 4; ++i) {
        TEST_ASSERT_EQUAL_UINT64(events[i*3+1].id, events[i*3+2].id);
        TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, events[i*3+2].value_u32[0]);
    }
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(4, stats.work_items_completed);
    TEST_ASSERT_EQUAL_UINT32(4, calls);
    TEST_ASSERT_EQUAL_UINT32(1, snapshots);
}

static void test_disabled_capture_keeps_work_and_emits_no_events(void)
{
    JceTraceCursor cursor;
    JceTraceEvent event;
    jce_trace_set_enabled(false);
    jce_renderer_end_frame(result);
    jce_renderer_destroy(result);
    TEST_ASSERT_EQUAL_UINT32(2, calls);
    /* Dispatch remains available to PERF_LOG; the actual snapshot owner
     * independently gates statistics access and canonical trace emission. */
    TEST_ASSERT_EQUAL_UINT32(1, snapshots);
    TEST_ASSERT_EQUAL_PTR(result, seen_renderer);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
}

static void test_null_cleanup_and_frame_record_cancelled_attempts(void)
{
    JceTraceCursor cursor;
    JceTraceEvent events[9];
    jce_renderer_begin_frame(NULL, NULL);
    jce_renderer_end_frame(NULL);
    jce_renderer_destroy(NULL);
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(9, jce_trace_read(&cursor, events, 9));
    for (unsigned i = 0; i < 3; ++i)
        TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_CANCELLED, events[i*3+2].value_u32[0]);
    TEST_ASSERT_EQUAL_UINT32(3, calls);
}

static void test_rejected_creates_never_consume_impl_or_finish_old_lease(void)
{
    admission_allowed = false;
    JceRendererConfig cfg = {0};
    TEST_ASSERT_NULL(jce_renderer_create((JceWindow *)&window_token, &cfg));
    TEST_ASSERT_NULL(jce_renderer_create_headless());
    TEST_ASSERT_EQUAL_UINT32(2, admission_begins);
    TEST_ASSERT_EQUAL_UINT32(0, calls);
    TEST_ASSERT_EQUAL_UINT32(0, admission_finishes);
    TEST_ASSERT_EQUAL_UINT32(0, joined_releases);
    JceTraceStats stats;
    jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
    TEST_ASSERT_EQUAL_UINT64(2, stats.work_items_completed);
}

static void test_fallback_and_null_destroy_do_not_release_backend_admission(void)
{
    fallback_token = true;
    jce_renderer_destroy(result);
    jce_renderer_destroy(NULL);
    TEST_ASSERT_EQUAL_UINT32(2, calls);
    TEST_ASSERT_EQUAL_UINT32(0, joined_releases);
    fallback_token = false;
    jce_renderer_destroy(result);
    TEST_ASSERT_EQUAL_UINT32(1, joined_releases);
}

static void test_nonnull_refused_frames_record_failed_without_snapshot(void)
{
    frame_allowed = false;
    jce_renderer_begin_frame(result, (JceWindow *)&window_token);
    jce_renderer_begin_frame_3d(result, (JceWindow *)&window_token, NULL, 0);
    jce_renderer_end_frame(result);
    JceTraceCursor cursor; JceTraceEvent events[9];
    jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(9, jce_trace_read(&cursor, events, 9));
    for (unsigned i = 0; i < 3; ++i)
        TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_FAILED, events[i*3+2].value_u32[0]);
    TEST_ASSERT_EQUAL_UINT32(0, snapshots);
    TEST_ASSERT_EQUAL_UINT32(3, calls);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
}

static void assert_submission_packet(int64_t generation, uint32_t before,
    uint32_t after, uint32_t ordinal, uint64_t parent, bool clock_expected)
{
    const char *names[] = {"renderer.submit.context_generation",
        "renderer.submit.backend_serial_before", "renderer.submit.backend_serial_after",
        "renderer.submit.ordinal_before", "renderer.submit_clock.frequency",
        "renderer.submit_clock.begin", "renderer.submit_clock.end"};
    const int64_t values[] = {generation, (int64_t)before, (int64_t)after,
        (int64_t)ordinal, (int64_t)jce_time_perf_freq(),
        (int64_t)central_clock_ticks[0], (int64_t)central_clock_ticks[1]};
    const unsigned expected_count = clock_expected ? 7u : 4u;
    JceTraceEvent events[32]; JceTraceCursor cursor;
    uint64_t span = 0, begun = 0, ended = 0;
    unsigned counts[7] = {0}, submissions = 0, begins = 0, ends = 0, counters = 0;
    jce_trace_cursor_init(&cursor);
    const uint32_t count = jce_trace_read(&cursor, events, 32);
    TEST_ASSERT_GREATER_THAN_UINT32(0, count);
    for (uint32_t i = 0; i < count; ++i) {
        JceTraceEvent *event = &events[i];
        if (strcmp(event->name, "renderer.frame_advance_cpu")) continue;
        if (event->type == JCE_TRACE_EVENT_TASK_SUBMIT) {
            span = event->id; ++submissions;
            TEST_ASSERT_EQUAL_UINT64(parent, event->parent_id);
        } else if (event->type == JCE_TRACE_EVENT_TASK_BEGIN) {
            ++begins; begun = event->timestamp_ns;
            TEST_ASSERT_EQUAL_UINT64(span, event->id);
        } else if (event->type == JCE_TRACE_EVENT_TASK_END) {
            ++ends; ended = event->timestamp_ns;
            TEST_ASSERT_EQUAL_UINT64(span, event->id);
            TEST_ASSERT_EQUAL_UINT32(JCE_TRACE_TASK_SUCCEEDED, event->value_u32[0]);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(1, submissions);
    TEST_ASSERT_EQUAL_UINT32(1, begins); TEST_ASSERT_EQUAL_UINT32(1, ends);
    TEST_ASSERT_NOT_EQUAL(0, span);
    for (uint32_t i = 0; i < count; ++i) {
        JceTraceEvent *event = &events[i];
        if (event->type != JCE_TRACE_EVENT_COUNTER) continue;
        ++counters;
        for (unsigned n = 0; n < expected_count; ++n) {
            if (strcmp(event->name, names[n])) continue;
            ++counts[n];
            TEST_ASSERT_EQUAL_UINT32(n + 1u, counters);
            TEST_ASSERT_EQUAL_UINT64(span, event->id);
            TEST_ASSERT_EQUAL_UINT64(span, event->parent_id);
            TEST_ASSERT_EQUAL_UINT64(jce_thread_current_id(), event->thread_id);
            TEST_ASSERT_EQUAL_INT64(values[n], (int64_t)event->value_u64);
            TEST_ASSERT_TRUE(event->timestamp_ns >= begun && event->timestamp_ns <= ended);
        }
    }
    TEST_ASSERT_EQUAL_UINT32(expected_count, counters);
    for (unsigned n = 0; n < expected_count; ++n) TEST_ASSERT_EQUAL_UINT32(1, counts[n]);
    TEST_ASSERT_EQUAL_UINT32(central_frames, central_clock_frames[1]);
    TEST_ASSERT_EQUAL_UINT32(central_frames - 1u, central_clock_frames[0]);
    TEST_ASSERT_EQUAL_UINT32(central_collections - 1u, central_clock_collections[0]);
    TEST_ASSERT_EQUAL_UINT32(central_clock_collections[0], central_clock_collections[1]);
}

static void test_central_submission_packet_uses_actual_call_and_parent(void)
{
    central_renderer.submitted_payload.context_generation = UINT64_C(9007199254740993);
    s_device.frame_index = 58; s_device.offscreen_submission_index = 7;
    JceTraceScope parent = jce_trace_scope_begin("host");
    TEST_ASSERT_EQUAL_UINT32(59, jce_renderer_submit_frame(&central_renderer));
    TEST_ASSERT_EQUAL_UINT64(parent.id, jce_trace_task_current_id());
    jce_trace_scope_end(parent, JCE_TRACE_TASK_SUCCEEDED);
    assert_submission_packet(INT64_C(9007199254740993), 58, 59, 7, parent.id, true);
    TEST_ASSERT_EQUAL_UINT32(1, central_frames); TEST_ASSERT_EQUAL_UINT32(1, central_context_queries);
    TEST_ASSERT_EQUAL_UINT32(1, central_collections); TEST_ASSERT_EQUAL_UINT32(1, central_retirements);
    TEST_ASSERT_EQUAL_UINT32(1, central_uploads); TEST_ASSERT_EQUAL_UINT32(8, s_device.offscreen_submission_index);
    JceTraceStats stats; jce_trace_get_stats(&stats);
    TEST_ASSERT_EQUAL_UINT32(0, stats.active_work_items);
}

static void test_central_submission_packet_preserves_zero_wrap_and_retained_epoch(void)
{
    /* Upload unbind marks valid=false but retains the owned renderer epoch.
     * That identity is not a claim that its backend is still available. */
    central_renderer.submitted_payload.context_generation = 9;
    central_renderer.submitted_payload.valid = false;
    s_device.frame_index = s_device.offscreen_submission_index = UINT32_MAX;
    TEST_ASSERT_EQUAL_UINT32(0, jce_renderer_submit_frame(&central_renderer));
    assert_submission_packet(9, UINT32_MAX, 0, UINT32_MAX, 0, true);
    TEST_ASSERT_EQUAL_UINT32(0, s_device.offscreen_submission_index);
    TEST_ASSERT_EQUAL_UINT32(1, central_frames);
}

static void test_central_submission_packet_keeps_unknown_epoch_unavailable(void)
{
    const uint64_t generations[] = {0, UINT64_C(9223372036854775808), INT64_MAX};
    const int64_t expected[] = {-1, -1, INT64_MAX};
    for (unsigned i = 0; i < 3; ++i) {
        jce_trace_reset();
        central_renderer.submitted_payload.context_generation = generations[i];
        s_device.frame_index = 0; s_device.offscreen_submission_index = 0;
        TEST_ASSERT_EQUAL_UINT32(1, jce_renderer_submit_frame(&central_renderer));
        assert_submission_packet(expected[i], 0, 1, 0, 0, true);
        TEST_ASSERT_EQUAL_UINT64(generations[i], central_renderer.submitted_payload.context_generation);
    }
}

static void test_central_submission_packet_does_not_invent_serial_progress(void)
{
    central_no_progress = true;
    central_renderer.submitted_payload.context_generation = 6;
    s_device.frame_index = 37; s_device.offscreen_submission_index = 12;
    TEST_ASSERT_EQUAL_UINT32(37, jce_renderer_submit_frame(&central_renderer));
    assert_submission_packet(6, 37, 37, 12, 0, true);
    TEST_ASSERT_EQUAL_UINT32(1, central_frames); TEST_ASSERT_EQUAL_UINT32(13, s_device.offscreen_submission_index);
}

static void test_central_refusal_emits_no_accepted_submission_packet(void)
{
    central_current = false;
    TEST_ASSERT_EQUAL_UINT32(0, jce_renderer_submit_frame(&central_renderer));
    TEST_ASSERT_EQUAL_UINT32(0, jce_renderer_submit_frame(NULL));
    central_renderer.is_fallback = true;
    TEST_ASSERT_EQUAL_UINT32(0, jce_renderer_submit_frame(&central_renderer));
    TEST_ASSERT_EQUAL_UINT32(1, central_context_queries); TEST_ASSERT_EQUAL_UINT32(0, central_frames);
    TEST_ASSERT_EQUAL_UINT32(0, central_clock_calls);
    TEST_ASSERT_EQUAL_UINT32(0, central_collections); TEST_ASSERT_EQUAL_UINT32(0, central_retirements);
    TEST_ASSERT_EQUAL_UINT32(0, central_uploads);
    JceTraceCursor cursor; JceTraceEvent events[10]; jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(9, jce_trace_read(&cursor, events, 10));
    for (unsigned i = 0; i < 9; ++i)
        TEST_ASSERT_EQUAL_STRING("renderer.submit_refused", events[i].name);
}

static void test_central_trace_disabled_keeps_one_call_and_no_packet(void)
{
    jce_trace_set_enabled(false);
    TEST_ASSERT_EQUAL_UINT32(1, jce_renderer_submit_frame(&central_renderer));
    TEST_ASSERT_EQUAL_UINT32(1, central_frames); TEST_ASSERT_EQUAL_UINT32(1, central_context_queries);
    TEST_ASSERT_EQUAL_UINT32(1, central_collections); TEST_ASSERT_EQUAL_UINT32(1, central_retirements);
    TEST_ASSERT_EQUAL_UINT32(1, central_uploads);
    TEST_ASSERT_EQUAL_UINT32(0, central_clock_calls);
    JceTraceCursor cursor; JceTraceEvent event; jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
}

static void test_central_phase_only_uses_two_clock_reads_without_trace(void)
{
    jce_trace_set_enabled(false);
    jce_perf_phase_set_enabled(true);
    TEST_ASSERT_EQUAL_UINT32(1, jce_renderer_submit_frame(&central_renderer));
    TEST_ASSERT_EQUAL_UINT32(1, central_frames);
    TEST_ASSERT_EQUAL_UINT32(2, central_clock_calls);
    TEST_ASSERT_EQUAL_UINT32(0, central_clock_frames[0]);
    TEST_ASSERT_EQUAL_UINT32(1, central_clock_frames[1]);
    JceTraceCursor cursor; JceTraceEvent event; jce_trace_cursor_init(&cursor);
    TEST_ASSERT_EQUAL_UINT32(0, jce_trace_read(&cursor, &event, 1));
}

static void test_central_reversed_clock_keeps_the_original_identity_packet(void)
{
    central_reverse_clock = true;
    central_renderer.submitted_payload.context_generation = 6;
    TEST_ASSERT_EQUAL_UINT32(1, jce_renderer_submit_frame(&central_renderer));
    TEST_ASSERT_EQUAL_UINT32(2, central_clock_calls);
    assert_submission_packet(6, 0, 1, 0, 0, false);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_failure_and_nested_service_identity);
    RUN_TEST(test_frame_and_destroy_preserve_arguments_and_balanced_spans);
    RUN_TEST(test_disabled_capture_keeps_work_and_emits_no_events);
    RUN_TEST(test_null_cleanup_and_frame_record_cancelled_attempts);
    RUN_TEST(test_rejected_creates_never_consume_impl_or_finish_old_lease);
    RUN_TEST(test_fallback_and_null_destroy_do_not_release_backend_admission);
    RUN_TEST(test_nonnull_refused_frames_record_failed_without_snapshot);
    RUN_TEST(test_central_submission_packet_uses_actual_call_and_parent);
    RUN_TEST(test_central_submission_packet_preserves_zero_wrap_and_retained_epoch);
    RUN_TEST(test_central_submission_packet_keeps_unknown_epoch_unavailable);
    RUN_TEST(test_central_submission_packet_does_not_invent_serial_progress);
    RUN_TEST(test_central_refusal_emits_no_accepted_submission_packet);
    RUN_TEST(test_central_trace_disabled_keeps_one_call_and_no_packet);
    RUN_TEST(test_central_phase_only_uses_two_clock_reads_without_trace);
    RUN_TEST(test_central_reversed_clock_keeps_the_original_identity_packet);
    return UNITY_END();
}
