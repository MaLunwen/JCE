#ifndef JCE_EDITOR_HOTKEY_DIAG_H
#define JCE_EDITOR_HOTKEY_DIAG_H

#include "jce_hotkeys.h"
#include <jce/os/core/jce_log.h>
#include <jce/os/platform/jce_window_event.h>
#include <cstdlib>
#include <climits>

/* Explicit opt-in calls the common logger directly so DIST INFO macro
 * stripping cannot remove diagnostics. Runtime levels and sinks still apply.
 * Observation only. This state never feeds keys, changes focus or dispatches an
 * action. Disabled capture takes no IDs, allocations, file writes or log rows. */
struct JceEditorHotkeyDiagState {
    unsigned rows = 0;
    int pending_until = -1;
    int gate_frame = -1;
    int query_frame = -1;
};
inline bool jce_editor_hotkey_diag_event_take(JceEditorHotkeyDiagState &state,
    bool enabled, bool preference_down, int frame)
{
    if (!enabled || state.rows >= 32) return false;
    if (preference_down && frame >= 0)
        state.pending_until = frame > INT_MAX - 8 ? INT_MAX : frame + 8;
    ++state.rows;
    return true;
}
inline bool jce_editor_hotkey_diag_sample_take(JceEditorHotkeyDiagState &state,
    bool enabled, bool query, int frame)
{
    if (!enabled || state.rows >= 32 || frame < 0 || frame > state.pending_until)
        return false;
    int &previous = query ? state.query_frame : state.gate_frame;
    if (previous == frame) return false;
    previous = frame;
    ++state.rows;
    return true;
}
inline bool jce_editor_hotkey_diag_enabled()
{
    static const bool enabled = [] {
        const char *value = std::getenv("JCE_EDITOR_HOTKEY_DIAG");
        return value && value[0] == '1' && value[1] == '\0';
    }();
    return enabled;
}
inline JceEditorHotkeyDiagState &jce_editor_hotkey_diag_state()
{
    static JceEditorHotkeyDiagState state;
    return state;
}
inline void jce_editor_hotkey_diag_event(const JceEvent &event, int mapped,
    int frame, bool initialized, bool active)
{
    if (!jce_editor_hotkey_diag_enabled()) return;
    const auto binding = jce_hotkey_get(JCE_HK_EDIT_PREFERENCES);
    const bool relevant = event.key.scancode == JCE_KEY_LCTRL ||
        event.key.scancode == JCE_KEY_RCTRL || event.key.scancode == JCE_KEY_COMMA ||
        (binding.key > 0 && mapped == binding.key);
    if (!relevant) return;
    const bool down = event.type == JCE_EVENT_KEY_DOWN;
    if (!jce_editor_hotkey_diag_event_take(jce_editor_hotkey_diag_state(), true,
            down && mapped == binding.key, frame)) return;
    jce_log_write(JCE_LOG_LEVEL_INFO, "editor-input", __FILE__, __LINE__, "hotkey_diag event frame=%d scancode=%d down=%d mods=%u mapped=%d initialized=%d active=%d",
        frame, (int)event.key.scancode, down ? 1 : 0, (unsigned)event.key.mod,
        mapped, initialized ? 1 : 0, active ? 1 : 0);
}
inline void jce_editor_hotkey_diag_gate(int frame, const char *reason, int mods)
{
    if (!jce_editor_hotkey_diag_sample_take(jce_editor_hotkey_diag_state(),
            jce_editor_hotkey_diag_enabled(), false, frame)) return;
    jce_log_write(JCE_LOG_LEVEL_INFO, "editor-input", __FILE__, __LINE__, "hotkey_diag gate frame=%d reason=%s imgui_mods=%d",
        frame, reason, mods);
}
inline void jce_editor_hotkey_diag_query(int frame, JceHotkeyChord binding,
    int mods, bool edge, bool chord, bool consumed)
{
    if (!jce_editor_hotkey_diag_sample_take(jce_editor_hotkey_diag_state(),
            jce_editor_hotkey_diag_enabled(), true, frame)) return;
    jce_log_write(JCE_LOG_LEVEL_INFO, "editor-input", __FILE__, __LINE__, "hotkey_diag query frame=%d key=%d binding_mods=%u imgui_mods=%d edge=%d chord=%d consumed=%d",
        frame, binding.key, (unsigned)binding.mods, mods, edge ? 1 : 0,
        chord ? 1 : 0, consumed ? 1 : 0);
}
#endif
