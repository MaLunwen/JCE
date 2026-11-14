/* Per-instance cost reads and optional same-frame on_update attribution. */
#include "jce_rt_internal.h"
#include "jce_rt_script_cost.h"
#include <jce/os/core/jce_perf_phase.h>

/* Launch diagnostic only. Copy the selector once; environment storage and
 * runtime-owned paths must never become persistent phase-name pointers. */
static bool s_selector_checked;
static char s_focus_path[sizeof(((struct ScriptEntry *)0)->script_path)];

static const char *rt_script_cost_selector(void)
{
    if (!s_selector_checked) {
        const char *flag = getenv("JCE_PERF_SCRIPT_DETAIL");
        s_selector_checked = true;
        if (flag && strcmp(flag, "1") == 0) {
            const char *path = getenv("JCE_PERF_SCRIPT_PATH");
            size_t length = 0;
            bool valid = path && path[0];
            if (valid) {
                while (length < sizeof s_focus_path && path[length]) {
                    const unsigned char ch = (unsigned char)path[length];
                    if (ch < 32 || ch == 127) valid = false;
                    ++length;
                }
                if (length >= sizeof s_focus_path) valid = false;
            }
            if (valid) {
                memcpy(s_focus_path, path, length + 1);
                jce_log_write(JCE_LOG_LEVEL_INFO, LOG_TAG, __FILE__, __LINE__,
                    "script detail v1: selector=exact scope=on_update path_bytes=%u path=%s",
                    (unsigned)length, s_focus_path);
            } else {
                jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                    "script detail refused: PATH needs 1..255 bytes without controls");
            }
        } else if (flag && flag[0] && strcmp(flag, "0") != 0) {
            jce_log_write(JCE_LOG_LEVEL_WARN, LOG_TAG, __FILE__, __LINE__,
                "script detail refused: DETAIL must be exactly 0 or 1");
        }
    }
    return s_focus_path[0] ? s_focus_path : NULL;
}

RtScriptCostFrame rt_script_cost_begin(bool profiling)
{
    RtScriptCostFrame frame = {0};
    if (profiling) frame.focus_path = rt_script_cost_selector();
    return frame;
}

void rt_script_cost_add(RtScriptCostFrame *frame, const char *path, double ms)
{
    if (!frame || !frame->focus_path) return;
    if (path && strcmp(path, frame->focus_path) == 0)
        frame->focus_ms += ms;
    else
        frame->other_ms += ms;
}

void rt_script_cost_end(const RtScriptCostFrame *frame)
{
    if (!frame || !frame->focus_path) return;
    jce_perf_phase_add("script_focus", frame->focus_ms);
    jce_perf_phase_add("script_other", frame->other_ms);
}

void jce_runtime_iterate_script_costs(const JceRuntime *rt,
                                      JceScriptCostIterFn cb, void *user)
{
    if (!rt || !cb) return;
    for (int i = 0; i < rt->script_count; ++i) {
        const struct ScriptEntry *se = &rt->scripts[i];
        JceScriptCostInfo info;
        info.entity      = (uint64_t)se->entity;
        info.script_path = se->script_path;
        info.last_ms     = se->cost_last_ms;
        info.avg_ms      = (se->cost_calls > 0)
                         ? (se->cost_total_ms / (double)se->cost_calls)
                         : 0.0;
        info.calls       = se->cost_calls;
        info.active      = se->active;
        info.measured    = se->cost_measured;
        cb(&info, user);
    }
}

void jce_runtime_reset_script_costs(JceRuntime *rt)
{
    if (!rt) return;
    for (int i = 0; i < rt->script_count; ++i) {
        rt->scripts[i].cost_total_ms = 0.0;
        rt->scripts[i].cost_calls    = 0u;
        /* Preserve the most recent measurement independently of this window. */
    }
}
