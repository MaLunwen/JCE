/* Optional application callbacks share one host dispatch path. */
#ifndef JCE_ENGINE_CALLBACKS_H
#define JCE_ENGINE_CALLBACKS_H
#include <jce/application/jce_app_interface.h>
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/os/core/jce_timer.h>

static inline void engine_app_update_and_draw(const JceAppDesc *app,
                                              float dt, const JceServices *svc)
{
    if (!app || (!app->update && !app->draw)) return;
    const uint64_t combined_start = jce_time_perf_counter();
    if (app->update) {
        JCE_PROFILE_ZONE_N("App::Update");
        const uint64_t started = jce_time_perf_counter();
        app->update(dt, app->user_data);
        jce_perf_phase_add("app_simulation",
            jce_time_perf_to_ms(started, jce_time_perf_counter()));
        JCE_PROFILE_ZONE_END;
    }
    if (app->draw) {
        JCE_PROFILE_ZONE_N("App::Draw");
        const uint64_t started = jce_time_perf_counter();
        app->draw(svc, app->user_data);
        jce_perf_phase_add("app_draw",
            jce_time_perf_to_ms(started, jce_time_perf_counter()));
        JCE_PROFILE_ZONE_END;
    }
    /* Preserve the historical combined metric for existing consumers. */
    jce_perf_phase_add("app_update",
        jce_time_perf_to_ms(combined_start, jce_time_perf_counter()));
}
#endif
