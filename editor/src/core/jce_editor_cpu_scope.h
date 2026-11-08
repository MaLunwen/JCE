/* Main-thread CPU scopes on the existing bounded trace owner. No GPU fence. */
#ifndef JCE_EDITOR_CPU_SCOPE_H
#define JCE_EDITOR_CPU_SCOPE_H
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_timer.h>

class JceEditorCpuScope {
    const char *name_;
    uint64_t id_ = 0, parent_ = 0, started_ = 0;
    JceTraceTaskState state_ = JCE_TRACE_TASK_SUCCEEDED;
public:
    explicit JceEditorCpuScope(const char *static_name) : name_(static_name) {
        if (!jce_trace_enabled()) return;
        parent_ = jce_trace_task_current_id();
        id_ = jce_trace_next_id();
        started_ = jce_time_perf_counter();
        jce_trace_task_submit(id_, parent_, JCE_TRACE_TASK_SERVICE, name_);
        jce_trace_task_begin(id_, id_, name_, 0);
    }
    JceEditorCpuScope(const JceEditorCpuScope &) = delete;
    JceEditorCpuScope &operator=(const JceEditorCpuScope &) = delete;
    void result(JceTraceTaskState state) { state_ = state; }
    ~JceEditorCpuScope() {
        if (!id_) return;
        const double ms = jce_time_perf_to_ms(started_, jce_time_perf_counter());
        jce_trace_task_end(id_, id_, name_, state_,
            ms > 0 ? static_cast<uint64_t>(ms * 1000000.0) : 0);
        jce_trace_task_restore_id(parent_);
    }
};
#endif
