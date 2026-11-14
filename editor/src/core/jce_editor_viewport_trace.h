/* CPU invocation/admission observations, not drawn-frame or GPU completion. */
#ifndef JCE_EDITOR_VIEWPORT_TRACE_H
#define JCE_EDITOR_VIEWPORT_TRACE_H
#include "jce_editor_cpu_scope.h"

enum class JceEditorViewportRenderReason {
    Admitted = 0, Unavailable = 1, EmptyExtent = 2, ImpostorBusy = 3,
    ProbeBusy = 4, MissingSceneOwner = 5, TargetRefused = 6,
    ResourcesRefused = 7, Incomplete = 8, SceneLoading = 9
};
enum class JceEditorViewportPolicyReason {
    Admitted = 0, Capture = 1, InvalidLayout = 2, FocusLost = 3,
    Clipped = 4, Hidden = 5, Collapsed = 6
};

inline void jce_editor_viewport_policy_trace(const char *static_name,
                                            JceEditorViewportPolicyReason reason)
{
    if (jce_trace_enabled())
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, static_name,
            static_cast<int64_t>(reason));
}

/* One reason sample per invoked render owner, parented to its CPU span. The
 * sample is a decision enum, not a cumulative or simultaneous counter. A
 * separate admitted sample marks passage of early guards. Later target/resource
 * refusal replaces the final reason; no sample establishes GPU work/presentation. */
class JceEditorViewportRenderTrace {
    JceEditorCpuScope scope_;
    const char *reason_name_;
    const char *admitted_name_;
    bool enabled_;
    JceEditorViewportRenderReason reason_ = JceEditorViewportRenderReason::Incomplete;
public:
    JceEditorViewportRenderTrace(const char *scope_name, const char *reason_name,
                                const char *admitted_name)
        : scope_(scope_name), reason_name_(reason_name), admitted_name_(admitted_name),
          enabled_(jce_trace_enabled()) {}
    JceEditorViewportRenderTrace(const JceEditorViewportRenderTrace &) = delete;
    JceEditorViewportRenderTrace &operator=(const JceEditorViewportRenderTrace &) = delete;
    void reason(JceEditorViewportRenderReason reason) { reason_ = reason; }
    void admitted() {
        reason_ = JceEditorViewportRenderReason::Admitted;
        if (enabled_)
            jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, admitted_name_, 1);
    }
    ~JceEditorViewportRenderTrace() {
        if (!enabled_) return;
        jce_trace_counter(JCE_TRACE_CATEGORY_RENDER, reason_name_,
            static_cast<int64_t>(reason_));
        const bool refused = reason_ == JceEditorViewportRenderReason::TargetRefused ||
            reason_ == JceEditorViewportRenderReason::ResourcesRefused ||
            reason_ == JceEditorViewportRenderReason::Incomplete;
        scope_.result(reason_ == JceEditorViewportRenderReason::Admitted
            ? JCE_TRACE_TASK_SUCCEEDED : refused ? JCE_TRACE_TASK_FAILED : JCE_TRACE_TASK_CANCELLED);
    }
};
#endif
