/* Sample existing SDK counters; definitions remain engine-owned. */
#include "metrics.h"
#include <jce/api_core.h>
#include <jce/api_platform.h>
#include <jce/api_introspect.h>
#include <inttypes.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define LOG_TAG "benchmark_player"

typedef struct BenchMetricsObservation {
    JceGpuStats gpu;
    bool gpu_available;
    JceGpuTimingObservation gpu_timing;
    bool gpu_timing_available;
    const char *frame_source;
    bool window_available;
    bool window_flags_available;
    uint32_t window_flags;
    uint32_t drawable_width;
    uint32_t drawable_height;
    int logical_width;
    int logical_height;
    bool fullscreen;
    unsigned frame;
    double simulation_seconds;
    const char *event;
    uint64_t monotonic_ms;
    uint64_t perf_counter_before;
    uint64_t perf_counter_after;
    uint64_t perf_freq;
} BenchMetricsObservation;

static void json_null(JceJson *object, const char *key)
{
    /* The public JSON facade has no null constructor. */
    jce_json_set_child(object, key, jce_json_parse_strict("null", 4u));
}

static void optional_number(JceJson *object, const char *key, bool available, double value)
{
    if (available) jce_json_set_number(object, key, value);
    else json_null(object, key);
}

static void optional_bytes(JceJson *object, const char *key, const char *available,
                           bool stats_available, int64_t value)
{
    const bool reported = stats_available && value >= 0;
    jce_json_set_bool(object, available, reported);
    optional_number(object, key, reported, (double)value);
}

static void exact_counter(JceJson *object, const char *key, uint64_t value)
{
    char decimal[21];
    snprintf(decimal, sizeof(decimal), "%" PRIu64, value);
    jce_json_set_string(object, key, decimal);
}

static void timing_value(JceJson *object, const char *key, bool available, double value)
{
    char state_key[64];
    snprintf(state_key, sizeof(state_key), "%s_state", key);
    if (!available) {
        json_null(object, key);
        jce_json_set_string(object, state_key, "UNAVAILABLE");
    } else if (!isfinite(value)) {
        char raw_key[64];
        snprintf(raw_key, sizeof(raw_key), "%s_raw", key);
        json_null(object, key);
        jce_json_set_string(object, state_key, "NON_FINITE");
        jce_json_set_string(object, raw_key,
                            isnan(value) ? "NaN" : (signbit(value) ? "-Infinity" : "Infinity"));
    } else {
        jce_json_set_number(object, key, value);
        jce_json_set_string(object, state_key,
                            value < 0 ? "NEGATIVE" : (value == 0 ? "FINITE_ZERO" : "FINITE_POSITIVE"));
    }
}

/* Signed raw ticks remain decimal strings, including values beyond 2^53. */
static void exact_signed_counter(JceJson *object,const char *key,int64_t value)
{
    char decimal[21];snprintf(decimal,sizeof(decimal),"%" PRId64,value);
    jce_json_set_string(object,key,decimal);
}
static const char *gpu_timing_reason(JceGpuTimingUnavailableReason reason)
{
    switch(reason) {
    case JCE_GPU_TIMING_AVAILABLE:return "AVAILABLE";
    case JCE_GPU_TIMING_CONTEXT_UNAVAILABLE:return "CONTEXT_UNAVAILABLE";
    case JCE_GPU_TIMING_BACKEND_UNAVAILABLE:return "BACKEND_UNAVAILABLE";
    case JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE:return "FREQUENCY_UNAVAILABLE";
    case JCE_GPU_TIMING_INTERVAL_UNAVAILABLE:return "INTERVAL_UNAVAILABLE";
    default:return "UNKNOWN_REASON";
    }
}
static JceJson *gpu_timing_record(const BenchMetricsObservation *observation)
{
    JceJson *object=jce_json_object();if(!object)return NULL;
    const JceGpuTimingObservation *sample=&observation->gpu_timing;
    const bool observed=observation->gpu_timing_available;
    jce_json_set_string(object,"source","jce_renderer_get_gpu_timing_observation");
    jce_json_set_string(object,"scope","latched_backend_interval_not_engine_frame_join_or_completion");
    jce_json_set_bool(object,"observation_available",observed);
    optional_number(object,"version",observed,sample->version);
    optional_number(object,"struct_size",observed,sample->struct_size);
    jce_json_set_bool(object,"context_available",observed && sample->context_available);
    jce_json_set_bool(object,"backend_available",observed && sample->backend_available);
    jce_json_set_bool(object,"timer_available",observed && sample->timer_available);
    optional_number(object,"unavailable_reason",observed,sample->unavailable_reason);
    jce_json_set_string(object,"unavailable_reason_name",observed ? gpu_timing_reason(sample->unavailable_reason) : "GETTER_UNAVAILABLE");
    if(observed && sample->context_available) exact_counter(object,"context_generation",sample->context_generation);
    else json_null(object,"context_generation");
    optional_number(object,"read_host_frame_index",observed && sample->context_available,sample->read_host_frame_index);
    optional_number(object,"gpu_source_frame",observed && sample->backend_available,sample->gpu_source_frame);
    if(observed && sample->backend_available) {
        exact_signed_counter(object,"timer_frequency",sample->timer_frequency);
        exact_signed_counter(object,"raw_begin",sample->raw_begin);
        exact_signed_counter(object,"raw_end",sample->raw_end);
    } else {
        json_null(object,"timer_frequency");json_null(object,"raw_begin");json_null(object,"raw_end");
    }
    timing_value(object,"duration_ms",observed && sample->timer_available,sample->duration_ms);
    jce_json_set_bool(object,"exact_engine_frame_join",false);
    jce_json_set_bool(object,"same_row_gpu_completion_observed",false);
    return object;
}

