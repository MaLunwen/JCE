/* Exercise the production C99 emitter through the actual trace core. */
#include "unity.h"
#ifndef JCE_TEST_RENDERER_SNAPSHOT_HEADER
#define JCE_TEST_RENDERER_SNAPSHOT_HEADER "renderer/jce_renderer_snapshot_internal.h"
#endif
#include JCE_TEST_RENDERER_SNAPSHOT_HEADER
#include <string.h>
#include <jce/os/core/jce_filesystem.h>

enum { SNAPSHOT_COUNTERS = 31 };
static JceTraceEvent events[256];
static uint32_t event_count;

void setUp(void)
{
    jce_trace_set_enabled(true);
    jce_trace_reset();
    event_count = 0;
}

void tearDown(void)
{
    jce_trace_set_enabled(false);
}

static JceRendererSnapshot available_snapshot(void)
{
    const JceRendererSnapshot snapshot = {
        .draw_count = 70000,
        .compute_count = 9,
        .texture_count = 21,
        .framebuffer_count = 4,
        .program_count = 31,
        .shader_count = 42,
        .vertex_buffer_count = 13,
        .index_buffer_count = 12,
        .transient_vb_used_bytes = 100000,
        .transient_ib_used_bytes = 9000,
        .texture_estimate_bytes = INT64_C(7000000000),
        .rt_estimate_bytes = INT64_C(5000000000),
        .adapter_reported_used_bytes = INT64_C(9000000000),
        .gpu_timer_frequency = INT64_C(1000000000),
        .gpu_time_begin = INT64_C(15000000000),
        .gpu_time_end = INT64_C(15006000000),
        .encoder_frame_used = 1,
        .adapter_reported_vendor_id = 0x8086,
        .adapter_reported_device_id = 0xa788,
        .bgfx_renderer_type = 2, /* Pinned bgfx raw D3D11 enum value. */
        .host_submission_ordinal = 57,
        .device_epoch = 7,
        .gpu_frame_num = 48,
        .gpu_view_count = 2,
        .gpu_view_data_available = true,
    };
    return snapshot;
}

static void read_events(void)
{
    JceTraceCursor cursor;
    jce_trace_cursor_init(&cursor);
    event_count = jce_trace_read(&cursor, events, 256);
    TEST_ASSERT_EQUAL_UINT64(0, cursor.lost_events);
    for (uint32_t i = 0; i < event_count; ++i) {
        TEST_ASSERT_EQUAL_INT(JCE_TRACE_EVENT_COUNTER, events[i].type);
        TEST_ASSERT_EQUAL_INT(JCE_TRACE_CATEGORY_RENDER, events[i].category);
    }
}

static void assert_counter(const char *name, int64_t expected)
{
    unsigned matches = 0;
    for (uint32_t i = 0; i < event_count; ++i) {
        if (strcmp(name, events[i].name) != 0) continue;
        TEST_ASSERT_EQUAL_UINT64((uint64_t)expected, events[i].value_u64);
        ++matches;
    }
    TEST_ASSERT_EQUAL_UINT32_MESSAGE(1, matches, name);
}

static void test_existing_resource_and_timer_observations_preserve_values(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 16;
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_COUNTERS, event_count);
    assert_counter("renderer.cpu_submission_index", 60);
    assert_counter("renderer.draw_count", 70000);
    assert_counter("renderer.compute_count", 9);
    assert_counter("renderer.texture_count", 21);
    assert_counter("renderer.framebuffer_count", 4);
    assert_counter("renderer.program_count", 31);
    assert_counter("renderer.shader_count", 42);
    assert_counter("renderer.vertex_buffer_count", 13);
    assert_counter("renderer.index_buffer_count", 12);
    assert_counter("renderer.transient_vb_used_bytes", 100000);
    assert_counter("renderer.transient_ib_used_bytes", 9000);
    assert_counter("renderer.texture_estimate_bytes", INT64_C(7000000000));
    assert_counter("renderer.rt_estimate_bytes", INT64_C(5000000000));
    assert_counter("gpu.adapter_reported_used_bytes", INT64_C(9000000000));
    assert_counter("gpu.timer_frequency", INT64_C(1000000000));
    assert_counter("gpu.reported_frame_ticks", 6000000);
    assert_counter("renderer.encoder_frame_used", 1);
    assert_counter("renderer.encoder_effective_capacity", 16);
    assert_counter("gpu.adapter_reported_vendor_id", 0x8086);
    assert_counter("gpu.adapter_reported_device_id", 0xa788);
    assert_counter("gpu.bgfx_renderer_type", 2);
}

