/* Application-private, owner-thread resource observations in the existing
 * bounded trace. No exporter, renderer phase, worker or allocator is created. */
#ifndef JCE_ENGINE_RESOURCE_OBSERVATION_H
#define JCE_ENGINE_RESOURCE_OBSERVATION_H
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_async.h>
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_mem_profile.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/renderer/jce_renderer.h>
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_async_stats_internal.h"
#include <string.h>

#define JCE_RESOURCE_OBSERVATION_DEFAULT_MS 1000u
#define JCE_RESOURCE_OBSERVATION_MIN_MS 250u
#define JCE_RESOURCE_OBSERVATION_MAX_MS 60000u

typedef struct JceEngineResourceObservation {
    uint64_t owner_thread;
    uint64_t last_sample_ns;
    uint32_t interval_ms;
    bool enabled;
    bool sampled;
} JceEngineResourceObservation;

static inline void engine_resource_observation_init(
    JceEngineResourceObservation *state, const char *enabled, uint32_t interval_ms)
{
    memset(state, 0, sizeof(*state));
    state->enabled = jce_env_flag_enabled(enabled) != 0;
    if (!state->enabled) return;
    if (!interval_ms) interval_ms = JCE_RESOURCE_OBSERVATION_DEFAULT_MS;
    if (interval_ms < JCE_RESOURCE_OBSERVATION_MIN_MS)
        interval_ms = JCE_RESOURCE_OBSERVATION_MIN_MS;
    if (interval_ms > JCE_RESOURCE_OBSERVATION_MAX_MS)
        interval_ms = JCE_RESOURCE_OBSERVATION_MAX_MS;
    state->interval_ms = interval_ms;
    state->owner_thread = jce_thread_current_id();
}

static inline void engine_resource_observation_counter(
    JceTraceScope scope, const char *name, int64_t value)
{
    jce_trace_counter_ring_internal(JCE_TRACE_CATEGORY_CORE, name, value,
                                   scope.id, scope.parent);
}

/* Each half is <= UINT32_MAX, exactly representable in JSON/JS consumers.
 * Raw signed GPU values use their defined modulo-2^64 representation. */
static inline void engine_resource_observation_u64(
    JceTraceScope scope, const char *high, const char *low, uint64_t value)
{
    engine_resource_observation_counter(scope, high, (int64_t)(value >> 32));
    engine_resource_observation_counter(scope, low, (int64_t)(value & UINT32_MAX));
}

