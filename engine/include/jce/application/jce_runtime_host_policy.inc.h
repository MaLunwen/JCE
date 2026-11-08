/* Implementation-only host policy shared by the stock SDK entry and Editor
 * Play. No exported API, SDL/bgfx dependency, timer, registry or sleep owner. */
#ifndef JCE_RUNTIME_HOST_POLICY_INC_H
#define JCE_RUNTIME_HOST_POLICY_INC_H
#include <jce/application/jce_engine.h>
#include <jce/application/jce_lifecycle.h>
#include <jce/application/jce_runtime.h>
#include <string.h>

typedef struct JceRuntimeHostPolicy {
    JceRuntime *runtime; /* borrowed; close policy before runtime destruction */
    JceLifecycleHandle listener;
    uint64_t transition;
    int running_fps, last_temporary_fps, paused_fps, background_fps;
    bool initialized, native_window, run_in_background, manual_pause;
    bool blocked, discard_resume_step, temporary_fps, audio_pause_owned;
    bool prior_background, published_background, background_owned;
} JceRuntimeHostPolicy;

static inline void jce_runtime_host_policy_tick(JceRuntimeHostPolicy *state,
                                                bool manual_pause)
{
    bool focused, paused, blocked, temporary;
    uint64_t transition;
    int target;
    if (!state || !state->initialized) return;
    state->manual_pause = manual_pause;
    focused = !state->native_window || jce_lifecycle_is_focused();
    paused = state->native_window && jce_lifecycle_is_paused();
    blocked = manual_pause || paused || (!focused && !state->run_in_background);
    /* Background PlayerLoop hooks share the same effective pause permission;
     * foreground auxiliary hooks retain their documented dispatch frequency. */
    const bool background = state->run_in_background && !manual_pause && !paused;
    state->published_background = background;
    if (jce_engine_get_run_in_background() != background)
        jce_engine_set_run_in_background(background);
    if (state->blocked && !blocked) state->discard_resume_step = true;
    state->blocked = blocked;
    state->transition = state->transition == UINT64_MAX ? 1u : state->transition + 1u;
    transition = state->transition;
    if (state->runtime && blocked != state->audio_pause_owned) {
        state->audio_pause_owned = blocked; /* publish before nested callbacks */
        if (blocked) jce_runtime_pause_audio(state->runtime);
        else jce_runtime_resume_audio(state->runtime);
        if (!state->initialized || state->transition != transition) return;
    }
    temporary = manual_pause || paused || !focused;
    if (temporary) {
        if (!state->temporary_fps ||
            jce_engine_get_target_fps() != state->last_temporary_fps)
            state->running_fps = jce_engine_get_target_fps();
        target = (!focused || paused) ? state->background_fps : state->paused_fps;
        state->temporary_fps = true;
        state->last_temporary_fps = target;
        if (jce_engine_get_target_fps() != target) jce_engine_set_target_fps(target);
    } else if (state->temporary_fps) {
        const int running_fps = state->running_fps;
        const bool restore = jce_engine_get_target_fps() == state->last_temporary_fps;
        state->temporary_fps = false;
        if (restore) jce_engine_set_target_fps(running_fps);
    }
}
static void JCE_CALL jce_runtime_host_policy_event(JceLifecycleEvent event, void *user)
{
    JceRuntimeHostPolicy *state = (JceRuntimeHostPolicy *)user;
    if (event == JCE_LIFECYCLE_FOCUS_LOST || event == JCE_LIFECYCLE_FOCUS_GAINED ||
        event == JCE_LIFECYCLE_PAUSE || event == JCE_LIFECYCLE_RESUME)
        jce_runtime_host_policy_tick(state, state->manual_pause);
}
static inline bool jce_runtime_host_policy_begin(JceRuntimeHostPolicy *state,
    JceRuntime *runtime, bool run_in_background, bool native_window,
    int paused_fps, int background_fps)
{
    if (!state) return false;
    if (state->initialized) return true;
    memset(state, 0, sizeof(*state));
    state->runtime = runtime;
    state->run_in_background = run_in_background;
    state->native_window = native_window;
    state->running_fps = jce_engine_get_target_fps();
    state->paused_fps = paused_fps < 0 ? 0 : paused_fps;
    state->background_fps = background_fps < 0 ? 0 : background_fps;
    if (native_window) {
        state->listener = jce_lifecycle_register(jce_runtime_host_policy_event, 0, state);
        if (!state->listener.id) { memset(state, 0, sizeof(*state)); return false; }
    }
    state->prior_background = jce_engine_get_run_in_background();
    state->background_owned = true;
    state->initialized = true;
    jce_runtime_host_policy_tick(state, false);
    return true;
}
static inline bool jce_runtime_host_policy_focus_step_allowed(const JceRuntimeHostPolicy *state)
{
    return state && state->initialized && (!state->native_window ||
        (!jce_lifecycle_is_paused() &&
         (jce_lifecycle_is_focused() || state->run_in_background)));
}
static inline bool jce_runtime_host_policy_step(JceRuntimeHostPolicy *state,
                                               bool manual_pause, float *dt)
{
    if (!state || !dt || !state->initialized) return false;
    jce_runtime_host_policy_tick(state, manual_pause);
    if (state->blocked) return false;
    if (state->discard_resume_step) {
        state->discard_resume_step = false;
        *dt = 0.0f;
        return false; /* one observed resumed interval is not simulation time */
    }
    return true;
}
static inline bool jce_runtime_host_policy_draw(const JceRuntimeHostPolicy *state,
                                               bool explicit_capture)
{
    return state && state->initialized && (!state->native_window || explicit_capture ||
        (jce_lifecycle_is_focused() && !jce_lifecycle_is_paused()));
}
static inline void jce_runtime_host_policy_end(JceRuntimeHostPolicy *state)
{
    int running_fps;
    bool restore;
    JceLifecycleHandle listener;
    if (!state || !state->initialized) return;
    running_fps = state->running_fps;
    restore = state->temporary_fps && jce_engine_get_target_fps() == state->last_temporary_fps;
    listener = state->listener;
    const bool restore_background = state->background_owned &&
        jce_engine_get_run_in_background() == state->published_background;
    const bool prior_background = state->prior_background;
    /* Detach before releasing the borrowed runtime. End never resumes audio
     * for a session being destroyed and cannot receive later focus callbacks. */
    memset(state, 0, sizeof(*state));
    if (listener.id) jce_lifecycle_unregister(listener);
    if (restore_background) jce_engine_set_run_in_background(prior_background);
    if (restore) jce_engine_set_target_fps(running_fps);
}
#endif