static void test_requested_limit_is_distinct_from_effective_capacity(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 1;
    /* The startup request is a separate observation, never inferred by emit. */
    jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, "renderer.encoder_capacity", 16);
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_COUNTERS + 1, event_count);
    assert_counter("renderer.encoder_capacity", 16);
    assert_counter("renderer.encoder_effective_capacity", 1);
    assert_counter("renderer.encoder_frame_used", 1);
}

static void test_frame_handle_count_does_not_replace_capacity(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 4;
    snapshot.encoder_frame_used = 3;
    jce_renderer_snapshot_emit(120, &snapshot, &capacity);
    read_events();
    assert_counter("renderer.encoder_frame_used", 3);
    assert_counter("renderer.encoder_effective_capacity", 4);
}

static void test_missing_caps_reports_unavailable_with_valid_frame_stats(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    jce_renderer_snapshot_emit(60, &snapshot, NULL);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_COUNTERS, event_count);
    assert_counter("renderer.encoder_frame_used", 1);
    assert_counter("renderer.encoder_effective_capacity", -1);
    assert_counter("gpu.adapter_reported_vendor_id", -1);
    assert_counter("gpu.adapter_reported_device_id", -1);
    assert_counter("gpu.bgfx_renderer_type", -1);
    assert_counter("renderer.draw_count", 70000);
}

static void test_missing_stats_emits_no_invented_snapshot(void)
{
    uint32_t capacity = 16;
    jce_renderer_snapshot_emit(60, NULL, &capacity);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
}

static void test_sixty_submission_cadence_is_unchanged(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    const uint32_t submissions[] = {59, 60, 119, 120};
    const bool expected_due[] = {false, true, false, true};
    uint32_t capacity = 1;
    for (unsigned i = 0; i < sizeof(submissions) / sizeof(submissions[0]); ++i) {
        TEST_ASSERT_EQUAL_INT(expected_due[i],
            jce_renderer_snapshot_due(submissions[i]));
        if (jce_renderer_snapshot_due(submissions[i]))
            jce_renderer_snapshot_emit(submissions[i], &snapshot, &capacity);
    }
    TEST_ASSERT_TRUE(jce_renderer_snapshot_due(0));
    read_events();
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_COUNTERS * 2, event_count);
    TEST_ASSERT_EQUAL_STRING("renderer.cpu_submission_index", events[0].name);
    TEST_ASSERT_EQUAL_UINT64(60, events[0].value_u64);
    TEST_ASSERT_EQUAL_STRING("renderer.cpu_submission_index", events[SNAPSHOT_COUNTERS].name);
    TEST_ASSERT_EQUAL_UINT64(120, events[SNAPSHOT_COUNTERS].value_u64);
}

static void test_disabled_capture_records_no_counters(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 1;
    jce_trace_set_enabled(false);
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
}

static void test_unavailable_gpu_timer_and_memory_preserve_sentinels(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    snapshot.gpu_timer_frequency = 0;
    snapshot.adapter_reported_used_bytes = INT64_MAX;
    snapshot.texture_estimate_bytes = -1;
    snapshot.rt_estimate_bytes = -1;
    jce_renderer_snapshot_emit(60, &snapshot, NULL);
    read_events();
    assert_counter("gpu.adapter_reported_used_bytes", -1);
    assert_counter("gpu.timer_frequency", 0);
    assert_counter("gpu.reported_frame_ticks", -1);
    assert_counter("renderer.texture_estimate_bytes", -1);
    assert_counter("renderer.rt_estimate_bytes", -1);
}

static void test_capacity_keeps_its_full_backend_width(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = UINT32_MAX;
    snapshot.encoder_frame_used = UINT8_MAX;
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    assert_counter("renderer.encoder_effective_capacity", UINT32_MAX);
    assert_counter("renderer.encoder_frame_used", UINT8_MAX);
}

