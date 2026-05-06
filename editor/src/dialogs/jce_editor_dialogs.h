/*
 * jce_editor_dialogs.h  Project dialog declarations.
 *
 * Matches reference Java editor dialogs: New Project, Open Project,
 * Save As, Unsaved Changes.
 */

#ifndef JCE_EDITOR_DIALOGS_H
#define JCE_EDITOR_DIALOGS_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* New Project dialog. */
void jce_editor_dialog_new_project(bool *p_open);

/* Open Project dialog. */
void jce_editor_dialog_open_project(bool *p_open);
/* Pre-fill the Open Project dialog's path field; safe to call before
 * the dialog is opened. Used by the File > Open Recent Project menu
 * to seed the path from a recents entry. */
void jce_editor_dialog_open_project_set_path(const char *path);

/* Save As dialog. */
void jce_editor_dialog_save_as(bool *p_open);

/* New Scene dialog. */
void jce_editor_dialog_new_scene(bool *p_open);

/* Open Scene dialog. */
void jce_editor_dialog_open_scene(bool *p_open);

/* Unsaved Changes dialog.
   result: 0 = pending, 1 = save, 2 = don't save, 3 = cancel. */
void jce_editor_dialog_unsaved_changes(bool *p_open, int *result);

/* Build Settings dialog (CMake preset launcher). */
void jce_editor_dialog_build_settings(bool *p_open);

/* Project Settings dialog (centralized: project / build / run / render / hotkeys). */
void jce_editor_dialog_project_settings(bool *p_open);

/* Preferences dialog (global per-user: general / fonts / editor / input / paths). */
void jce_editor_dialog_preferences(bool *p_open);

/* Pump pending host-dialog callback results onto the main thread.  Must be
   called once per frame from the editor's main loop.  This drains the
   thread-safe queue populated by SDL's worker-thread folder/file picker
   callbacks and applies the results safely (no data race with ImGui). */
void jce_editor_dialogs_pump_pending(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_DIALOGS_H */