static void sample_window(const JceServices *services, BenchMetricsObservation *observation)
{
    observation->window_available = services && services->window;
    observation->window_flags_available = false;
    if (!observation->window_available) return;
    jce_window_get_size(services->window, &observation->drawable_width,
                        &observation->drawable_height);
    jce_window_get_logical(services->window, &observation->logical_width,
                           &observation->logical_height);
    observation->fullscreen = jce_window_is_fullscreen(services->window);
    observation->window_flags_available = jce_window_get_flags(services->window,
                                                              &observation->window_flags);
}

/* Pure encoding shared by ordinary JSONL records and the private self-test.
 * The clocks bracket the CPU read, not collection or completion of a GPU frame. */
static char *serialize_record(JceJson *root, const BenchMetricsObservation *observation)
{
    if (!root || !observation) return NULL;
    const bool available = observation->gpu_available && observation->gpu.valid;
    const JceGpuStats *gpu = &observation->gpu;
    JceJson *counters = jce_json_object();
    JceJson *window = jce_json_object();
    JceJson *timing = gpu_timing_record(observation);
    if (!counters || !window || !timing) {
        jce_json_free(counters);
        jce_json_free(window);
        jce_json_free(timing);
        return NULL;
    }
    jce_json_set_number(root, "frame", observation->frame);
    optional_number(root,"simulation_seconds",isfinite(observation->simulation_seconds),observation->simulation_seconds);
    jce_json_set_number(root, "monotonic_ms", (double)observation->monotonic_ms);
    jce_json_set_string(root, "event", observation->event);
    jce_json_set_string(root, "observation_event", observation->event);
    jce_json_set_string(root,"observation_frame_source",observation->frame_source ? observation->frame_source : "benchmark_update_count");
    jce_json_set_bool(root, "backend_frame_index_available", false);
    json_null(root, "backend_frame_index");
    jce_json_set_string(root, "gpu_stats_source", "latest_asynchronous_backend_stats");
    jce_json_set_bool(root, "same_row_gpu_completion_observed", false);
    jce_json_set_string(root, "observation_clock_source", "jce_time_perf_counter");
    jce_json_set_string(root, "observation_clock_scope",
                        "CPU read of public GPU stats and timing observation; not backend collection time");
    jce_json_set_string(root, "monotonic_ms_source",
                        "observation_time_not_backend_collection_time");
    exact_counter(root, "observation_perf_counter_before", observation->perf_counter_before);
    exact_counter(root, "observation_perf_counter_after", observation->perf_counter_after);
    exact_counter(root, "observation_perf_freq", observation->perf_freq);
    jce_json_set_bool(root, "gpu_getter_succeeded", observation->gpu_available);
    jce_json_set_bool(root, "gpu_available", available);
    if (observation->gpu_available) jce_json_set_bool(counters, "valid", gpu->valid);
    else json_null(counters, "valid");
    /* Preserve unqualified legacy numeric stats. Only the independent
     * gpu_timing object carries timer availability and a qualified interval. */
    jce_json_set_string(counters, "timing_support", "UNQUALIFIED_LEGACY_GPU_STATS");
    jce_json_set_string(counters, "timing_qualification_scope",
                        "numeric state only; no timing support or wait improvement inference");
    timing_value(counters, "cpu_frame_ms", available, gpu->cpu_frame_ms);
    timing_value(counters, "cpu_submit_ms", available, gpu->cpu_submit_ms);
    timing_value(counters, "gpu_ms", available, gpu->gpu_ms);
    timing_value(counters, "wait_submit_ms", available, gpu->wait_submit_ms);
    timing_value(counters, "wait_render_ms", available, gpu->wait_render_ms);
    optional_number(counters, "draws", available, gpu->num_draw);
    optional_number(counters, "textures", available, gpu->num_textures);
    optional_number(counters, "frame_buffers", available, gpu->num_frame_buffers);
    optional_number(counters, "programs", available, gpu->num_programs);
    optional_number(counters, "shaders", available, gpu->num_shaders);
    optional_number(counters, "max_gpu_latency", available, gpu->max_gpu_latency);
    optional_number(counters, "backbuffer_width", available, gpu->backbuffer_width);
    optional_number(counters, "backbuffer_height", available, gpu->backbuffer_height);
    jce_json_set_string(counters, "backbuffer_size_source", "latest_asynchronous_backend_stats");
    jce_json_set_string(counters, "gpu_memory_scope",
                        "backend-reported; may be adapter-wide, not process-owned live allocations");
    optional_bytes(counters, "render_target_bytes", "render_target_bytes_available",
                    available, gpu->rt_memory_used);
    optional_bytes(counters, "texture_bytes", "texture_bytes_available",
                    available, gpu->texture_memory_used);
    optional_bytes(counters, "gpu_bytes", "gpu_bytes_available", available, gpu->gpu_memory_used);
    jce_json_set_child(root, "gpu", counters);
    jce_json_set_child(root,"gpu_timing",timing);

    jce_json_set_bool(window, "actual_available", observation->window_available);
    optional_number(window, "drawable_width", observation->window_available,
                     observation->drawable_width);
    optional_number(window, "drawable_height", observation->window_available,
                     observation->drawable_height);
    optional_number(window, "logical_width", observation->window_available,
                     observation->logical_width);
    optional_number(window, "logical_height", observation->window_available,
                     observation->logical_height);
    jce_json_set_string(window, "drawable_size_source", "jce_window_get_size_cached_drawable_pixels");
    jce_json_set_string(window, "logical_size_source", "jce_window_get_logical_cached_logical_units");
    jce_json_set_bool(window, "fullscreen_actual_available", observation->window_available);
    if (observation->window_available)
        jce_json_set_bool(window, "fullscreen_actual", observation->fullscreen);
    else json_null(window, "fullscreen_actual");
    jce_json_set_string(window, "fullscreen_actual_source", "jce_window_is_fullscreen_platform_flags");
    const bool flags_available = observation->window_available &&
                                  observation->window_flags_available;
    jce_json_set_bool(window, "flags_actual_available", flags_available);
    optional_number(window, "flags_actual", flags_available, observation->window_flags);
    jce_json_set_string(window, "flags_actual_source", "jce_window_get_flags_platform_flags");
    jce_json_set_string(window, "flags_actual_scope",
        "Supported platform window bits read after GPU stats within this collector call; "
        "not OS foreground ownership, physical visibility or presentation completion");
    static const char *const state_names[] = {
        "hidden_actual", "focused_actual", "maximized_actual", "not_focusable_actual"
    };
    static const uint32_t state_bits[] = {
        JCE_WINDOW_HIDDEN, JCE_WINDOW_INPUT_FOCUS,
        JCE_WINDOW_MAXIMIZED, JCE_WINDOW_NOT_FOCUSABLE
    };
    for (size_t i = 0; i < sizeof(state_bits) / sizeof(state_bits[0]); ++i) {
        char key[64];
        snprintf(key, sizeof(key), "%s_available", state_names[i]);
        jce_json_set_bool(window, key, flags_available);
        if (flags_available)
            jce_json_set_bool(window, state_names[i],
                              (observation->window_flags & state_bits[i]) != 0u);
        else json_null(window, state_names[i]);
        snprintf(key, sizeof(key), "%s_source", state_names[i]);
        jce_json_set_string(window, key, "jce_window_get_flags_platform_flags");
    }
    jce_json_set_child(root, "window", window);
    return jce_json_print(root, false);
}