static void test_selected_adapter_identity_preserves_raw_bgfx_renderer_type(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 1;
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    assert_counter("gpu.adapter_reported_vendor_id", 0x8086);
    assert_counter("gpu.adapter_reported_device_id", 0xa788);
    assert_counter("gpu.bgfx_renderer_type", 2);

    jce_trace_reset();
    snapshot.adapter_reported_vendor_id = 0x10de;
    snapshot.adapter_reported_device_id = 0x1234;
    snapshot.bgfx_renderer_type = 7; /* Pinned bgfx raw OpenGLES enum value. */
    jce_renderer_snapshot_emit(120, &snapshot, &capacity);
    read_events();
    assert_counter("gpu.adapter_reported_vendor_id", 0x10de);
    assert_counter("gpu.adapter_reported_device_id", 0x1234);
    assert_counter("gpu.bgfx_renderer_type", 7);
}

static void test_reported_zero_adapter_values_are_available(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 1;
    snapshot.adapter_reported_vendor_id = 0;
    snapshot.adapter_reported_device_id = 0;
    snapshot.bgfx_renderer_type = 0;
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    assert_counter("gpu.adapter_reported_vendor_id", 0);
    assert_counter("gpu.adapter_reported_device_id", 0);
    assert_counter("gpu.bgfx_renderer_type", 0);
}

static void test_adapter_observations_keep_full_private_field_width(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 1;
    /* Synthetic width controls do not claim backend enum admission. */
    snapshot.adapter_reported_vendor_id = UINT16_MAX;
    snapshot.adapter_reported_device_id = UINT16_MAX;
    snapshot.bgfx_renderer_type = UINT32_MAX;
    jce_renderer_snapshot_emit(60, &snapshot, &capacity);
    read_events();
    assert_counter("gpu.adapter_reported_vendor_id", UINT16_MAX);
    assert_counter("gpu.adapter_reported_device_id", UINT16_MAX);
    assert_counter("gpu.bgfx_renderer_type", UINT32_MAX);
}

static void test_first_actual_submission_supplements_sixty_frame_cadence(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    uint32_t capacity = 1;
    const uint32_t frames[] = {3, 4, 59, 60, 61, 120};
    const uint32_t ordinals[] = {1, 2, 57, 58, 59, 118};
    const bool due[] = {true, false, false, true, false, true};
    for (unsigned i = 0; i < sizeof(frames) / sizeof(frames[0]); ++i) {
        TEST_ASSERT_EQUAL_INT(due[i], jce_renderer_snapshot_due_at_submission(frames[i], ordinals[i]));
        if (jce_renderer_snapshot_due_at_submission(frames[i], ordinals[i]))
            jce_renderer_snapshot_emit(frames[i], &snapshot, &capacity);
    }
    read_events();
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_COUNTERS * 3, event_count);
    TEST_ASSERT_EQUAL_UINT64(3, events[0].value_u64);
    TEST_ASSERT_EQUAL_UINT64(60, events[SNAPSHOT_COUNTERS].value_u64);
    TEST_ASSERT_EQUAL_UINT64(120, events[SNAPSHOT_COUNTERS * 2].value_u64);
}

static void test_new_session_first_submission_and_traceoff_are_independent(void)
{
    TEST_ASSERT_TRUE(jce_renderer_snapshot_due_at_submission(9, 1));
    TEST_ASSERT_FALSE(jce_renderer_snapshot_due_at_submission(9, 2));
    TEST_ASSERT_TRUE(jce_renderer_snapshot_due_at_submission(9, 1));
    JceRendererSnapshot snapshot = available_snapshot();
    jce_trace_set_enabled(false);
    if (jce_renderer_snapshot_due_at_submission(9, 1))
        jce_renderer_snapshot_emit(9, &snapshot, NULL);
    read_events(); TEST_ASSERT_EQUAL_UINT32(0, event_count);
}

