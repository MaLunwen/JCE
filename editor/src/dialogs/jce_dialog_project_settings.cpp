/*
 * jce_dialog_project_settings.cpp  Shim: the Project Settings UI lives in
 * the dock panel `editor/src/panels/jce_panel_project_settings.cpp`. This
 * shim preserves the public symbol so menu / hotkey paths that still call
 * `jce_editor_dialog_project_settings(&open)` continue to work — flipping
 * the panel visible flag and clearing the dialog's one-shot bool.
 */

#include "jce_editor_dialogs.h"
#include "ui/jce_editor_panels.h"

extern "C" void jce_editor_dialog_project_settings(bool *p_open)
{
    if (!p_open || !*p_open) return;
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_PROJECT_SETTINGS);
    if (vis) *vis = true;
    *p_open = false;
}
