/* Complete raw export is one write; long phase names never overread a row. */
#include "application/jce_frame_kpi.h"
#include "application/jce_engine_perf_hitch.h"
#include <jce/os/core/jce_perf_phase.h>
#include "unity.h"
#include <math.h>
#include <string.h>

typedef struct {
    char bytes[32768];
    size_t size;
    unsigned calls;
    bool fail;
} Sink;

static char names[48][257];

static size_t SDLCALL sink_write(void *user, const void *bytes, size_t size,
                                 SDL_IOStatus *status)
{
    Sink *sink = user;
    ++sink->calls;
    if (sink->fail || size >= sizeof(sink->bytes) - sink->size) {
        *status = SDL_IO_STATUS_ERROR;
        return 0;
    }
    memcpy(sink->bytes + sink->size, bytes, size);
    sink->size += size;
    sink->bytes[sink->size] = 0;
    return size;
}

static SDL_IOStream *stream_for(Sink *sink)
{
    SDL_IOStreamInterface api;
    SDL_INIT_INTERFACE(&api);
    api.write = sink_write;
    return SDL_OpenIO(&api, sink);
}

void setUp(void) { jce_perf_phase_set_enabled(1); }
void tearDown(void) {}

static void test_context_header_and_exact_typed_csv_preserve_legacy_bytes(void)
{
    static const char expected[] =
        "18446744073709551615,frame_wall,0.0000,\"{\"\"v\"\":1,"
        "\"\"window_present\"\":true,\"\"flags_begin\"\":0,"
        "\"\"flags_end\"\":4294967295,\"\"point_size\"\":[640,360],"
        "\"\"drawable_size\"\":[1280,720],\"\"native_size_begin\"\":[1280,720],"
        "\"\"native_size_end\"\":[1920,1080],\"\"backend_frame_begin\"\":0,"
        "\"\"backend_frame_end\"\":4294967295}\"\n"
        "18446744073709551615,frame_cpu,2.5000,\n"
        "18446744073709551615,frame_gpu,0.1250,\n";
    JceFrameKpiContext context = {0};
    context.window_present = true;
    context.flags_begin_valid = context.flags_end_valid = true;
    context.flags_end = UINT32_MAX;
    context.point_width = 640; context.point_height = 360;
    context.drawable_width = 1280; context.drawable_height = 720;
    context.native_width_begin = 1280; context.native_height_begin = 720;
    context.native_width_end = 1920; context.native_height_end = 1080;
    context.backend_frame_begin_valid = context.backend_frame_end_valid = true;
    context.backend_frame_end = UINT32_MAX;
    JceGpuStats stats = {0};
    stats.valid = true; stats.cpu_frame_ms = 2.5; stats.gpu_ms = 0.125;
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    TEST_ASSERT_EQUAL_STRING("frame_index,phase,ms\n", jce_frame_kpi_csv_header(false));
    TEST_ASSERT_EQUAL_STRING("frame_index,phase,ms,context_json\n", jce_frame_kpi_csv_header(true));
    bool encoded = jce_frame_kpi_encode_with_context(&buffer, UINT64_MAX, 0.0,
        &stats, false, &context, &size);
    bool exact = encoded && size == sizeof expected - 1u &&
        memcmp(buffer.bytes, expected, sizeof expected) == 0;
    bool legacy = jce_frame_kpi_encode(&buffer, 7, 1.0, NULL, false, &size);
    bool old_bytes = legacy && strcmp(buffer.bytes, "7,frame_wall,1.0000\n") == 0;
    jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_TRUE(encoded); TEST_ASSERT_TRUE(exact); TEST_ASSERT_TRUE(old_bytes);
}