static inline void engine_resource_observation_tick(
    JceEngineResourceObservation *state, bool headless, bool gpu_query_allowed)
{
    /* Default-off must precede every clock, thread, stats and metadata query. */
    if (!state || !state->enabled || !jce_trace_enabled()) return;
    /* SDL's cooperative Web owner has raw thread ID zero. Initialization,
     * enabled state and equality supply ownership; zero is not an error code. */
    if (jce_thread_current_id() != state->owner_thread) return;
    const uint64_t now_ns = jce_time_ticks_ns();
    const uint64_t interval_ns = (uint64_t)state->interval_ms * 1000000u;
    if (state->sampled && now_ns >= state->last_sample_ns &&
        now_ns - state->last_sample_ns < interval_ns) return;
    JceTraceScope scope = jce_trace_scope_begin("engine.app_resources");
    if (!scope.id) return;
    const bool clock_rollback = state->sampled && now_ns < state->last_sample_ns;
    state->sampled = true;
    state->last_sample_ns = now_ns; /* One current sample, never catch-up work. */

    JceMemStats memory;
    memset(&memory, 0, sizeof(memory));
    const bool memory_copied = jce_mem_stats(&memory);
    uint64_t tagged_current = 0, tagged_peak = 0;
    jce_mem_profile_get_aggregate_totals(&tagged_current, &tagged_peak);
    JceAsyncExecutorStats async_stats;
    memset(&async_stats, 0, sizeof(async_stats));
    const bool async_available = jce_async_default_stats_internal(&async_stats);
    JceGpuTimingObservation gpu;
    memset(&gpu, 0, sizeof(gpu));
    const bool gpu_copied = gpu_query_allowed &&
        jce_renderer_get_gpu_timing_observation(&gpu, sizeof(gpu)) &&
        gpu.struct_size == sizeof(gpu) && gpu.version == 1u;
    const bool memory_rss_available = memory_copied && memory.current_rss > 0;
    const bool memory_commit_available = memory_copied && memory.current_commit > 0;
    const bool gpu_context_available = gpu_copied && gpu.context_available;
    const bool gpu_backend_available = gpu_context_available && gpu.backend_available;
    const bool gpu_timer_available = gpu_backend_available && gpu.timer_available;

#define ER_COUNTER(name_, value_) engine_resource_observation_counter(scope, name_, (int64_t)(value_))
#define ER_U64(name_, value_) engine_resource_observation_u64(scope, name_ ".hi32", name_ ".lo32", (uint64_t)(value_))
    ER_COUNTER("resource.schema", 1);
    ER_U64("resource.sample_ns", now_ns);
    ER_U64("resource.owner_thread_id", state->owner_thread);
    ER_COUNTER("resource.host_headless", headless);
    ER_COUNTER("resource.pid_available", 0); /* No authoritative public self-PID here. */
    ER_COUNTER("resource.host_role_available", 0); /* App name is not a host-role enum. */
    ER_COUNTER("resource.clock_rollback", clock_rollback);
    ER_COUNTER("resource.memory.copied", memory_copied);
    ER_COUNTER("resource.memory.rss_available", memory_rss_available);
    ER_COUNTER("resource.memory.commit_available", memory_commit_available);
    ER_U64("resource.memory.rss_bytes", memory_rss_available ? memory.current_rss : 0);
    ER_U64("resource.memory.commit_bytes", memory_commit_available ? memory.current_commit : 0);
    ER_COUNTER("resource.tagged.partial_scope", 1);
    ER_U64("resource.tagged.current_bytes", tagged_current);
    ER_U64("resource.tagged.peak_bytes", tagged_peak);
    ER_COUNTER("resource.async.available", async_available);
    ER_COUNTER("resource.async.mode", async_available ? async_stats.mode : 0);
    ER_COUNTER("resource.async.worker_count", async_available ? async_stats.worker_count : 0);
    ER_COUNTER("resource.async.live_tasks", async_available ? async_stats.live_tasks : 0);
    ER_COUNTER("resource.async.queued_tasks", async_available ? async_stats.queued_tasks : 0);
    ER_COUNTER("resource.async.running_tasks", async_available ? async_stats.running_tasks : 0);
    ER_COUNTER("resource.async.pending_completions", async_available ? async_stats.pending_completions : 0);
    ER_U64("resource.async.submitted_tasks", async_available ? async_stats.submitted_tasks : 0);
    ER_U64("resource.async.failed_tasks", async_available ? async_stats.failed_tasks : 0);
    ER_U64("resource.async.queue_time_ns", async_available ? async_stats.total_queue_time_ns : 0);
    ER_U64("resource.async.run_time_ns", async_available ? async_stats.total_run_time_ns : 0);
    ER_COUNTER("resource.gpu.query_allowed", gpu_query_allowed);
    ER_COUNTER("resource.gpu.copied", gpu_copied);
    ER_COUNTER("resource.gpu.context_available", gpu_context_available);
    ER_COUNTER("resource.gpu.backend_available", gpu_backend_available);
    ER_COUNTER("resource.gpu.timer_available", gpu_timer_available);
    ER_COUNTER("resource.gpu.reason_available", gpu_copied);
    ER_COUNTER("resource.gpu.reason", gpu_copied ? gpu.unavailable_reason : 0);
    ER_U64("resource.gpu.context_generation", gpu_context_available ? gpu.context_generation : 0);
    ER_COUNTER("resource.gpu.read_host_frame", gpu_context_available ? gpu.read_host_frame_index : 0);
    ER_COUNTER("resource.gpu.source_frame", gpu_backend_available ? gpu.gpu_source_frame : 0);
    ER_U64("resource.gpu.frequency", gpu_backend_available ? gpu.timer_frequency : 0);
    ER_U64("resource.gpu.raw_begin", gpu_backend_available ? gpu.raw_begin : 0);
    ER_U64("resource.gpu.raw_end", gpu_backend_available ? gpu.raw_end : 0);
#undef ER_U64
#undef ER_COUNTER
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}
#endif