static void test_source_frame_and_view_ids_are_not_cpu_submission_identity(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    const JceRendererViewSnapshot color = {3, 46, 15000000001, 15003000001};
    const JceRendererViewSnapshot ui = {250, 47, 15003000001, 15006000001};
    const uint64_t saved_span = jce_trace_task_current_id();
    const uint64_t span = UINT64_C(9007199254740993);
    jce_renderer_snapshot_emit(60, &snapshot, NULL);
    jce_trace_task_restore_id(span);
    jce_renderer_snapshot_emit_view(60, &snapshot, &color);
    jce_renderer_snapshot_emit_view(60, &snapshot, &ui);
    jce_trace_task_restore_id(saved_span);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(SNAPSHOT_COUNTERS + 18, event_count);
    assert_counter("renderer.api_frame_number", 60);
    assert_counter("renderer.host_submission_ordinal", 57);
    assert_counter("renderer.device_epoch", 7);
    assert_counter("renderer.device_epoch_available", 1);
    assert_counter("gpu.source_frame_number", 48);
    /* Both stack-built name sets must survive all subsequent formatter reuse. */
    const char *suffixes[] = {"device_epoch", "api_frame", "submission_ordinal",
        "source_frame", "begin_ticks", "end_ticks", "timer_frequency",
        "interval_available", "reported_ticks"};
    const int64_t expected[][9] = {
        {7, 60, 57, 46, INT64_C(15000000001), INT64_C(15003000001),
            INT64_C(1000000000), 1, INT64_C(3000000)},
        {7, 60, 57, 47, INT64_C(15003000001), INT64_C(15006000001),
            INT64_C(1000000000), 1, INT64_C(3000000)}};
    const unsigned view_ids[] = {3, 250};
    for (unsigned v = 0; v < 2; ++v) {
        for (unsigned field = 0; field < 9; ++field) {
            char expected_name[48];
            (void)snprintf(expected_name, sizeof(expected_name), "gpu.view.%u.%s",
                view_ids[v], suffixes[field]);
            assert_counter(expected_name, expected[v][field]);
            const JceTraceEvent *event = &events[SNAPSHOT_COUNTERS + v * 9 + field];
            TEST_ASSERT_EQUAL_STRING(expected_name, event->name);
            TEST_ASSERT_EQUAL_UINT64(span, event->id);
            TEST_ASSERT_EQUAL_UINT64(span, event->parent_id);
        }
    }
}

static void test_raw_ticks_above_double_precision_survive_actual_json_export(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    snapshot.gpu_time_begin = INT64_C(9007199254740993);
    snapshot.gpu_time_end = INT64_C(9007199254740995);
    snapshot.gpu_timer_frequency = INT64_C(9007199254740997);
    jce_renderer_snapshot_emit(60, &snapshot, NULL);
    read_events();
    assert_counter("gpu.raw_begin_ticks", INT64_C(9007199254740993));
    assert_counter("gpu.raw_end_ticks", INT64_C(9007199254740995));
    assert_counter("gpu.timer_frequency", INT64_C(9007199254740997));
    assert_counter("gpu.reported_frame_ticks", 2);
    char path[512] = {0};
    TEST_ASSERT_TRUE(jce_fs_host_make_unique_path("_ut_gpu_provenance.json", path, sizeof(path)));
    TEST_ASSERT_TRUE(jce_trace_export_chrome_json(path));
    uint64_t read = 0, total = 0;
    char *json = (char *)jce_fs_host_read_capped(path, 32768, &read, &total);
    TEST_ASSERT_NOT_NULL(json);
    const bool bounded_complete = read == total && total < 32768;
    const bool begin_exact = strstr(json, "\"value\":9007199254740993") != NULL;
    const bool end_exact = strstr(json, "\"value\":9007199254740995") != NULL;
    const bool frequency_exact = strstr(json, "\"value\":9007199254740997") != NULL;
    jce_fs_buffer_free(json);
    TEST_ASSERT_TRUE(jce_fs_host_remove_file(path));
    TEST_ASSERT_TRUE(bounded_complete);
    TEST_ASSERT_TRUE(begin_exact);
    TEST_ASSERT_TRUE(end_exact);
    TEST_ASSERT_TRUE(frequency_exact);
}

static void test_zero_and_wrapping_frame_ids_are_kept_with_device_epoch(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    snapshot.gpu_frame_num = 0;
    snapshot.gpu_time_begin = 0;
    snapshot.gpu_time_end = 1;
    snapshot.host_submission_ordinal = 1;
    const JceRendererViewSnapshot zero_view = {UINT16_MAX, 0, 0, 1};
    jce_renderer_snapshot_emit(0, &snapshot, NULL);
    jce_renderer_snapshot_emit_view(0, &snapshot, &zero_view);
    read_events();
    assert_counter("renderer.api_frame_number", 0);
    assert_counter("gpu.source_frame_number", 0);
    assert_counter("gpu.interval_available", 1);
    assert_counter("gpu.raw_begin_ticks", 0);
    assert_counter("gpu.view.65535.source_frame", 0);
    assert_counter("gpu.view.65535.interval_available", 1);
    jce_trace_reset();
    snapshot.gpu_frame_num = UINT32_MAX;
    snapshot.device_epoch = 8;
    jce_renderer_snapshot_emit(UINT32_MAX, &snapshot, NULL);
    read_events();
    assert_counter("renderer.api_frame_number", UINT32_MAX);
    assert_counter("gpu.source_frame_number", UINT32_MAX);
    assert_counter("renderer.device_epoch", 8);
}

