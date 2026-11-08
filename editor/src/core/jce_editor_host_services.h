/* Editor-owned CPU maintenance. Never dispatch gameplay, ImGui or GPU work. */
#ifndef JCE_EDITOR_HOST_SERVICES_H
#define JCE_EDITOR_HOST_SERVICES_H
#include "jce_editor.h"
#include "jce_build_manager.h"
#include "jce_run_manager.h"
#include "jce_cook_manager.h"
#include "jce_editor_headless_build.h"
#include "jce_editor_state.h"
#include "scene/jce_asset_path_index.h"
#include "ui/jce_editor_panels.h"

#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_timer.h>

/* A stack adapter to the public canonical service events, not a new trace
 * registry/thread/capture policy. Names at these call sites are static. */
class JceEditorHostServiceTrace {
    const char *name_;
    uint64_t id_ = 0, parent_ = 0, started_ = 0;
    bool returned_ = false;
public:
    explicit JceEditorHostServiceTrace(const char *name) : name_(name) {
        if (!jce_trace_enabled()) return;
        parent_ = jce_trace_task_current_id();
        id_ = jce_trace_next_id();
        started_ = jce_time_perf_counter();
        jce_trace_task_submit(id_, parent_, JCE_TRACE_TASK_SERVICE, name_);
        jce_trace_task_begin(id_, id_, name_, 0);
    }
    JceEditorHostServiceTrace(const JceEditorHostServiceTrace &) = delete;
    JceEditorHostServiceTrace &operator=(const JceEditorHostServiceTrace &) = delete;
    void returned() { returned_ = true; }
    ~JceEditorHostServiceTrace() {
        if (!id_) return;
        const double ms = jce_time_perf_to_ms(started_, jce_time_perf_counter());
        jce_trace_task_end(id_, id_, name_, returned_ ? JCE_TRACE_TASK_SUCCEEDED
            : JCE_TRACE_TASK_FAILED, ms > 0 ? (uint64_t)(ms * 1000000.0) : 0);
        jce_trace_task_restore_id(parent_);
    }
};
#define JCE_EDITOR_HOST_SERVICE_CALL(name_, call_) do { \
    JceEditorHostServiceTrace service_trace_(name_); \
    call_; \
    service_trace_.returned(); \
} while (false)

static inline void jce_editor_host_services_poll(bool headless, float dt,
                                                 bool refresh_ui_clock)
{
    JceEditorHostServiceTrace poll_trace("editor.cpu_services.poll");
    if (headless) {
        JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.build", jce_build_manager_poll());
        JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.headless", jce_editor_headless_build_poll());
        poll_trace.returned();
        return;
    }
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.console", jce_editor_panel_console_poll());
    /* Run's pending-build path already polls Build. Observe before Run can
     * finish that transition so neither path polls the same child twice. */
    const bool run_owns_build = jce_run_manager_state() == JCE_RUN_BUILDING;
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.run", jce_run_manager_poll());
    if (!run_owns_build)
        JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.build", jce_build_manager_poll());
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.cook", jce_cook_manager_poll());
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.serial", jce_state_scene_serial_poll());
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.index", jce_asset_path_index_poll());
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.test_runner", jce_editor_panel_test_runner_tick());
    JCE_EDITOR_HOST_SERVICE_CALL("editor.cpu_services.maintenance",
        jce_editor_tick_cpu_services(dt, refresh_ui_clock));
    poll_trace.returned();
}
#endif
