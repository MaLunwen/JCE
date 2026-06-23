/*
 * jce_panel_preferences.h  User-level Preferences panel (P4-A.2).
 *
 * Unity-style "Edit > Preferences..." panel — user-scoped settings only
 * (theme, font size, UI scale, autosave interval, startup behaviour).
 * Project-scoped configuration lives in the Project Settings dialog.
 *
 * Persisted to ".jce/prefs.json" via jce_fs_host_* (see
 * jce_panel_preferences.cpp for the path resolution rationale).
 *
 * The "Hotkeys" sub-tab hosts the full rebinding editor (filter /
 * capture / conflict detection / reset), wired in P4-A.3.
 */

#ifndef JCE_PANEL_PREFERENCES_H
#define JCE_PANEL_PREFERENCES_H

#ifdef __cplusplus
extern "C" {
#endif

typedef enum JceEditorStartupBehavior {
    JCE_EDITOR_STARTUP_LAST = 0,
    JCE_EDITOR_STARTUP_EMPTY,
    JCE_EDITOR_STARTUP_PICKER,
    JCE_EDITOR_STARTUP_COUNT
} JceEditorStartupBehavior;

/* Load prefs.json and apply theme / font / UI scale to the live ImGui
 * context.  Safe to call before the panel has ever been opened; called
 * once from jce_editor_init() right after the panel system is up. */
void jce_editor_prefs_load_and_apply(void);

/* User-scoped startup behaviour from .jce/prefs.json. */
JceEditorStartupBehavior jce_editor_prefs_startup_behavior(void);
void jce_editor_prefs_set_startup_behavior(JceEditorStartupBehavior behavior);

/* Autosave interval in seconds (0 = disabled), driven by the General tab.
 * Polled by the editor main loop to drive a real autosave timer. */
int jce_editor_prefs_autosave_interval_sec(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PANEL_PREFERENCES_H */