static void test_context_unknowns_are_null_and_nonwall_fields_are_empty(void)
{
    static const char absent[] =
        "0,frame_wall,1.0000,\"{\"\"v\"\":1,\"\"window_present\"\":false,"
        "\"\"flags_begin\"\":null,\"\"flags_end\"\":null,\"\"point_size\"\":null,"
        "\"\"drawable_size\"\":null,\"\"native_size_begin\"\":null,"
        "\"\"native_size_end\"\":null,\"\"backend_frame_begin\"\":0,"
        "\"\"backend_frame_end\"\":null}\"\n";
    JceFrameKpiContext context = {0};
    /* A missing window suppresses stale geometry/flags, but not renderer ID 0. */
    context.flags_begin_valid = context.flags_end_valid = true;
    context.flags_begin = context.flags_end = UINT32_MAX;
    context.point_width = context.point_height = context.drawable_width = context.drawable_height = 3;
    context.native_width_begin = context.native_height_begin = 4;
    context.native_width_end = context.native_height_end = 5;
    context.backend_frame_begin_valid = true;
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 1.0,
        NULL, false, &context, &size));
    TEST_ASSERT_EQUAL_STRING(absent, buffer.bytes);
    context.window_present = true;
    context.flags_begin_valid = context.flags_end_valid = false;
    context.point_height = context.drawable_width = context.native_height_begin = context.native_width_end = 0;
    jce_perf_phase_add("context_zero_phase", 0.0); jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 1, 2.0,
        NULL, true, &context, &size));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "1,context_zero_phase,0.0000,\n"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"flags_begin\"\":null,\"\"flags_end\"\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"point_size\"\":null,\"\"drawable_size\"\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"native_size_begin\"\":null,\"\"native_size_end\"\":null"));
    const char *json = strstr(buffer.bytes, "{\"\"v\"\":1");
    TEST_ASSERT_NOT_NULL(json);
    TEST_ASSERT_NULL(strstr(json + 1, "{\"\"v\"\":1"));
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_context_record_obeys_scratch_budget_without_partial_success(void)
{
    JceFrameKpiBuffer buffer = {0}; JceFrameKpiContext context = {0}; size_t size = 999;
    buffer.max_capacity = 32;
    bool encoded = jce_frame_kpi_encode_with_context(&buffer, 0, 1.0,
        NULL, false, &context, &size);
    size_t capacity = buffer.capacity;
    jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_FALSE(encoded); TEST_ASSERT_EQUAL_UINT(0, size);
    TEST_ASSERT_LESS_OR_EQUAL_UINT(32, capacity);
}

static void test_sampled_zero_snapshot_exports_zero_row(void)
{
    char report[128];
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_FALSE(jce_perf_phase_frame_has_data());
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 0, 1.0, NULL, true));
    TEST_ASSERT_EQUAL_STRING("0,frame_wall,1.0000\n", sink.bytes);

    sink.size = sink.calls = 0;
    jce_perf_phase_add("zero_phase", 0.0);
    jce_perf_phase_report(report, sizeof report);
    jce_perf_phase_frame_tick();
    bool exported = jce_frame_kpi_export(&buffer, stream, 1, 3.75, NULL, true);
    bool zero_row = strstr(sink.bytes, "1,zero_phase,0.0000\n") != NULL;
    bool wall_row = strstr(sink.bytes, "1,frame_wall,3.7500\n") != NULL;
    bool available = jce_perf_phase_frame_has_data() != 0;
    bool closed = SDL_CloseIO(stream);
    jce_frame_kpi_buffer_destroy(&buffer);

    TEST_ASSERT_TRUE(exported);
    TEST_ASSERT_EQUAL_UINT(1, sink.calls);
    TEST_ASSERT_TRUE(wall_row);
    TEST_ASSERT_TRUE_MESSAGE(zero_row,
        "a populated zero-duration snapshot was omitted from raw export");
    TEST_ASSERT_TRUE(available);
    TEST_ASSERT_TRUE(closed);
    TEST_ASSERT_NULL(buffer.bytes);
    TEST_ASSERT_EQUAL_UINT(0, buffer.capacity);
}

