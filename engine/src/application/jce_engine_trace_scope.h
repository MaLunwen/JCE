/* Bootstrap service scopes reuse the public bounded trace authority. */
#ifndef JCE_ENGINE_TRACE_SCOPE_H
#define JCE_ENGINE_TRACE_SCOPE_H
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_timer.h>

typedef struct JceEngineTraceScope {
    uint64_t id;
    uint64_t parent;
    uint64_t started;
    const char *name;
} JceEngineTraceScope;

static inline JceEngineTraceScope engine_trace_begin(const char *name)
{
    JceEngineTraceScope scope = {0, 0, 0, name};
    if (jce_trace_enabled()) {
        scope.parent = jce_trace_task_current_id();
        scope.id = jce_trace_next_id();
        scope.started = jce_time_perf_counter();
        jce_trace_task_submit(scope.id, scope.parent,
                              JCE_TRACE_TASK_SERVICE, name);
        jce_trace_task_begin(scope.id, scope.id, name, 0);
    }
    return scope;
}

static inline void engine_trace_end(JceEngineTraceScope scope,
                                    JceTraceTaskState state)
{
    if (scope.id) {
        const double ms = jce_time_perf_to_ms(scope.started,
                                              jce_time_perf_counter());
        jce_trace_task_end(scope.id, scope.id, scope.name, state,
                           ms > 0.0 ? (uint64_t)(ms * 1000000.0) : 0);
        jce_trace_task_restore_id(scope.parent);
    }
}

#define JCE_ENGINE_TRACE_CALL(name_, call_) do { \
    JceEngineTraceScope scope_ = engine_trace_begin(name_); \
    call_; \
    engine_trace_end(scope_, JCE_TRACE_TASK_SUCCEEDED); \
} while (0)
#define JCE_ENGINE_TRACE_RESULT(name_, call_, succeeded_) do { \
    JceEngineTraceScope scope_ = engine_trace_begin(name_); \
    call_; \
    engine_trace_end(scope_, (succeeded_) ? JCE_TRACE_TASK_SUCCEEDED \
                                         : JCE_TRACE_TASK_FAILED); \
} while (0)
#endif
