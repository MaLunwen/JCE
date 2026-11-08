/* Private Build completion policy. Existing status consumers retain their ABI. */
#ifndef JCE_BUILD_PROCESS_OUTCOME_H
#define JCE_BUILD_PROCESS_OUTCOME_H

#include "jce_build_manager.h"
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_timer.h>

struct JceBuildProcessOutcome {
    JceBuildState state;
    JceTraceTaskState trace_state;
};

static inline JceBuildProcessOutcome jce_build_process_outcome(bool cancelled, int exit_code)
{
    if (cancelled) return {JCE_BUILD_FAILED, JCE_TRACE_TASK_CANCELLED};
    if (exit_code != 0) return {JCE_BUILD_FAILED, JCE_TRACE_TASK_FAILED};
    return {JCE_BUILD_SUCCEEDED, JCE_TRACE_TASK_SUCCEEDED};
}

/* Observe the existing owner handling an exited child. This static-name CPU
 * service covers release and the next-step/finish decision, not child run time
 * or GPU completion. A failed next spawn/finish is also a failed transition.
 * Cancellation has priority even when the OS child exits zero. */
class JceBuildProcessCompletionTrace {
    const JceBuildState &state_;
    bool cancelled_;
    int exit_code_;
    uint64_t id_ = 0, parent_ = 0, started_ = 0;
public:
    JceBuildProcessCompletionTrace(const JceBuildState &state, bool cancelled, int exit_code)
        : state_(state), cancelled_(cancelled), exit_code_(exit_code) {
        if (!jce_trace_enabled()) return;
        parent_ = jce_trace_task_current_id();
        id_ = jce_trace_next_id();
        started_ = jce_time_perf_counter();
        jce_trace_task_submit(id_, parent_, JCE_TRACE_TASK_SERVICE, "editor.build.tool_completion");
        jce_trace_task_begin(id_, id_, "editor.build.tool_completion", 0);
    }
    JceBuildProcessCompletionTrace(const JceBuildProcessCompletionTrace &) = delete;
    JceBuildProcessCompletionTrace &operator=(const JceBuildProcessCompletionTrace &) = delete;
    ~JceBuildProcessCompletionTrace() {
        if (!id_) return;
        const auto terminal = jce_build_process_outcome(cancelled_, state_ == JCE_BUILD_FAILED ? 1 : 0);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_completion.cancelled", cancelled_);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_completion.exit_code", exit_code_);
        jce_trace_counter(JCE_TRACE_CATEGORY_CORE, "editor.build.tool_completion.state", state_);
        const double ms = jce_time_perf_to_ms(started_, jce_time_perf_counter());
        jce_trace_task_end(id_, id_, "editor.build.tool_completion", terminal.trace_state,
            ms > 0 ? static_cast<uint64_t>(ms * 1000000.0) : 0);
        jce_trace_task_restore_id(parent_);
    }
};

#endif
