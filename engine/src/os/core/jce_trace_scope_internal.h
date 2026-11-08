/* Private synchronous service spans over the single bounded trace authority.
 * Producer capture/reset changes require quiescence, as in jce_trace.h. */
#ifndef JCE_TRACE_SCOPE_INTERNAL_H
#define JCE_TRACE_SCOPE_INTERNAL_H
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_timer.h>

/* Private correlation seam; public counter ABI stays unchanged. */
void jce_trace_counter_scoped_internal(JceTraceCategory category, const char *name,
                                        int64_t value, uint64_t id, uint64_t parent);

/* Allocator callbacks use the same ring writer without optional profiler
 * plots, task TLS setters, allocation or external consumer callbacks. */
void jce_trace_counter_ring_internal(JceTraceCategory category, const char *name,
                                     int64_t value, uint64_t id, uint64_t parent);

typedef struct JceTraceScope {
    uint64_t id;
    uint64_t parent;
    uint64_t started;
    const char *name;
    uint64_t restore;
} JceTraceScope;

typedef struct JceTracePerfIntervalNames {
    const char *frequency;
    const char *begin;
    const char *end;
} JceTracePerfIntervalNames;

/* Emit raw clock edges together inside an already active CPU scope. */
static inline void jce_trace_perf_interval_emit(JceTraceScope scope,
    JceTraceCategory category, JceTracePerfIntervalNames names,
    uint64_t begin, uint64_t end)
{
    if (!scope.id || begin > end || end > (uint64_t)INT64_MAX) return;
    const uint64_t frequency = jce_time_perf_freq();
    if (!frequency || frequency > (uint64_t)INT64_MAX) return;

    jce_trace_counter_ring_internal(category, names.frequency,
        (int64_t)frequency, scope.id, scope.id);
    jce_trace_counter_ring_internal(category, names.begin,
        (int64_t)begin, scope.id, scope.id);
    jce_trace_counter_ring_internal(category, names.end,
        (int64_t)end, scope.id, scope.id);
}

static inline JceTraceScope jce_trace_scope_begin_parent(const char *name, uint64_t parent)
{
    JceTraceScope scope = {0, 0, 0, name, 0};
    if (jce_trace_enabled()) {
        scope.restore = jce_trace_task_current_id();
        scope.parent = parent ? parent : scope.restore;
        scope.id = jce_trace_next_id();
        scope.started = jce_time_perf_counter();
        jce_trace_task_submit(scope.id, scope.parent,
                              JCE_TRACE_TASK_SERVICE, name);
        jce_trace_task_begin(scope.id, scope.id, name, 0);
    }
    return scope;
}

static inline JceTraceScope jce_trace_scope_begin(const char *name)
{
    return jce_trace_scope_begin_parent(name, 0);
}

static inline void jce_trace_scope_end(JceTraceScope scope,
                                    JceTraceTaskState state)
{
    if (scope.id) {
        const double ms = jce_time_perf_to_ms(scope.started,
                                              jce_time_perf_counter());
        jce_trace_task_end(scope.id, scope.id, scope.name, state,
                           ms > 0.0 ? (uint64_t)(ms * 1000000.0) : 0);
        jce_trace_task_restore_id(scope.restore);
    }
}

#define JCE_TRACE_SCOPE_CALL(name_, call_) do { \
    JceTraceScope scope_ = jce_trace_scope_begin(name_); \
    call_; \
    jce_trace_scope_end(scope_, JCE_TRACE_TASK_SUCCEEDED); \
} while (0)
#define JCE_TRACE_SCOPE_RESULT(name_, call_, succeeded_) do { \
    JceTraceScope scope_ = jce_trace_scope_begin(name_); \
    call_; \
    jce_trace_scope_end(scope_, (succeeded_) ? JCE_TRACE_TASK_SUCCEEDED \
                                         : JCE_TRACE_TASK_FAILED); \
} while (0)
#endif
