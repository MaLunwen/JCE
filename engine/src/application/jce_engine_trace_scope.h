/* Application aliases; implementation belongs to the core trace owner. */
#ifndef JCE_ENGINE_TRACE_SCOPE_H
#define JCE_ENGINE_TRACE_SCOPE_H
#include <jce/os/core/jce_env.h>
#include <jce/os/core/jce_perf_phase.h>
#include "os/core/jce_trace_scope_internal.h"
/* Environment policy shared by startup and frame telemetry. */
static inline bool engine_trace_flag_enabled(const char *value)
{
    return jce_env_flag_enabled(value) != 0;
}
static inline bool engine_trace_policy(const char *trace, const char *perf,
                                       const char *export_path,
                                       const char *stream_path, bool debug_default)
{
    if ((export_path && export_path[0]) || (stream_path && stream_path[0]))
        return true;
    if (trace && trace[0]) return engine_trace_flag_enabled(trace);
    return engine_trace_flag_enabled(perf) || debug_default;
}
typedef JceTraceScope JceEngineTraceScope;
#define engine_trace_begin jce_trace_scope_begin
#define engine_trace_end jce_trace_scope_end
/* Scope success records the void API returning. Admission follows. */
static inline void engine_trace_renderer_tail_end(JceEngineTraceScope scope,
                                                  uint64_t begin, uint64_t end)
{
    jce_perf_phase_add("end_frame", jce_time_perf_to_ms(begin, end));
    const JceTracePerfIntervalNames names = {"engine.renderer_tail_clock.frequency",
        "engine.renderer_tail_clock.begin", "engine.renderer_tail_clock.end"};
    jce_trace_perf_interval_emit(scope, JCE_TRACE_CATEGORY_RENDER, names, begin, end);
    engine_trace_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

#define JCE_ENGINE_TRACE_CALL JCE_TRACE_SCOPE_CALL
#define JCE_ENGINE_TRACE_RESULT JCE_TRACE_SCOPE_RESULT
#endif
