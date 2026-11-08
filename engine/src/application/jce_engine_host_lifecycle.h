/* Single host's drawable and resumed-interval disposition. No registry. */
#ifndef JCE_ENGINE_HOST_LIFECYCLE_H
#define JCE_ENGINE_HOST_LIFECYCLE_H
#include <jce/application/jce_lifecycle.h>
#include <jce/os/platform/jce_window.h>
#include <SDL3/SDL_events.h>
#include "os/core/jce_trace_scope_internal.h"
#include "os/core/jce_trace_capture_internal.h"
#include "application/jce_engine_perf_hitch.h"

/* Actual inputs to the existing drawable decision, not a rendering policy.
 * Failed native-size queries retain their raw decision outputs separately
 * from whether those outputs are valid observed dimensions. */
typedef struct JceEngineDrawableObservation {
    bool headless, window_present;
    bool minimized_gate, lifecycle_paused;
    bool extent_query_attempted, extent_query_valid;
    bool allowed;
    int width, height;
} JceEngineDrawableObservation;

typedef struct JceEngineHostLifecycle {
    bool render_blocked, discard_step;
    bool drawable_published;
    uint32_t drawable_generation;
    uint32_t drawable_reason;
    JceEngineDrawableObservation drawable_observation;
} JceEngineHostLifecycle;

static inline bool engine_host_drawable_same(
    const JceEngineDrawableObservation *a, const JceEngineDrawableObservation *b)
{
    return a->headless == b->headless && a->window_present == b->window_present &&
        a->minimized_gate == b->minimized_gate &&
        a->lifecycle_paused == b->lifecycle_paused &&
        a->extent_query_attempted == b->extent_query_attempted &&
        a->extent_query_valid == b->extent_query_valid &&
        a->allowed == b->allowed && a->width == b->width && a->height == b->height;
}

/* Opt-in transition evidence over the existing ring. Native flags are sampled
 * only when publishing a transition, never continuously or as OS occlusion.
 * The snapshot/cache cannot admit, skip, resize or otherwise control a frame. */
static inline void engine_host_observe_drawable(JceEngineHostLifecycle *state,
    const JceEngineDrawableObservation *observation, const JceWindow *window)
{
    if (!jce_trace_enabled()) return;
    const uint32_t generation = jce_trace_capture_generation();
    if (state->drawable_published && state->drawable_generation == generation &&
        engine_host_drawable_same(&state->drawable_observation, observation)) return;

    JceTraceScope scope = jce_trace_scope_begin("engine.drawable_transition");
    if (!scope.id) return;
    uint32_t flags = 0;
    const bool flags_attempted = !observation->headless && observation->window_present;
    const bool flags_valid = flags_attempted && jce_window_get_flags(window, &flags);
    const bool extent_known = observation->extent_query_valid;
#define DRAWABLE_COUNTER(name_, value_) jce_trace_counter_scoped_internal( \
    JCE_TRACE_CATEGORY_CORE, "drawable." name_, (int64_t)(value_), scope.id, scope.parent)
    DRAWABLE_COUNTER("generation", generation);
    DRAWABLE_COUNTER("headless", observation->headless);
    DRAWABLE_COUNTER("window_present", observation->window_present);
    DRAWABLE_COUNTER("windowed_inputs_used", !observation->headless);
    DRAWABLE_COUNTER("minimized_gate", observation->minimized_gate);
    DRAWABLE_COUNTER("lifecycle_paused", observation->lifecycle_paused);
    DRAWABLE_COUNTER("extent_query_attempted", observation->extent_query_attempted);
    DRAWABLE_COUNTER("extent_query_valid", extent_known);
    /* Preserve the old positivity decision even if a failed query wrote output.
     * This gate is not a claim that an unavailable native extent is known. */
    DRAWABLE_COUNTER("extent_gate_open", observation->width > 0 && observation->height > 0);
    if (extent_known) {
        DRAWABLE_COUNTER("native_width", observation->width);
        DRAWABLE_COUNTER("native_height", observation->height);
    }
    DRAWABLE_COUNTER("allowed", observation->allowed);
    DRAWABLE_COUNTER("transition_flags_query_attempted", flags_attempted);
    DRAWABLE_COUNTER("transition_flags_valid", flags_valid);
    if (flags_valid) {
        DRAWABLE_COUNTER("transition_flags", flags);
        DRAWABLE_COUNTER("transition_hidden", (flags & JCE_WINDOW_HIDDEN) != 0);
        DRAWABLE_COUNTER("transition_minimized", (flags & JCE_WINDOW_MINIMIZED) != 0);
        DRAWABLE_COUNTER("transition_input_focus", (flags & JCE_WINDOW_INPUT_FOCUS) != 0);
    }
