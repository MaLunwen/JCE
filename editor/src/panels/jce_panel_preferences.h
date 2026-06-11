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

/* Load prefs.json and apply theme / font / UI scale to the live ImGui
 * context.  Safe to call before the panel has ever been opened; called
 * once from jce_editor_init() right after the panel system is up. */
void jce_editor_prefs_load_and_apply(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PANEL_PREFERENCES_H */
