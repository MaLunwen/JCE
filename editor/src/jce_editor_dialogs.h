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

/* Save As dialog. */
void jce_editor_dialog_save_as(bool *p_open);

/* Unsaved Changes dialog.
   result: 0 = pending, 1 = save, 2 = don't save, 3 = cancel. */
void jce_editor_dialog_unsaved_changes(bool *p_open, int *result);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_DIALOGS_H */