static void test_repeated_latched_sample_does_not_become_a_new_gpu_frame(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    for (uint32_t cpu_frame = 60; cpu_frame <= 61; ++cpu_frame) {
        jce_trace_reset();
        snapshot.host_submission_ordinal = cpu_frame - 3;
        jce_renderer_snapshot_emit(cpu_frame, &snapshot, NULL);
        read_events();
        assert_counter("renderer.api_frame_number", cpu_frame);
        assert_counter("renderer.host_submission_ordinal", cpu_frame - 3);
        assert_counter("gpu.source_frame_number", 48);
        assert_counter("gpu.raw_begin_ticks", INT64_C(15000000000));
        assert_counter("gpu.raw_end_ticks", INT64_C(15006000000));
    }
}

static void test_unavailable_intervals_keep_raw_observations_without_overflow(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    const int64_t values[][3] = {{0, 1, 2}, {-1, 1, 2}, {1, 0, 0},
        {1, -1, INT64_MAX}, {1, 9, 8}};
    for (unsigned i = 0; i < sizeof(values)/sizeof(values[0]); ++i) {
        jce_trace_reset();
        snapshot.gpu_timer_frequency = values[i][0];
        snapshot.gpu_time_begin = values[i][1];
        snapshot.gpu_time_end = values[i][2];
        jce_renderer_snapshot_emit(60, &snapshot, NULL);
        read_events();
        assert_counter("gpu.interval_available", 0);
        assert_counter("gpu.reported_frame_ticks", -1);
        assert_counter("gpu.raw_begin_ticks", values[i][1]);
        assert_counter("gpu.raw_end_ticks", values[i][2]);
        assert_counter("gpu.timer_frequency", values[i][0]);
    }
    snapshot = available_snapshot();
    snapshot.device_epoch = UINT64_MAX;
    snapshot.gpu_view_data_available = false;
    jce_trace_reset();
    jce_renderer_snapshot_emit(60, &snapshot, NULL);
    read_events();
    assert_counter("renderer.device_epoch", -1);
    assert_counter("renderer.device_epoch_available", 0);
    assert_counter("gpu.view_count", 2);
    assert_counter("gpu.view_data_available", 0);
}

static void test_view_traceoff_and_missing_metadata_emit_no_invented_views(void)
{
    JceRendererSnapshot snapshot = available_snapshot();
    const JceRendererViewSnapshot view = {UINT16_MAX, 0, 0, 1};
    snapshot.gpu_view_data_available = false;
    jce_renderer_snapshot_emit_view(60, &snapshot, &view);
    jce_renderer_snapshot_emit_view(60, NULL, &view);
    jce_renderer_snapshot_emit_view(60, &snapshot, NULL);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
    snapshot.gpu_view_data_available = true;
    jce_trace_set_enabled(false);
    jce_renderer_snapshot_emit_view(60, &snapshot, &view);
    read_events();
    TEST_ASSERT_EQUAL_UINT32(0, event_count);
}