static JceJson *self_test_record(const BenchMetricsObservation *observation)
{
    JceJson *root = jce_json_object();
    char *text = serialize_record(root, observation);
    /* Inspect the production wire representation, not the pre-print tree. */
    JceJson *decoded = text ? jce_json_parse_strict(text, strlen(text)) : NULL;
    jce_json_free_string(text);
    jce_json_free(root);
    return decoded;
}

static bool self_test_string(const JceJson *object, const char *key, const char *expected)
{
    const JceJson *value = jce_json_get(object, key);
    const char *text = jce_json_string_value(value, NULL);
    return jce_json_is_string(value) && text && strcmp(text, expected) == 0;
}

static bool self_test_number(const JceJson *object, const char *key, double expected)
{
    const JceJson *value = jce_json_get(object, key);
    return jce_json_is_number(value) && jce_json_number_value(value, NAN) == expected;
}

static bool self_test_bool(const JceJson *object, const char *key, bool expected)
{
    const JceJson *value = jce_json_get(object, key);
    return jce_json_is_bool(value) && jce_json_bool_value(value, !expected) == expected;
}

static bool self_test_group(const char *name, bool passed)
{
    if (passed) LOG_INFO(LOG_TAG, "benchmark-metrics-selftest: group=%s result=PASS", name);
    else LOG_ERROR(LOG_TAG, "benchmark-metrics-selftest: group=%s result=FAIL", name);
    return passed;
}