static void test_backend_latched_opt_in_preserves_default_bytes_and_zero(void)
{
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    JceGpuStats stats = {0}; stats.valid = true;
    stats.cpu_frame_ms = 2.5; stats.gpu_ms = 0.125;
    stats.cpu_submit_ms = 1.25; stats.wait_submit_ms = 0; stats.wait_render_ms = 7;
    const char *legacy = "7,frame_wall,4.0000\n7,frame_cpu,2.5000\n7,frame_gpu,0.1250\n";
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 7, 4, &stats, false, &size));
    TEST_ASSERT_EQUAL_UINT64(strlen(legacy), size);
    TEST_ASSERT_EQUAL_MEMORY(legacy, buffer.bytes, size + 1);
    char *scratch = buffer.bytes; size_t capacity = buffer.capacity;
    buffer.backend_timings = true;
    JceFrameKpiContext context = {0};
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 8, 4, &stats,
                                                      false, &context, &size));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "8,frame_backend_latched_cpu_submit,1.2500,\n"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "8,frame_backend_latched_wait_submit,0.0000,\n"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "8,frame_backend_latched_wait_render,7.0000,\n"));
    TEST_ASSERT_EQUAL_PTR(scratch, buffer.bytes);
    TEST_ASSERT_EQUAL_UINT64(capacity, buffer.capacity);
    jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_FALSE(buffer.backend_timings);
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 7, 4, &stats, false, &size));
    TEST_ASSERT_EQUAL_MEMORY(legacy, buffer.bytes, size + 1);
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_backend_latched_missing_and_invalid_timers_remain_unavailable(void)
{
    JceFrameKpiBuffer buffer = {0}; buffer.backend_timings = true; size_t size = 0;
    JceGpuStats stats = {0}; stats.valid = true;
    stats.cpu_frame_ms = 2; stats.gpu_ms = 1;
    stats.cpu_submit_ms = NAN; stats.wait_submit_ms = -1; stats.wait_render_ms = INFINITY;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 0, 3, &stats, false, &size));
    TEST_ASSERT_NULL(strstr(buffer.bytes, "frame_backend_latched_"));
    stats.wait_submit_ms = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 1, 3, &stats, false, &size));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "1,frame_backend_latched_wait_submit,0.0000\n"));
    TEST_ASSERT_NULL(strstr(buffer.bytes, "frame_backend_latched_cpu_submit"));
    TEST_ASSERT_NULL(strstr(buffer.bytes, "frame_backend_latched_wait_render"));
    stats.valid = false;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 2, 3, &stats, false, &size));
    TEST_ASSERT_EQUAL_STRING("2,frame_wall,3.0000\n", buffer.bytes);
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 3, 3, NULL, false, &size));
    TEST_ASSERT_EQUAL_STRING("3,frame_wall,3.0000\n", buffer.bytes);

    buffer.backend_timings = false;
    stats.valid = true;
    stats.gpu_ms = -1;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 4, 3, &stats, false, &size));
    TEST_ASSERT_EQUAL_STRING("4,frame_wall,3.0000\n4,frame_cpu,2.0000\n", buffer.bytes);
    stats.cpu_frame_ms = NAN;
    stats.gpu_ms = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&buffer, 5, 3, &stats, false, &size));
    TEST_ASSERT_EQUAL_STRING("5,frame_wall,3.0000\n5,frame_gpu,0.0000\n", buffer.bytes);
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_backend_latched_rows_keep_complete_record_budget_and_single_write(void)
{
    JceGpuStats stats = {0}; stats.valid = true;
    stats.cpu_frame_ms = 2; stats.gpu_ms = 1;
    stats.cpu_submit_ms = 3; stats.wait_submit_ms = 4; stats.wait_render_ms = 5;
    JceFrameKpiBuffer reference = {0}; reference.backend_timings = true;
    size_t required = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode(&reference, 9, 6, &stats, false, &required));
    JceFrameKpiBuffer exact = {0}; exact.backend_timings = true;
    exact.max_capacity = required + 1u;
    Sink sink = {0}; SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    bool exported = jce_frame_kpi_export(&exact, stream, 9, 6, &stats, false);
    bool complete = sink.size == required &&
        memcmp(sink.bytes, reference.bytes, required + 1u) == 0;
    unsigned successful_writes = sink.calls;
    jce_frame_kpi_buffer_destroy(&exact);
    exact.backend_timings = true;
    exact.max_capacity = required; /* One byte short including the NUL. */
    sink.size = sink.calls = 0; sink.bytes[0] = 0;
    bool refused = !jce_frame_kpi_export(&exact, stream, 9, 6, &stats, false);
    bool no_partial_write = !sink.calls && !sink.size;
    size_t encoded_size = 999;
    bool encode_refused = !jce_frame_kpi_encode(&exact, 9, 6, &stats, false, &encoded_size);
    bool closed = SDL_CloseIO(stream);
    jce_frame_kpi_buffer_destroy(&exact);
    jce_frame_kpi_buffer_destroy(&reference);
    TEST_ASSERT_TRUE(exported); TEST_ASSERT_TRUE(complete);
    TEST_ASSERT_EQUAL_UINT(1, successful_writes);
    TEST_ASSERT_TRUE(refused); TEST_ASSERT_TRUE(no_partial_write);
    TEST_ASSERT_TRUE(encode_refused); TEST_ASSERT_EQUAL_UINT64(0, encoded_size);
    TEST_ASSERT_TRUE(closed);
}

