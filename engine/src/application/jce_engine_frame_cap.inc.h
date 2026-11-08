/* Private implementation include: the one frame-cap state, policy and wait.
 * Included once by jce_engine.c; fixtures port clock/sleep/trace publication. */
#include "jce_engine_frame_cap.h"
#include "jce_engine_perf_hitch.h"
#include <jce/application/jce_engine.h>
#include <jce/os/core/jce_thread.h>
#include <jce/os/core/jce_timer.h>
#include <jce/os/core/jce_trace.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_trace_scope_internal.h"

/* The rate and deadline are one process-global owner, callable before boot. */
static struct {
    int fps;
    uint64_t deadline;
    bool run_in_background; /* main-thread effective host permission */
    const void *launch_owner;
    bool launch_present, launch_observed;
    int launch_fps, requested_fps, observed_requested, observed_effective;
} s_frame_cap = {0};
/* Repeated declarations are intentional: exact-owner fixtures port these
 * functions only after including their real public declarations. */
void jce_engine_launch_fps_observe(const void *owner, int phase);

void jce_engine_set_target_fps(int fps)
{
    if (fps < 0) fps = 0;
    if (s_frame_cap.launch_owner) {
        s_frame_cap.requested_fps = fps;
        if (s_frame_cap.launch_present) fps = s_frame_cap.launch_fps;
    }
    if (fps != s_frame_cap.fps) {
        s_frame_cap.fps = fps;
        s_frame_cap.deadline = 0;
    }
    jce_engine_launch_fps_observe(s_frame_cap.launch_owner, 2);
}

int jce_engine_get_target_fps(void) { return s_frame_cap.fps; }

void jce_engine_launch_fps_observe(const void *owner, int phase)
{
    if (!owner || s_frame_cap.launch_owner != owner) return;
    const int effective = jce_engine_get_target_fps();
    if (phase == 2 && s_frame_cap.launch_observed &&
        s_frame_cap.observed_requested == s_frame_cap.requested_fps &&
        s_frame_cap.observed_effective == effective) return;
    s_frame_cap.launch_observed = true;
    s_frame_cap.observed_requested = s_frame_cap.requested_fps;
    s_frame_cap.observed_effective = effective;
    jce_log_write(JCE_LOG_LEVEL_INFO, "launch-policy", __FILE__, __LINE__,
        "launch-fps: phase=%d requested=%d override_present=%d override=%d effective=%d scope=engine_rate_owner_not_display_cadence",
        phase, s_frame_cap.requested_fps, s_frame_cap.launch_present ? 1 : 0,
        s_frame_cap.launch_fps, effective);
    JceTraceScope scope = jce_trace_scope_begin("engine.launch_fps");
    if (scope.id) {
#define FPS_DETAIL(name_, value_) jce_trace_counter_scoped_internal(JCE_TRACE_CATEGORY_CORE, \
    "launch.fps." name_, (int64_t)(value_), scope.id, scope.id)
        FPS_DETAIL("phase", phase);
        FPS_DETAIL("requested", s_frame_cap.requested_fps);
        FPS_DETAIL("override_present", s_frame_cap.launch_present);
        FPS_DETAIL("override", s_frame_cap.launch_fps);
        FPS_DETAIL("effective", effective);
#undef FPS_DETAIL
    }
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
}

bool jce_engine_launch_fps_begin(const void *owner, bool present, int fps)
{
    if (!owner || fps < 0 || s_frame_cap.launch_owner) return false;
    s_frame_cap.launch_owner = owner;
    s_frame_cap.requested_fps = s_frame_cap.fps;
    s_frame_cap.launch_present = present; s_frame_cap.launch_fps = fps;
    if (present) { s_frame_cap.fps = fps; s_frame_cap.deadline = 0; }
    jce_engine_launch_fps_observe(owner, 1);
    return true;
}

void jce_engine_launch_fps_end(const void *owner)
{
    if (!owner || s_frame_cap.launch_owner != owner) return;
    s_frame_cap.launch_present = false;
    s_frame_cap.fps = s_frame_cap.requested_fps; s_frame_cap.deadline = 0;
    jce_engine_launch_fps_observe(owner, 5);
    s_frame_cap.launch_owner = NULL; s_frame_cap.launch_observed = false;
    s_frame_cap.launch_fps = 0;
}

void jce_engine_set_run_in_background(bool enabled)
{
    s_frame_cap.run_in_background = enabled;
}
bool jce_engine_get_run_in_background(void)
{
    return s_frame_cap.run_in_background;
}

uint64_t jce_engine_frame_cap_advance(uint64_t now, uint64_t budget,
                                      uint64_t *deadline)
{
    if (!deadline || budget == 0) return 0;
    if (*deadline == 0 || now >= *deadline + budget) {
        *deadline = now + budget;
        return 0;
    }
    if (now >= *deadline) {
        *deadline += budget;
        return 0;
    }
    const uint64_t wait_until = *deadline;
    *deadline += budget;
    return wait_until;
}

/* Sleep the bulk and spin the tail, exactly as before. Record the existing
 * wait-start and final spin counter; do not add a second clock or read. */
static void engine_frame_cap_wait(JceEnginePerfHitch *state)
{
    const int fps = s_frame_cap.fps;
    JceEnginePerfPacing pacing = {fps, 0.0, 0.0, false};
    if (fps <= 0) goto recorded;
    const uint64_t freq = jce_time_perf_freq();
    if (freq == 0) goto recorded;
    const uint64_t budget = freq / (uint64_t)fps;
    const uint64_t decision = jce_time_perf_counter();
    const uint64_t until = jce_engine_frame_cap_advance(
        decision, budget, &s_frame_cap.deadline);
    if (budget == 0) goto recorded;
    pacing.clock_valid = true;
    if (until == 0) goto recorded;
    const uint64_t spin_ticks = freq / 2000u;
    const uint64_t now = jce_time_perf_counter();
    if (now < decision) pacing.clock_valid = false;
    if (now < until) {
        uint64_t wait_id = 0, parent_id = 0;
        if (pacing.clock_valid && jce_trace_enabled()) {
            parent_id = jce_trace_task_current_id();
            wait_id = jce_trace_next_id();
            jce_trace_wait_begin(wait_id, parent_id, "engine.frame_pacing");
        }
        const uint64_t left = until - now;
        pacing.planned_ms = (double)left * 1000.0 / (double)freq;
        if (left > spin_ticks) {
            const uint32_t ms = (uint32_t)(((left - spin_ticks) * 1000u) / freq);
            if (ms > 0) jce_thread_sleep_ms(ms);
        }
        uint64_t end;
        while ((end = jce_time_perf_counter()) < until) { /* tail spin */ }
        if (end >= now)
            pacing.actual_ms = (double)(end - now) * 1000.0 / (double)freq;
        else
            pacing.clock_valid = false;
        if (wait_id) {
            /* Reuse the wait owner's existing samples. Trace event clocks
             * belong to the trace API; no extra pacing counter is read. */
            const double ns = pacing.actual_ms * 1000000.0;
            jce_trace_wait_end(wait_id, parent_id, "engine.frame_pacing",
                ns >= (double)UINT64_MAX ? UINT64_MAX : (uint64_t)ns);
        }
    }
recorded:
    jce_engine_perf_hitch_record_pacing(state, &pacing);
}