static bool self_test_null_window(const JceJson *root)
{
    const JceJson *window = jce_json_get(root, "window");
    return jce_json_is_object(window) &&
           self_test_bool(window, "actual_available", false) &&
           jce_json_is_null(jce_json_get(window, "drawable_width")) &&
           jce_json_is_null(jce_json_get(window, "drawable_height")) &&
           jce_json_is_null(jce_json_get(window, "logical_width")) &&
           jce_json_is_null(jce_json_get(window, "logical_height")) &&
           self_test_bool(window, "fullscreen_actual_available", false) &&
           jce_json_is_null(jce_json_get(window, "fullscreen_actual")) &&
           self_test_bool(window, "hidden_actual_available", false) &&
           jce_json_is_null(jce_json_get(window, "hidden_actual")) &&
           self_test_bool(window, "focused_actual_available", false) &&
           jce_json_is_null(jce_json_get(window, "focused_actual"));
}

bool bench_metrics_self_test(void)
{
    static const char *timings[] = {
        "cpu_frame_ms", "cpu_submit_ms", "gpu_ms", "wait_submit_ms", "wait_render_ms"
    };
    BenchMetricsObservation observation = {0};
    observation.event = "selftest";
    observation.gpu.valid = true;
    observation.gpu.wait_submit_ms = -9.0; /* Unavailable poison must not leak. */
    observation.gpu.num_draw = 99;
    JceJson *root = self_test_record(&observation);
    const JceJson *gpu = jce_json_get(root, "gpu");
    bool passed = root && self_test_bool(root, "gpu_available", false) &&
                  self_test_bool(root, "gpu_getter_succeeded", false) &&
                  jce_json_is_null(jce_json_get(gpu, "valid")) &&
                  jce_json_is_null(jce_json_get(gpu, "draws")) &&
                  jce_json_is_null(jce_json_get(gpu, "textures")) &&
                  jce_json_is_null(jce_json_get(gpu, "max_gpu_latency")) &&
                  jce_json_is_null(jce_json_get(gpu, "backbuffer_width")) &&
                  jce_json_is_null(jce_json_get(gpu, "backbuffer_height"));
    for (size_t i = 0; i < sizeof(timings) / sizeof(timings[0]); ++i) {
        char state_key[64];
        snprintf(state_key, sizeof(state_key), "%s_state", timings[i]);
        passed = jce_json_is_null(jce_json_get(gpu, timings[i])) &&
                 self_test_string(gpu, state_key, "UNAVAILABLE") && passed;
    }
    bool all_passed = self_test_group("backend-unavailable", passed);
    jce_json_free(root);

    memset(&observation.gpu, 0, sizeof(observation.gpu));
    observation.gpu_available = true;
    observation.gpu.valid = true;
    observation.window_available = true;
    root = self_test_record(&observation);
    gpu = jce_json_get(root, "gpu");
    const JceJson *window = jce_json_get(root, "window");
    passed = root && self_test_bool(root, "gpu_available", true) &&
             self_test_bool(gpu, "valid", true) &&
             self_test_string(gpu, "timing_support", "UNQUALIFIED_LEGACY_GPU_STATS") &&
             self_test_number(gpu, "draws", 0) &&
             self_test_number(gpu, "max_gpu_latency", 0) &&
             self_test_number(gpu, "backbuffer_width", 0) &&
             self_test_number(gpu, "backbuffer_height", 0) &&
             self_test_bool(window, "actual_available", true) &&
             self_test_number(window, "drawable_width", 0) &&
             self_test_bool(window, "fullscreen_actual_available", true) &&
             self_test_bool(window, "fullscreen_actual", false);
    for (size_t i = 0; i < sizeof(timings) / sizeof(timings[0]); ++i) {
        char state_key[64];
        snprintf(state_key, sizeof(state_key), "%s_state", timings[i]);
        passed = self_test_number(gpu, timings[i], 0) &&
                 self_test_string(gpu, state_key, "FINITE_ZERO") && passed;
    }
    all_passed = self_test_group("valid-all-zero", passed) && all_passed;
    jce_json_free(root);

    observation.gpu.cpu_frame_ms = NAN;
    observation.gpu.cpu_submit_ms = -1.25;
    observation.gpu.gpu_ms = INFINITY;
    observation.gpu.wait_submit_ms = -2.5;
    observation.gpu.wait_render_ms = -INFINITY;
    observation.gpu.num_draw = 7;
    root = self_test_record(&observation);
    gpu = jce_json_get(root, "gpu");
    passed = root && jce_json_is_null(jce_json_get(gpu, "cpu_frame_ms")) &&
             self_test_string(gpu, "cpu_frame_ms_state", "NON_FINITE") &&
             self_test_string(gpu, "cpu_frame_ms_raw", "NaN") &&
             self_test_number(gpu, "cpu_submit_ms", -1.25) &&
             self_test_string(gpu, "cpu_submit_ms_state", "NEGATIVE") &&
             jce_json_is_null(jce_json_get(gpu, "gpu_ms")) &&
             self_test_string(gpu, "gpu_ms_state", "NON_FINITE") &&
             self_test_string(gpu, "gpu_ms_raw", "Infinity") &&
             self_test_number(gpu, "wait_submit_ms", -2.5) &&
             self_test_string(gpu, "wait_submit_ms_state", "NEGATIVE") &&
             jce_json_is_null(jce_json_get(gpu, "wait_render_ms")) &&
             self_test_string(gpu, "wait_render_ms_state", "NON_FINITE") &&
             self_test_string(gpu, "wait_render_ms_raw", "-Infinity") &&
             self_test_number(gpu, "draws", 7) &&
             self_test_string(gpu, "timing_support", "UNQUALIFIED_LEGACY_GPU_STATS");
    all_passed = self_test_group("nan-negative", passed) && all_passed;
    jce_json_free(root);

    observation.drawable_width = 42;
    observation.logical_width = 42;
    observation.fullscreen = true;
    sample_window(NULL, &observation);
    root = self_test_record(&observation);
    passed = root && self_test_null_window(root);
    jce_json_free(root);
    JceServices empty_services = {0};
    sample_window(&empty_services, &observation);
    root = self_test_record(&observation);
    passed = root && self_test_null_window(root) && passed;
    all_passed = self_test_group("null-window", passed) && all_passed;
    jce_json_free(root);

    /* Valid zero and observed state are distinct from an unavailable getter. */
    observation.window_available = true;
    observation.window_flags_available = true;
    observation.window_flags = 0u;
    root = self_test_record(&observation);
    window = root ? jce_json_get(root, "window") : NULL;
    passed = root && self_test_bool(window, "flags_actual_available", true) &&
        self_test_number(window, "flags_actual", 0) &&
        self_test_bool(window, "hidden_actual_available", true) &&
        self_test_bool(window, "hidden_actual", false) &&
        self_test_bool(window, "focused_actual", false) &&
        self_test_bool(window, "maximized_actual", false) &&
        self_test_bool(window, "not_focusable_actual", false);
    jce_json_free(root);
    observation.window_flags = JCE_WINDOW_HIDDEN | JCE_WINDOW_MAXIMIZED |
                               JCE_WINDOW_NOT_FOCUSABLE;
    root = self_test_record(&observation);
    window = root ? jce_json_get(root, "window") : NULL;
    passed = root && self_test_number(window, "flags_actual", observation.window_flags) &&
        self_test_bool(window, "hidden_actual", true) &&
        self_test_bool(window, "focused_actual", false) &&
        self_test_bool(window, "maximized_actual", true) &&
        self_test_bool(window, "not_focusable_actual", true) && passed;
    jce_json_free(root);
    observation.window_flags = JCE_WINDOW_INPUT_FOCUS;
    root = self_test_record(&observation);
    window = root ? jce_json_get(root, "window") : NULL;
    passed = root && self_test_bool(window, "focused_actual", true) &&
        self_test_bool(window, "hidden_actual", false) && passed;
    jce_json_free(root);
    sample_window(NULL, &observation);
    root = self_test_record(&observation);
    window = root ? jce_json_get(root, "window") : NULL;
    passed = root && self_test_null_window(root) &&
        self_test_bool(window, "flags_actual_available", false) &&
        jce_json_is_null(jce_json_get(window, "flags_actual")) &&
        self_test_bool(window, "maximized_actual_available", false) &&
        jce_json_is_null(jce_json_get(window, "maximized_actual")) &&
        self_test_bool(window, "not_focusable_actual_available", false) &&
        jce_json_is_null(jce_json_get(window, "not_focusable_actual")) && passed;
    all_passed = self_test_group("window-flags", passed) && all_passed;
    jce_json_free(root);

    observation.frame = 23;
    observation.perf_counter_before = UINT64_C(9007199254740993);
    observation.perf_counter_after = UINT64_MAX;
    observation.perf_freq = UINT64_MAX;
    root = self_test_record(&observation);
    passed = root &&
             self_test_string(root, "observation_perf_counter_before", "9007199254740993") &&
             self_test_string(root, "observation_perf_counter_after", "18446744073709551615") &&
             self_test_string(root, "observation_perf_freq", "18446744073709551615") &&
             self_test_number(root, "frame", 23) &&
             self_test_string(root, "observation_event", "selftest") &&
             self_test_string(root, "observation_frame_source", "benchmark_update_count") &&
             self_test_bool(root, "backend_frame_index_available", false) &&
             jce_json_is_null(jce_json_get(root, "backend_frame_index")) &&
             self_test_string(root, "gpu_stats_source", "latest_asynchronous_backend_stats") &&
             self_test_bool(root, "same_row_gpu_completion_observed", false) &&
             self_test_string(root, "observation_clock_source", "jce_time_perf_counter") &&
             self_test_string(root, "monotonic_ms_source",
                               "observation_time_not_backend_collection_time");
    all_passed = self_test_group("exact-uint64", passed) && all_passed;
    jce_json_free(root);
    observation.gpu_timing_available=true;
    observation.gpu_timing.struct_size=sizeof(observation.gpu_timing);observation.gpu_timing.version=1;
    observation.gpu_timing.context_available=observation.gpu_timing.backend_available=true;
    observation.gpu_timing.timer_available=true;observation.gpu_timing.unavailable_reason=JCE_GPU_TIMING_AVAILABLE;
    observation.gpu_timing.context_generation=UINT64_MAX;observation.gpu_timing.read_host_frame_index=23;
    observation.gpu_timing.gpu_source_frame=17;observation.gpu_timing.timer_frequency=INT64_C(9007199254740993);
    observation.gpu_timing.raw_begin=INT64_MIN;observation.gpu_timing.raw_end=INT64_MAX;
    observation.gpu_timing.duration_ms=2.5;observation.frame_source="default_host_draw_count";
    observation.simulation_seconds=NAN;root=self_test_record(&observation);
    const JceJson *timing=root ? jce_json_get(root,"gpu_timing") : NULL;
    passed=root && self_test_bool(timing,"observation_available",true) &&
        self_test_bool(timing,"timer_available",true) && self_test_number(timing,"duration_ms",2.5) &&
        self_test_string(timing,"timer_frequency","9007199254740993") &&
        self_test_string(timing,"raw_begin","-9223372036854775808") && self_test_string(timing,"raw_end","9223372036854775807") &&
        self_test_string(timing,"context_generation","18446744073709551615") &&
        self_test_number(timing,"read_host_frame_index",23) && self_test_number(timing,"gpu_source_frame",17) &&
        self_test_string(root,"observation_frame_source","default_host_draw_count") &&
        jce_json_is_null(jce_json_get(root,"simulation_seconds")) && self_test_bool(timing,"exact_engine_frame_join",false) &&
        self_test_bool(timing,"same_row_gpu_completion_observed",false);
    all_passed=self_test_group("gpu-timing-exact-signed-identity",passed) && all_passed;jce_json_free(root);
    observation.gpu_timing.timer_available=false;observation.gpu_timing.unavailable_reason=JCE_GPU_TIMING_FREQUENCY_UNAVAILABLE;
    observation.gpu_timing.timer_frequency=0;observation.gpu_timing.duration_ms=99;root=self_test_record(&observation);
    timing=root ? jce_json_get(root,"gpu_timing") : NULL;
    passed=root && self_test_bool(timing,"timer_available",false) && self_test_string(timing,"timer_frequency","0") &&
        self_test_string(timing,"unavailable_reason_name","FREQUENCY_UNAVAILABLE") &&
        jce_json_is_null(jce_json_get(timing,"duration_ms")) && self_test_string(timing,"duration_ms_state","UNAVAILABLE");
    all_passed=self_test_group("gpu-timing-unavailable-not-zero",passed) && all_passed;jce_json_free(root);
    observation.gpu_timing_available=false;root=self_test_record(&observation);timing=root ? jce_json_get(root,"gpu_timing") : NULL;
    passed=root && self_test_bool(timing,"observation_available",false) &&
        self_test_string(timing,"unavailable_reason_name","GETTER_UNAVAILABLE") &&
        jce_json_is_null(jce_json_get(timing,"raw_begin")) && jce_json_is_null(jce_json_get(timing,"context_generation"));
    all_passed=self_test_group("gpu-timing-missing-getter",passed) && all_passed;jce_json_free(root);
    return all_passed;
}