static void test_growth_complete_rows_and_reuse(void)
{
    for (unsigned index = 0; index < 48; ++index) {
        memset(names[index], 'x', 256);
        names[index][0] = 'A' + (char)(index % 26);
        names[index][1] = 'A' + (char)(index / 26);
        names[index][256] = 0;
        jce_perf_phase_add(names[index], index + 0.25);
    }
    jce_perf_phase_frame_tick();
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    JceGpuStats stats = {0};
    stats.valid = true;
    stats.cpu_frame_ms = 2.5;
    stats.gpu_ms = 0.125;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 42, 3.75, &stats, true));
    TEST_ASSERT_EQUAL_UINT(1, sink.calls);
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "42,frame_wall,3.7500\n"));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "42,frame_cpu,2.5000\n"));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "42,frame_gpu,0.1250\n"));
    for (unsigned index = 0; index < 48; ++index)
        TEST_ASSERT_NOT_NULL(strstr(sink.bytes, names[index]));
    TEST_ASSERT_GREATER_OR_EQUAL_UINT(16384, buffer.capacity);
    char *bytes = buffer.bytes;
    size_t capacity = buffer.capacity;
    sink.size = sink.calls = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 43, 4, NULL, true));
    TEST_ASSERT_EQUAL_UINT(1, sink.calls);
    TEST_ASSERT_EQUAL_PTR(bytes, buffer.bytes);
    TEST_ASSERT_EQUAL_UINT(capacity, buffer.capacity);
    TEST_ASSERT_NULL(strstr(sink.bytes, "frame_cpu"));
    TEST_ASSERT_TRUE(SDL_CloseIO(stream));
    jce_frame_kpi_buffer_destroy(&buffer);
    TEST_ASSERT_NULL(buffer.bytes);
    TEST_ASSERT_EQUAL_UINT(0, buffer.capacity);
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_failed_stream_and_invalid_clock_fail_closed(void)
{
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    sink.fail = true;
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_FALSE(jce_frame_kpi_export(&buffer, stream, 44, 1, NULL, true));
    TEST_ASSERT_EQUAL_UINT(0, sink.size);
    sink.fail = false;
    sink.calls = 0;
    TEST_ASSERT_FALSE(jce_frame_kpi_export(&buffer, stream, 45, -1, NULL, true));
    TEST_ASSERT_EQUAL_UINT(0, sink.calls);
    TEST_ASSERT_TRUE(SDL_CloseIO(stream));
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_csv_delimiters_cannot_create_forged_rows(void)
{
    jce_perf_phase_add("bad\n0,frame_wall,0", 1);
    jce_perf_phase_frame_tick();
    JceFrameKpiBuffer buffer = {0};
    Sink sink = {0};
    SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_NOT_NULL(stream);
    TEST_ASSERT_FALSE(jce_frame_kpi_export(&buffer, stream, 46, 1, NULL, true));
    TEST_ASSERT_EQUAL_UINT(0, sink.calls);
    TEST_ASSERT_TRUE(SDL_CloseIO(stream));
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_discontinuous_interval_omits_stale_phases_then_keeps_fresh_zero(void)
{
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, false, 4.0); /* No perf log; raw continuity remains active. */
    jce_engine_perf_hitch_begin_interval(&state);
    jce_engine_perf_hitch_end_interval(&state, true);
    jce_perf_phase_add("stale_render", 90.0); jce_perf_phase_frame_tick();
    jce_engine_perf_hitch_begin_interval(&state);
    jce_engine_perf_hitch_end_interval(&state, false); /* skipped iteration */
    jce_engine_perf_hitch_begin_interval(&state);
    TEST_ASSERT_FALSE(state.interval_eligible);
    TEST_ASSERT_TRUE(jce_perf_phase_frame_has_data());
    JceFrameKpiBuffer buffer = {0}; Sink sink = {0}; SDL_IOStream *stream = stream_for(&sink);
    JceGpuStats stats = {0}; stats.valid = true; stats.cpu_frame_ms = 2.5; stats.gpu_ms = 0.5;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 11, 100.0, &stats, state.interval_eligible));
    TEST_ASSERT_EQUAL_STRING("11,frame_wall,100.0000\n11,frame_cpu,2.5000\n11,frame_gpu,0.5000\n", sink.bytes);
    jce_engine_perf_hitch_end_interval(&state, true);
    jce_perf_phase_add("fresh_zero", 0.0); jce_perf_phase_frame_tick();
    jce_engine_perf_hitch_begin_interval(&state);
    TEST_ASSERT_TRUE(state.interval_eligible);
    sink.size = sink.calls = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 12, 1.0, NULL, state.interval_eligible));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "12,fresh_zero,0.0000\n"));
    TEST_ASSERT_NULL(strstr(sink.bytes, "stale_render,90.0000"));
    TEST_ASSERT_TRUE(SDL_CloseIO(stream)); jce_frame_kpi_buffer_destroy(&buffer);
    jce_engine_perf_hitch_end_interval(&state, true);
}