#undef DRAWABLE_COUNTER
    jce_trace_scope_end(scope, JCE_TRACE_TASK_SUCCEEDED);
    /* A disabled/suppressed/refused scope must leave a later baseline pending. */
    state->drawable_observation = *observation;
    state->drawable_generation = generation;
    state->drawable_published = true;
}

static inline bool engine_host_render_allowed(JceEngineHostLifecycle *state,
        bool minimized, bool os_paused, int width, int height)
{
    const bool blocked = minimized || os_paused || width <= 0 || height <= 0;
    state->drawable_reason = (minimized ? JCE_ENGINE_DRAWABLE_MINIMIZED : 0u) |
        (os_paused ? JCE_ENGINE_DRAWABLE_PAUSED : 0u) |
        (width <= 0 || height <= 0 ? JCE_ENGINE_DRAWABLE_EXTENT : 0u);
    if (state->render_blocked && !blocked) state->discard_step = true;
    state->render_blocked = blocked;
    return !blocked;
}
static inline float engine_host_step_dt(JceEngineHostLifecycle *state, float dt)
{
    if (!(dt >= 0.0f)) dt = 0.0f; /* includes NaN */
    if (dt > 0.1f) dt = 0.1f;
    if (state->discard_step) { state->discard_step = false; dt = 0.0f; }
    return dt;
}
static inline void engine_host_lifecycle_event(JceEngineHostLifecycle *state,
                                               JceWindow *window, uint32_t type)
{
    switch (type) {
    case SDL_EVENT_WINDOW_FOCUS_GAINED:
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_GAINED); break;
    case SDL_EVENT_WINDOW_FOCUS_LOST:
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); break;
    case SDL_EVENT_WINDOW_MINIMIZED:
        state->render_blocked = true;
        jce_lifecycle_emit(JCE_LIFECYCLE_FOCUS_LOST); break;
    case SDL_EVENT_WINDOW_RESTORED:
    case SDL_EVENT_WINDOW_SHOWN: {
        uint32_t flags = 0;
        if (state->render_blocked) state->discard_step = true;
        /* Restore is not a mobile/manual RESUME and does not invent focus. */
        if (jce_window_get_flags(window, &flags))
            jce_lifecycle_emit((flags & JCE_WINDOW_INPUT_FOCUS) != 0
                ? JCE_LIFECYCLE_FOCUS_GAINED : JCE_LIFECYCLE_FOCUS_LOST);
        break;
    }
    case SDL_EVENT_DID_ENTER_BACKGROUND:
        jce_lifecycle_emit(JCE_LIFECYCLE_PAUSE); break;
    case SDL_EVENT_WILL_ENTER_FOREGROUND:
        if (jce_lifecycle_is_paused()) state->discard_step = true;
        jce_lifecycle_emit(JCE_LIFECYCLE_RESUME); break;
    case SDL_EVENT_LOW_MEMORY:
        jce_lifecycle_emit(JCE_LIFECYCLE_LOW_MEMORY); break;
    case SDL_EVENT_RENDER_DEVICE_RESET:
        jce_lifecycle_emit(JCE_LIFECYCLE_DEVICE_RESET); break;
    default: break;
    }
}
#endif