static void renderer_counters(JceSceneRenderer *renderer, JceJson *root)
{
    JceSceneCullStats cull;
    jce_scene_renderer_get_cull_stats(renderer, &cull);
    JceJson *object = jce_json_object();
    jce_json_set_bool(object, "enabled", cull.enabled);
    jce_json_set_number(object, "candidates", cull.total);
    jce_json_set_number(object, "visible", cull.visible);
    jce_json_set_number(object, "culled", cull.culled);
    jce_json_set_number(object, "grid_objects", cull.grid_objects);
    jce_json_set_number(object, "inserted", cull.inserted);
    jce_json_set_number(object, "updated", cull.updated);
    jce_json_set_number(object, "removed", cull.removed);
    jce_json_set_number(object, "disabled_skipped", cull.disabled_skipped);
    jce_json_set_child(root, "entity_culling", object);

    JceSceneLodStats lod;
    jce_scene_renderer_get_lod_stats(renderer, &lod);
    object = jce_json_object();
    jce_json_set_bool(object, "enabled", lod.enabled);
    jce_json_set_number(object, "level_count", lod.level_count);
    jce_json_set_number(object, "culled", lod.culled);
    JceJson *levels = jce_json_array();
    for (int i = 0; i < JCE_SCENE_LOD_MAX_LEVELS; ++i)
        jce_json_array_push_number(levels, lod.picks[i]);
    jce_json_set_child(object, "picks", levels);
    jce_json_set_child(root, "model_lod", object);

    JceSceneGpuDrivenStats gpu;
    jce_scene_renderer_get_gpu_driven_stats(renderer, &gpu);
    object = jce_json_object();
    jce_json_set_string(object, "scope", "model batches; not foliage dispatches");
    jce_json_set_bool(object, "enabled", gpu.enabled);
    jce_json_set_bool(object, "active", gpu.active);
    jce_json_set_bool(object, "adaptive_bypass", gpu.adaptive_bypass);
    jce_json_set_bool(object, "forced", gpu.forced);
    jce_json_set_bool(object, "supported", gpu.supported);
    jce_json_set_bool(object, "indirect_supported", gpu.indirect_supported);
    jce_json_set_bool(object, "indirect_ready", gpu.indirect_ready);
    jce_json_set_bool(object, "dispatch_succeeded", gpu.dispatch_succeeded);
    jce_json_set_bool(object, "hiz_enabled", gpu.hiz_enabled);
    jce_json_set_number(object, "candidate_records", gpu.candidate_records);
    jce_json_set_number(object, "candidate_groups", gpu.candidate_groups);
    jce_json_set_number(object, "candidate_draws", gpu.candidate_draws);
    jce_json_set_number(object, "input_records", gpu.records);
    jce_json_set_number(object, "gpu_groups", gpu.gpu_groups);
    jce_json_set_number(object, "gpu_runs", gpu.gpu_runs);
    jce_json_set_number(object, "compute_dispatches", gpu.compute_dispatches);
    jce_json_set_number(object, "indirect_submits", gpu.indirect_submits);
    jce_json_set_number(object, "fixed_count_submits", gpu.fixed_count_submits);
    jce_json_set_number(object, "cpu_fallback_submits", gpu.cpu_fallback_submits);
    jce_json_set_number(object, "buffer_growths", gpu.buffer_growths);
    jce_json_set_number(object, "upload_calls", gpu.upload_calls);
    jce_json_set_number(object, "uploaded_bytes", (double)gpu.uploaded_bytes);
    jce_json_set_number(object, "sort_ms", gpu.sort_ms);
    jce_json_set_number(object, "prepare_ms", gpu.prepare_ms);
    jce_json_set_number(object, "flush_ms", gpu.flush_ms);
    jce_json_set_child(root, "model_gpu_driven", object);
}

