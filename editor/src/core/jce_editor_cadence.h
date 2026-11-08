#ifndef JCE_EDITOR_CADENCE_H
#define JCE_EDITOR_CADENCE_H
#include "jce_editor_config.h"
#include "jce_editor_frame_rate.h"
#include <jce/application/jce_engine.h>
#include <jce/os/platform/jce_input.h>
#include <jce/os/platform/jce_window.h>
#include <jce/os/platform/jce_window_event.h>

/* Editor preference rates. Authoring writes only outside Play; the existing
 * Play transaction composes its preview cap. The engine owns the one
 * sleep/deadline mechanism. No renderer or clock owner. */
struct JceEditorCadenceRates {
    int active = 60, idle = 30, background = 5, play_background = 15, play = 60;
};
static constexpr const char *JCE_EDITOR_CADENCE_ACTIVE = "editor.frame_rate.active";
static constexpr const char *JCE_EDITOR_CADENCE_IDLE = "editor.frame_rate.idle";
static constexpr const char *JCE_EDITOR_CADENCE_PLAY = "editor.frame_rate.play";
static constexpr const char *JCE_EDITOR_CADENCE_BACKGROUND = "editor.frame_rate.background";
inline int jce_editor_cadence_rate(int value, int fallback)
{
    return value >= 0 && value <= 1000 ? value : fallback;
}
inline JceEditorCadenceRates jce_editor_cadence_rates(const JceEditorConfig &cfg)
{
    const JceEditorCadenceRates defaults;
    return {
        jce_editor_cadence_rate(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_ACTIVE, defaults.active), defaults.active),
        jce_editor_cadence_rate(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_IDLE, defaults.idle), defaults.idle),
        jce_editor_cadence_rate(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, defaults.background), defaults.background),
        jce_editor_cadence_rate(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_BACKGROUND, defaults.play_background), defaults.play_background),
        jce_editor_cadence_rate(jce_editor_config_get_ui_int_or(&cfg, JCE_EDITOR_CADENCE_PLAY, defaults.play), defaults.play)
    };
}
enum class JceEditorCadenceMode { Active, Idle, Background, Suspended };
struct JceEditorCadence {
    uint64_t last_activity = 0, frequency = 0;
    int previous_fps = 0, last_applied = -1;
    bool initialized = false, suspended = false, was_focused = false;
    JceEditorCadenceMode mode = JceEditorCadenceMode::Active;
};
inline bool jce_editor_cadence_input_held(const JceInputFrame &input)
{
    if (input.mouse_buttons || input.touch_count) return true;
    for (const auto bits : input.keys_bits) if (bits) return true;
    return false;
}
inline bool jce_editor_cadence_input_event(JceEventType type)
{
    switch (type) {
    case JCE_EVENT_KEY_DOWN: case JCE_EVENT_KEY_UP: case JCE_EVENT_TEXT_INPUT:
    case JCE_EVENT_MOUSE_MOTION: case JCE_EVENT_MOUSE_BUTTON_DOWN:
    case JCE_EVENT_MOUSE_BUTTON_UP: case JCE_EVENT_MOUSE_WHEEL:
    case JCE_EVENT_WINDOW_FOCUS_GAINED: case JCE_EVENT_WINDOW_RESIZED:
    case JCE_EVENT_WINDOW_EXPOSED: return true;
    default: return false;
    }
}
inline void jce_editor_cadence_activity(JceEditorCadence &state, uint64_t now)
{
    state.last_activity = now;
}
inline void jce_editor_cadence_apply(JceEditorCadence &state, int target)
{
    if (jce_engine_get_target_fps() != target) {
        jce_engine_set_target_fps(target);
    }
    state.last_applied = target;
}
inline void jce_editor_cadence_begin(JceEditorCadence &state,
                                    const JceEditorCadenceRates &rates,
                                    uint64_t now, uint64_t frequency)
{
    if (state.initialized) return;
    state = {};
    state.initialized = true;
    state.previous_fps = jce_engine_get_target_fps();
    state.last_activity = now;
    state.frequency = frequency;
    jce_editor_cadence_apply(state, rates.active);
}
inline void jce_editor_cadence_tick(JceEditorCadence &state,
                                   const JceEditorCadenceRates &rates,
                                   uint64_t now, uint64_t frequency,
                                   uint32_t window_flags, bool flags_valid,
                                   bool held_input, bool simulation)
{
    if (!state.initialized) return;
    if (simulation) {
        state.suspended = true;
        state.mode = JceEditorCadenceMode::Suspended;
        state.last_activity = now;
        return; /* The existing Play/Pause transaction owns public target FPS. */
    }
    const bool focused = flags_valid && (window_flags & JCE_WINDOW_INPUT_FOCUS) &&
        !(window_flags & (JCE_WINDOW_HIDDEN | JCE_WINDOW_MINIMIZED));
    if (state.suspended || held_input || now < state.last_activity ||
        frequency != state.frequency || (focused && !state.was_focused))
        state.last_activity = now;
    state.suspended = false;
    state.frequency = frequency;
    state.was_focused = focused;
    const bool idle = frequency && frequency <= UINT64_MAX / 2u &&
        now >= state.last_activity && now - state.last_activity >= frequency * 2u;
    state.mode = !focused ? JceEditorCadenceMode::Background :
                 idle ? JceEditorCadenceMode::Idle : JceEditorCadenceMode::Active;
    /* Inactive preferences add bounds; they cannot lift a lower active/idle cap. */
    int target = idle ? jce_editor_frame_rate_limit(rates.active, rates.idle) : rates.active;
    if (!focused) target = jce_editor_frame_rate_limit(target, rates.background);
    jce_editor_cadence_apply(state, target);
}
inline void jce_editor_cadence_end(JceEditorCadence &state)
{
    if (!state.initialized) return;
    /* Only release our own last value. An active Play transaction must Stop first. */
    if (!state.suspended && jce_engine_get_target_fps() == state.last_applied)
        jce_engine_set_target_fps(state.previous_fps);
    state = {};
}
#endif