static void test_phase_reenable_omits_stale_then_exports_fresh_zero_wall(void)
{
    JceEnginePerfHitch state;
    jce_engine_perf_hitch_init(&state, false, 4.0);
    jce_perf_phase_add("gate_old_snapshot", 90.0); jce_perf_phase_frame_tick();
    jce_perf_phase_set_enabled(0);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_engine_perf_hitch_end_interval(&state, true);
    jce_perf_phase_set_enabled(1);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick();
    TEST_ASSERT_FALSE(state.interval_eligible);
    JceFrameKpiBuffer buffer = {0}; Sink sink = {0}; SDL_IOStream *stream = stream_for(&sink);
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 13, 0.0, NULL, state.interval_eligible));
    TEST_ASSERT_EQUAL_STRING("13,frame_wall,0.0000\n", sink.bytes);
    jce_perf_phase_add("gate_fresh_zero", 0.0);
    jce_engine_perf_hitch_end_interval(&state, true);
    TEST_ASSERT_TRUE(jce_engine_perf_hitch_begin_interval(&state));
    jce_perf_phase_frame_tick();
    TEST_ASSERT_TRUE(state.interval_eligible);
    sink.size = sink.calls = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_export(&buffer, stream, 14, 0.0, NULL, state.interval_eligible));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "14,gate_fresh_zero,0.0000\n"));
    TEST_ASSERT_NOT_NULL(strstr(sink.bytes, "14,frame_wall,0.0000\n"));
    TEST_ASSERT_NULL(strstr(sink.bytes, "gate_old_snapshot,90.0000"));
    TEST_ASSERT_TRUE(SDL_CloseIO(stream)); jce_frame_kpi_buffer_destroy(&buffer);
    jce_engine_perf_hitch_end_interval(&state, true);
}

static JceFrameViewportObservation maximal_viewport(uint32_t id)
{
    JceFrameViewportObservation v = {0};
    v.struct_size = sizeof v; v.source_id = id; v.flags = 255;
    v.cpu_outcome = JCE_FRAME_VIEWPORT_CPU_COMPLETED; v.refusal_reason = UINT32_MAX;
    v.requested_width = v.requested_height = UINT32_MAX;
    v.allocated_width = v.allocated_height = UINT32_MAX;
    v.display_width = v.display_height = UINT32_MAX;
    v.texture_index = UINT16_MAX - 1;
    v.image_count = v.queued_commands = v.main_submits = v.recording_submits = UINT32_MAX;
    return v;
}