static void test_view_names_keep_complete_provider_bytes_and_rotating_identity(void)
{
    JceRendererViewNames names = jce_renderer_view_names_begin(2, 57, true);
    TEST_ASSERT_EQUAL_UINT16(0, names.start);
    TEST_ASSERT_TRUE(jce_renderer_view_names_append(&names, 0, 0, "Renderer/Main", sizeof("Renderer/Main")));
    TEST_ASSERT_TRUE(names.truncated);
    const char label[] = "View \xc3\xa9\n% x";
    TEST_ASSERT_TRUE(jce_renderer_view_names_append(&names, 171, UINT32_MAX, label, sizeof(label)));
    TEST_ASSERT_EQUAL_STRING("0@0=Renderer/Main;171@4294967295=View%20%C3%A9%0A%25%20x", names.text);
    TEST_ASSERT_EQUAL_UINT16(2, names.listed);
    TEST_ASSERT_FALSE(names.truncated);
    TEST_ASSERT_FALSE(names.unavailable);
    names = jce_renderer_view_names_begin(2, 117, true);
    TEST_ASSERT_EQUAL_UINT16(1, names.start);
    TEST_ASSERT_TRUE(jce_renderer_view_names_append(&names, 171, 48, "Renderer/OffscreenComposite", sizeof("Renderer/OffscreenComposite")));
    TEST_ASSERT_TRUE(jce_renderer_view_names_append(&names, 0, 48, "", 1));
    TEST_ASSERT_EQUAL_STRING("171@48=Renderer/OffscreenComposite;0@48=", names.text);
    TEST_ASSERT_FALSE(names.truncated);
}
static void test_view_names_exact_capacity_and_unavailable_data_never_claim_complete(void)
{
    char fitting[220]; memset(fitting, 'a', sizeof(fitting) - 1); fitting[sizeof(fitting) - 1] = '\0';
    JceRendererViewNames names = jce_renderer_view_names_begin(2, 1, true);
    TEST_ASSERT_TRUE(jce_renderer_view_names_append(&names, 0, 0, fitting, sizeof(fitting)));
    TEST_ASSERT_EQUAL_UINT32(223, names.used);
    char saved[224]; memcpy(saved, names.text, sizeof(saved));
    TEST_ASSERT_FALSE(jce_renderer_view_names_append(&names, 171, 48, "last", 5));
    TEST_ASSERT_EQUAL_MEMORY(saved, names.text, sizeof(saved));
    TEST_ASSERT_EQUAL_UINT16(1, names.listed);
    TEST_ASSERT_TRUE(names.truncated);
    TEST_ASSERT_FALSE(names.unavailable);
    char unterminated[256]; memset(unterminated, 'a', sizeof(unterminated));
    names = jce_renderer_view_names_begin(1, 1, true);
    TEST_ASSERT_FALSE(jce_renderer_view_names_append(&names, 0, 0, unterminated, sizeof(unterminated)));
    TEST_ASSERT_TRUE(names.unavailable);
    TEST_ASSERT_TRUE(names.truncated);
    TEST_ASSERT_EQUAL_STRING("", names.text);
    names = jce_renderer_view_names_begin(1, 1, false);
    TEST_ASSERT_TRUE(names.unavailable);
    TEST_ASSERT_FALSE(jce_renderer_view_names_append(&names, 0, 0, "name", 5));
    names = jce_renderer_view_names_begin(0, 1, true);
    TEST_ASSERT_FALSE(names.truncated);
    TEST_ASSERT_FALSE(names.unavailable);
    TEST_ASSERT_EQUAL_UINT16(0, names.listed);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_existing_resource_and_timer_observations_preserve_values);
    RUN_TEST(test_requested_limit_is_distinct_from_effective_capacity);
    RUN_TEST(test_frame_handle_count_does_not_replace_capacity);
    RUN_TEST(test_missing_caps_reports_unavailable_with_valid_frame_stats);
    RUN_TEST(test_missing_stats_emits_no_invented_snapshot);
    RUN_TEST(test_sixty_submission_cadence_is_unchanged);
    RUN_TEST(test_disabled_capture_records_no_counters);
    RUN_TEST(test_unavailable_gpu_timer_and_memory_preserve_sentinels);
    RUN_TEST(test_capacity_keeps_its_full_backend_width);
    RUN_TEST(test_selected_adapter_identity_preserves_raw_bgfx_renderer_type);
    RUN_TEST(test_reported_zero_adapter_values_are_available);
    RUN_TEST(test_adapter_observations_keep_full_private_field_width);
    RUN_TEST(test_first_actual_submission_supplements_sixty_frame_cadence);
    RUN_TEST(test_new_session_first_submission_and_traceoff_are_independent);
    RUN_TEST(test_source_frame_and_view_ids_are_not_cpu_submission_identity);
    RUN_TEST(test_raw_ticks_above_double_precision_survive_actual_json_export);
    RUN_TEST(test_zero_and_wrapping_frame_ids_are_kept_with_device_epoch);
    RUN_TEST(test_repeated_latched_sample_does_not_become_a_new_gpu_frame);
    RUN_TEST(test_unavailable_intervals_keep_raw_observations_without_overflow);
    RUN_TEST(test_view_traceoff_and_missing_metadata_emit_no_invented_views);
    RUN_TEST(test_view_names_keep_complete_provider_bytes_and_rotating_identity);
    RUN_TEST(test_view_names_exact_capacity_and_unavailable_data_never_claim_complete);
    return UNITY_END();
}
