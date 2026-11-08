/* Optional application callbacks share one host dispatch path. */
#ifndef JCE_ENGINE_CALLBACKS_H
#define JCE_ENGINE_CALLBACKS_H
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>
#include "jce_engine_trace_scope.h"

static inline void engine_app_update_only(const JceAppDesc *app, float dt)
{
    if (app && app->update) {
        JCE_PROFILE_ZONE_N("App::Update");
        const uint64_t started = jce_time_perf_counter();
        JCE_ENGINE_TRACE_CALL("engine.app_update",
            app->update(dt, app->user_data));
        jce_perf_phase_add("app_simulation",
            jce_time_perf_to_ms(started, jce_time_perf_counter()));
        JCE_PROFILE_ZONE_END;
    }
}
static inline void engine_app_draw_only(const JceAppDesc *app, const JceServices *svc)
{
    if (app && app->draw) {
        JCE_PROFILE_ZONE_N("App::Draw");
        const uint64_t started = jce_time_perf_counter();
        JCE_ENGINE_TRACE_CALL("engine.app_draw",
            app->draw(svc, app->user_data));
        jce_perf_phase_add("app_draw",
            jce_time_perf_to_ms(started, jce_time_perf_counter()));
        JCE_PROFILE_ZONE_END;
    }
}
static inline void engine_app_update_and_draw(const JceAppDesc *app,
                                              float dt, const JceServices *svc)
{
    if (!app || (!app->update && !app->draw)) return;
    const uint64_t combined_start = jce_time_perf_counter();
    engine_app_update_only(app, dt);
    engine_app_draw_only(app, svc);
    /* Preserve the historical combined metric for existing consumers. */
    jce_perf_phase_add("app_update",
        jce_time_perf_to_ms(combined_start, jce_time_perf_counter()));
}
#endif