static void test_v2_maximum_four_observations_fit_existing_bounded_record(void)
{
    JceFrameKpiContext context = {0}; context.version = 2;
    context.window_present = true;
    context.flags_begin_valid = context.flags_end_valid = true;
    context.flags_begin = context.flags_end = UINT32_MAX;
    context.point_width = context.point_height = context.drawable_width = context.drawable_height = UINT32_MAX;
    context.native_width_begin = context.native_height_begin = UINT32_MAX;
    context.native_width_end = context.native_height_end = UINT32_MAX;
    context.backend_frame_begin_valid = context.backend_frame_end_valid = true;
    context.backend_frame_begin = context.backend_frame_end = UINT32_MAX;
    JceFrameViewportObservation viewports[4];
    for (unsigned i = 0; i < 4; ++i) viewports[i] = maximal_viewport(UINT32_MAX - i);
    TEST_ASSERT_EQUAL_UINT(64, sizeof viewports[0]);
    TEST_ASSERT_TRUE(jce_frame_kpi_context_report(&context, viewports, 4, sizeof viewports[0]));
    JceFrameKpiBuffer buffer = {0}; buffer.max_capacity = 4096;
    size_t size = 999;
    bool encoded = jce_frame_kpi_encode_with_context(&buffer, UINT64_MAX, 1.0, NULL, false, &context, &size);
    TEST_ASSERT_TRUE(encoded); TEST_ASSERT_LESS_THAN_UINT(4096, size);
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"v\"\":2"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"source_id\"\":4294967292"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"recording_submits\"\":4294967295"));
    TEST_ASSERT_NULL(strstr(buffer.bytes, ":null"));
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_v2_zero_unknown_future_prefix_and_failed_copy_are_distinct(void)
{
    struct FutureViewport { JceFrameViewportObservation value; uint32_t tail[2]; } future[2] = {0};
    for (unsigned i = 0; i < 2; ++i) {
        future[i].value.struct_size = sizeof future[i]; future[i].value.source_id = i + 1;
        future[i].value.flags = JCE_FRAME_VIEWPORT_TEXTURE_VALID | JCE_FRAME_VIEWPORT_IMAGE_COUNT_VALID |
            JCE_FRAME_VIEWPORT_QUEUE_COUNT_VALID | JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID;
        future[i].tail[0] = UINT32_MAX;
    }
    JceFrameKpiContext context = {0}; context.version = 2;
    uint32_t short_prefix = sizeof short_prefix;
    TEST_ASSERT_FALSE(jce_frame_kpi_context_report(&context,
        (const JceFrameViewportObservation *)&short_prefix, 1, sizeof(JceFrameViewportObservation)));
    TEST_ASSERT_FALSE(jce_frame_kpi_context_report(&context, NULL, 0, 1));
    TEST_ASSERT_FALSE(context.viewport_reported);
    TEST_ASSERT_TRUE(jce_frame_kpi_context_report(&context, &future[0].value, 2, sizeof future[0]));
    future[0].value.source_id = 90;
    TEST_ASSERT_EQUAL_UINT32(1, context.viewports[0].source_id);
    TEST_ASSERT_EQUAL_UINT32(sizeof(JceFrameViewportObservation), context.viewports[0].struct_size);
    JceFrameKpiContext saved = context;
    TEST_ASSERT_FALSE(jce_frame_kpi_context_report(&context, NULL, 0, 0));
    TEST_ASSERT_EQUAL_MEMORY(&saved, &context, sizeof saved);
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 0, NULL, false, &context, &size));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"requested_size\"\":null"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"texture_index\"\":0,\"\"image_count\"\":0"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"queued_commands\"\":0,\"\"main_submits\"\":0,\"\"recording_submits\"\":0"));
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_v2_invalid_internal_packets_fail_without_publishing_partial_bytes(void)
{
    for (unsigned mutation = 0; mutation < 9; ++mutation) {
        JceFrameKpiContext context = {0}; context.version = 2;
        context.viewport_reported = true; context.viewport_count = 1;
        context.viewports[0] = maximal_viewport(1);
        switch (mutation) {
        case 0: context.version = 4; break;
        case 1: context.viewport_count = 5; break;
        case 2: context.viewport_reported = false; break;
        case 3: context.viewports[0].flags = 256; break;
        case 4: context.viewports[0].cpu_outcome = 4; break;
        case 5: context.viewports[0].requested_height = 0; break;
        case 6: context.viewports[0].texture_index = UINT16_MAX; break;
        case 7: context.viewports[0].flags &= ~JCE_FRAME_VIEWPORT_SUBMIT_COUNTS_VALID; break;
        default: context.viewport_count = 2; context.viewports[1] = context.viewports[0]; break;
        }
        JceFrameKpiBuffer buffer = {0}; size_t size = 99;
        bool encoded = jce_frame_kpi_encode_with_context(&buffer, 0, 1.0, NULL, false, &context, &size);
        jce_frame_kpi_buffer_destroy(&buffer);
        TEST_ASSERT_FALSE(encoded); TEST_ASSERT_EQUAL_UINT(0, size);
    }
    JceFrameKpiContext context = {0}; context.version = 2;
    JceFrameKpiBuffer buffer = {0}; buffer.max_capacity = 32; size_t size = 99;
    TEST_ASSERT_FALSE(jce_frame_kpi_encode_with_context(&buffer, 0, 1.0, NULL, false, &context, &size));
    TEST_ASSERT_EQUAL_UINT(0, size); jce_frame_kpi_buffer_destroy(&buffer);
}