static void metrics_sample(const JceServices *services, JceScene *scene,
                          JceSceneRenderer *renderer, unsigned frame,
                          double simulation_seconds, const char *event,const char *frame_source)
{
    const char *path = getenv("JCE_BENCH_COUNTERS");
    if (!path || !path[0]) return;
    char stack[16384];
    char *bytes = stack;
    size_t capacity = sizeof(stack);
    size_t needed = jce_introspect_stats_json(scene, bytes, capacity);
    if (needed >= capacity && needed < SIZE_MAX - 1024u) {
        capacity = needed + 1024u;
        bytes = jce_malloc(capacity);
        if (bytes) needed = jce_introspect_stats_json(scene, bytes, capacity);
    }
    if (!bytes || needed >= capacity) {
        if (bytes != stack) jce_free(bytes);
        LOG_ERROR(LOG_TAG, "benchmark counters could not be sampled");
        return;
    }
    JceJson *stats = jce_json_parse_strict(bytes, needed);
    if (bytes != stack) jce_free(bytes);
    JceJson *root = jce_json_object();
    if (!stats || !root) {
        jce_json_free(stats);
        jce_json_free(root);
        LOG_ERROR(LOG_TAG, "benchmark counters could not be encoded");
        return;
    }
    BenchMetricsObservation observation = {0};
    observation.frame = frame;
    observation.simulation_seconds = simulation_seconds;
    observation.event = event;
    observation.frame_source = frame_source;
    observation.perf_freq = jce_time_perf_freq();
    observation.perf_counter_before = jce_time_perf_counter();
    observation.gpu_available = jce_renderer_get_gpu_stats(&observation.gpu);
    observation.gpu_timing_available = jce_renderer_get_gpu_timing_observation(
        &observation.gpu_timing,sizeof(observation.gpu_timing));
    observation.perf_counter_after = jce_time_perf_counter();
    observation.monotonic_ms = jce_time_ticks_ms();
    sample_window(services, &observation);
    jce_json_set_child(root, "engine", stats);
    /* Introspection memory totals count explicitly tagged allocations only.
     * Zero is not evidence that the process has no heap or no leaks. */
    jce_json_set_string(root, "engine_memory_scope", "explicit tagged allocations only");
    JceMemStats memory;
    if (jce_mem_stats(&memory)) {
        JceJson *process = jce_json_object();
        jce_json_set_number(process, "rss_bytes", (double)memory.current_rss);
        jce_json_set_number(process, "peak_rss_bytes", (double)memory.peak_rss);
        jce_json_set_number(process, "committed_bytes", (double)memory.current_commit);
        jce_json_set_number(process, "peak_committed_bytes", (double)memory.peak_commit);
        jce_json_set_string(process, "scope", "mimalloc process snapshot; not live heap accounting");
        jce_json_set_child(root, "process_memory", process);
    }

    if (renderer && frame) {
        renderer_counters(renderer, root);
        JceSceneRqStats queue;
        jce_scene_renderer_get_rq_stats(renderer, &queue);
        JceJson *counters = jce_json_object();
        jce_json_set_bool(counters, "enabled", queue.enabled);
        jce_json_set_number(counters, "commands", queue.commands_in);
        jce_json_set_number(counters, "submits", queue.submits_out);
        jce_json_set_number(counters, "batches_merged", queue.batches_merged);
        jce_json_set_number(counters, "instances", queue.instances_total);
        jce_json_set_child(root, "render_queue", counters);
    }
    char *text = serialize_record(root, &observation);
    if (text) {
        size_t length = strlen(text);
        /* Replace the owned terminator with LF and write the complete record
         * in one SDK operation. Logger messages have a bounded ring size. */
        text[length] = '\n';
        const bool initial = frame == 0 && event &&
            (!strcmp(event,"initialized") || !strcmp(event,"default-initialized"));
        bool written = initial ? jce_fs_host_write_all(path, text, length + 1u)
                               : jce_fs_host_append(path, text, length + 1u);
        if (written) LOG_INFO(LOG_TAG, "benchmark-counters: saved frame=%u event=%s", frame, event);
        else LOG_ERROR(LOG_TAG, "benchmark counters could not be written");
    } else LOG_ERROR(LOG_TAG, "benchmark counters could not be serialized");
    jce_json_free_string(text);
    jce_json_free(root);
}

void bench_metrics_sample(const JceServices *services,JceScene *scene,
    JceSceneRenderer *renderer,unsigned frame,double simulation_seconds,const char *event)
{
    metrics_sample(services,scene,renderer,frame,simulation_seconds,event,"benchmark_update_count");
}
void bench_metrics_sample_draw(const JceServices *services,JceScene *scene,
    JceSceneRenderer *renderer,unsigned frame,const char *event)
{
    metrics_sample(services,scene,renderer,frame,NAN,event,"default_host_draw_count");
}
