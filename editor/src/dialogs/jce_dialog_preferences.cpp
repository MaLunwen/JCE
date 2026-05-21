/*
 * jce_dialog_preferences.cpp  Thin shim → dockable Preferences panel.
 *
 * The standalone modal was retired in P5-A.1; the dockable
 * jce_panel_preferences.cpp is now the single source of truth for
 * user-scoped preferences. This file is kept only so existing menu /
 * hotkey callers that link against `jce_editor_dialog_preferences`
 * keep resolving — it just toggles the panel visible.
 */

#include "jce_editor_dialogs.h"
#include "ui/jce_editor_panels.h"

extern "C" void jce_editor_dialog_preferences(bool *p_open)
{
    if (!p_open || !*p_open) return;

    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_USER_PREFERENCES);
    if (vis) *vis = true;

    /* One-shot: the panel owns its own visibility from here on. */
    *p_open = false;
}