static void test_v3_clock_exact_u64_zero_rounding_and_legacy_bytes(void)
{
    JceFrameKpiBuffer buffer = {0}; size_t size = 0;
    JceFrameKpiContext context = {0}; context.version = 3; context.clock_valid = true;
    context.clock_frequency = UINT64_MAX;
    context.clock_begin = context.clock_end = UINT64_MAX;
    TEST_ASSERT_TRUE(jce_frame_kpi_context_report(&context, NULL, 0, 0));
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 0.0, NULL, false, &context, &size));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes,
        "\"\"v\"\":3,\"\"window_present\"\":false"));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes,
        "\"\"viewport_reported\"\":true,\"\"viewports\"\":[],\"\"clock\"\":{"
        "\"\"domain\"\":\"\"jce-performance-counter\"\","
        "\"\"frequency_hz\"\":\"\"18446744073709551615\"\","
        "\"\"begin\"\":\"\"18446744073709551615\"\","
        "\"\"end\"\":\"\"18446744073709551615\"\"}"));
    context.clock_begin = context.clock_end = 0; context.clock_frequency = 3;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 0.0, NULL, false, &context, &size));
    TEST_ASSERT_NOT_NULL(strstr(buffer.bytes, "\"\"begin\"\":\"\"0\"\",\"\"end\"\":\"\"0\"\""));
    context.clock_end = 1;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 333.3333, NULL, false, &context, &size));
    /* V2 ignores the new private clock fields and preserves its exact bytes. */
    context.version = 2; context.clock_valid = false; context.clock_frequency = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, 0, 1.0, NULL, false, &context, &size));
    TEST_ASSERT_EQUAL_STRING(
        "0,frame_wall,1.0000,\"{\"\"v\"\":2,\"\"window_present\"\":false,"
        "\"\"flags_begin\"\":null,\"\"flags_end\"\":null,\"\"point_size\"\":null,"
        "\"\"drawable_size\"\":null,\"\"native_size_begin\"\":null,\"\"native_size_end\"\":null,"
        "\"\"backend_frame_begin\"\":null,\"\"backend_frame_end\"\":null,"
        "\"\"viewport_reported\"\":true,\"\"viewports\"\":[]}\"\n", buffer.bytes);
    jce_frame_kpi_buffer_destroy(&buffer);
}

static void test_v3_invalid_clock_and_duration_fail_without_partial_success(void)
{
    for (unsigned mutation = 0; mutation < 6; ++mutation) {
        JceFrameKpiContext context = {0}; context.version = 3;
        context.clock_valid = true; context.clock_frequency = 1000;
        context.clock_begin = 1; context.clock_end = 2;
        double wall = 1.0;
        switch (mutation) {
        case 0: context.clock_valid = false; break;
        case 1: context.clock_frequency = 0; break;
        case 2: context.clock_end = 0; break;
        case 3: wall = 2.0; break;
        case 4: wall = NAN; break;
        default: wall = -1.0; break;
        }
        JceFrameKpiBuffer buffer = {0}; size_t size = 99;
        bool encoded = jce_frame_kpi_encode_with_context(&buffer, 0, wall, NULL, false, &context, &size);
        jce_frame_kpi_buffer_destroy(&buffer);
        TEST_ASSERT_FALSE(encoded); TEST_ASSERT_EQUAL_UINT(0, size);
    }
}

