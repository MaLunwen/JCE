#ifndef JCE_FRAME_KPI_INTERNAL_H
#define JCE_FRAME_KPI_INTERNAL_H
#include <SDL3/SDL_iostream.h>
#include <jce/application/jce_engine.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/os/core/jce_perf_phase.h>
#include "os/core/jce_trace_scope_internal.h"

/* Direct call-wall measurements, not thread CPU time or exclusive time.
 * sampled requires an enabled observer and an entered call. A zero duration
 * is then valid. Unsampled counters are unobserved: zero does not prove
 * the actor did not run. These private
 * facts do not prove that the void phase-add published a slot into raw CSV. */
typedef struct {
    bool sampled;
    bool phase_requested, trace_requested;
    uint32_t stats_calls, snapshot_encode_write_calls, encode_calls, reserve_calls;
    uint32_t write_calls, flush_calls;
    double stats_ms, snapshot_encode_write_ms, encode_ms, reserve_ms, write_ms, flush_ms;
} JceFrameKpiProfile;

typedef struct {
    char *bytes;
    size_t capacity;
    /* Scratch budget includes the terminating NUL. Set before first encode.
     * Zero preserves legacy unbounded synchronous-helper growth. Capture glue
     * admits its own separate scratch budget alongside the byte queue budget;
     * their sum is not a whole-engine/process memory certification. */
    size_t max_capacity;
    /* Explicit capture-session opt-in. These latest asynchronous backend
     * values are not current CPU phases or additive frame components. */
    bool backend_timings;
    JceFrameKpiProfile last_profile;
} JceFrameKpiBuffer;

/* Optional completed-render CPU observations. Cached point/drawable sizes are
 * distinct from native pixel queries. Zero or incomplete size pairs encode
 * null; validity booleans preserve valid zero flags and backend frame IDs. */
typedef struct {
    bool window_present;
    bool flags_begin_valid, flags_end_valid;
    bool backend_frame_begin_valid, backend_frame_end_valid;
    uint32_t flags_begin, flags_end;
    uint32_t point_width, point_height, drawable_width, drawable_height;
    uint32_t native_width_begin, native_height_begin;
    uint32_t native_width_end, native_height_end;
    uint32_t backend_frame_begin, backend_frame_end;
    /* Zero preserves the original private fixture/v1 encoding. */
    uint32_t version;
    bool viewport_reported;
    uint32_t viewport_count;
    JceFrameViewportObservation viewports[4];
    /* V3 borrows the exact completed Engine interval; zero ticks are valid.
     * These monotonic performance-counter ticks have no external epoch. */
    bool clock_valid;
    uint64_t clock_frequency, clock_begin, clock_end;
} JceFrameKpiContext;

/* Transactional bounded copy. The Engine owns thread/draw/session admission. */
bool jce_frame_kpi_viewport_valid(const JceFrameViewportObservation *observation);
bool jce_frame_kpi_context_report(JceFrameKpiContext *context,
    const JceFrameViewportObservation *observations, uint32_t count, size_t stride);

/* One exact header authority for the existing complete-record writer. */
const char *jce_frame_kpi_csv_header(bool context_json);

typedef struct {
    JceTraceScope trace;
    uint64_t started;
    bool phase_requested, sampled;
} JceFrameKpiScope;

/* Reuse trace ownership; neither observer enabled means no new clock/read
 * registration. Phase slots may be full, so sampled is not CSV availability. */
static inline JceFrameKpiScope jce_frame_kpi_scope_begin(const char *name)
{
    JceFrameKpiScope scope = {0};
    scope.phase_requested = jce_perf_phase_enabled() != 0;
    scope.trace = jce_trace_scope_begin(name);
    scope.sampled = scope.phase_requested || scope.trace.id != 0;
    if (scope.sampled)
        scope.started = scope.trace.id ? scope.trace.started : jce_time_perf_counter();
    return scope;
}
static inline double jce_frame_kpi_scope_end(JceFrameKpiScope scope, bool succeeded)
{
    double ms = 0.0;
    if (scope.sampled) {
        ms = jce_time_perf_to_ms(scope.started, jce_time_perf_counter());
        if (scope.phase_requested) jce_perf_phase_add(scope.trace.name, ms);
    }
    jce_trace_scope_end(scope.trace, succeeded ? JCE_TRACE_TASK_SUCCEEDED
                                             : JCE_TRACE_TASK_FAILED);
    return ms;
}

/* Main-thread only. Reuses the buffer and performs one complete stream write.
 * phases_eligible is the shared engine interval-continuity decision; false
 * omits phase rows without replacing absent measurements with zero.
 * Invalid CSV phase names or incomplete output fail rather than truncating.
 * snapshot_encode_write contains original peek/format/reserve/write work;
 * reserve and write are nested direct intervals. Never sum overlapping spans
 * or subtract them to manufacture exclusive encoding/CPU time. */
/* One encoder shared by synchronous and queued export. Bytes remain borrowed
 * from buffer until the next encode/destroy. No stream IO occurs here. The
 * consumer must copy them before returning. out_size is zero on failure. */
bool jce_frame_kpi_encode(JceFrameKpiBuffer *buffer, uint64_t frame,
                          double wall_ms, const JceGpuStats *stats,
                          bool phases_eligible, size_t *out_size);
/* A non-NULL context selects the four-column schema. Only frame_wall carries
 * its fixed JSON object; other rows have an empty fourth field. The caller
 * supplies a sealed observation belonging to this completed render. */
bool jce_frame_kpi_encode_with_context(JceFrameKpiBuffer *buffer, uint64_t frame,
    double wall_ms, const JceGpuStats *stats, bool phases_eligible,
    const JceFrameKpiContext *context, size_t *out_size);
bool jce_frame_kpi_export(JceFrameKpiBuffer *buffer, SDL_IOStream *stream,
                          uint64_t frame, double wall_ms, const JceGpuStats *stats, bool phases_eligible);
void jce_frame_kpi_buffer_destroy(JceFrameKpiBuffer *buffer);
#endif