static void test_v3_maximum_four_viewports_fit_unchanged_record_budget(void)
{
    JceFrameKpiContext context = {0}; context.version = 3; context.window_present = true;
    context.flags_begin_valid = context.flags_end_valid = true;
    context.flags_begin = context.flags_end = UINT32_MAX;
    context.point_width = context.point_height = context.drawable_width = context.drawable_height = UINT32_MAX;
    context.native_width_begin = context.native_height_begin = UINT32_MAX;
    context.native_width_end = context.native_height_end = UINT32_MAX;
    context.backend_frame_begin_valid = context.backend_frame_end_valid = true;
    context.backend_frame_begin = context.backend_frame_end = UINT32_MAX;
    context.clock_valid = true; context.clock_frequency = UINT64_MAX;
    context.clock_begin = 0; context.clock_end = UINT64_MAX;
    JceFrameViewportObservation viewports[4];
    for (unsigned i = 0; i < 4; ++i) viewports[i] = maximal_viewport(UINT32_MAX - i);
    TEST_ASSERT_TRUE(jce_frame_kpi_context_report(&context, viewports, 4, sizeof viewports[0]));
    JceFrameKpiBuffer buffer = {0}; buffer.max_capacity = 4096; size_t size = 0;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, UINT64_MAX, 1000.0,
        NULL, false, &context, &size));
    TEST_ASSERT_LESS_THAN_UINT(4096, size);
    const size_t exact_capacity = size + 1;
    buffer.max_capacity = buffer.capacity = exact_capacity;
    TEST_ASSERT_TRUE(jce_frame_kpi_encode_with_context(&buffer, UINT64_MAX, 1000.0,
        NULL, false, &context, &size));
    buffer.max_capacity = buffer.capacity = exact_capacity - 1;
    TEST_ASSERT_FALSE(jce_frame_kpi_encode_with_context(&buffer, UINT64_MAX, 1000.0,
        NULL, false, &context, &size));
    TEST_ASSERT_EQUAL_UINT(0, size);
    jce_frame_kpi_buffer_destroy(&buffer);
}

int main(void)
{
    UNITY_BEGIN();
    RUN_TEST(test_sampled_zero_snapshot_exports_zero_row);
    RUN_TEST(test_backend_latched_opt_in_preserves_default_bytes_and_zero);
    RUN_TEST(test_backend_latched_missing_and_invalid_timers_remain_unavailable);
    RUN_TEST(test_backend_latched_rows_keep_complete_record_budget_and_single_write);
    RUN_TEST(test_context_header_and_exact_typed_csv_preserve_legacy_bytes);
    RUN_TEST(test_context_unknowns_are_null_and_nonwall_fields_are_empty);
    RUN_TEST(test_context_record_obeys_scratch_budget_without_partial_success);
    RUN_TEST(test_growth_complete_rows_and_reuse);
    RUN_TEST(test_failed_stream_and_invalid_clock_fail_closed);
    RUN_TEST(test_discontinuous_interval_omits_stale_phases_then_keeps_fresh_zero);
    RUN_TEST(test_phase_reenable_omits_stale_then_exports_fresh_zero_wall);
    RUN_TEST(test_csv_delimiters_cannot_create_forged_rows);
    RUN_TEST(test_v2_maximum_four_observations_fit_existing_bounded_record);
    RUN_TEST(test_v2_zero_unknown_future_prefix_and_failed_copy_are_distinct);
    RUN_TEST(test_v2_invalid_internal_packets_fail_without_publishing_partial_bytes);
    RUN_TEST(test_v3_clock_exact_u64_zero_rounding_and_legacy_bytes);
    RUN_TEST(test_v3_invalid_clock_and_duration_fail_without_partial_success);
    RUN_TEST(test_v3_maximum_four_viewports_fit_unchanged_record_budget);
    return UNITY_END();
}
