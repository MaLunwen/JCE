/*
 * jce_dialog_build.cpp  Shim — the Build UI lives in the dockable
 * jce_panel_build_profiles.cpp.  This shim preserves the public
 * symbol so the menu / hotkey path (cmd_build_settings_ → s_show_build)
 * keeps linking: it just flips the Build Profiles panel visible.
 */

#include "jce_editor_dialogs.h"
#include "ui/jce_editor_panels.h"

extern "C" void jce_editor_dialog_build_settings(bool *p_open)
{
    if (!p_open || !*p_open) return;

    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_BUILD_PROFILES);
    if (vis) *vis = true;

    /* One-shot: the panel owns its own visibility from here on. */
    *p_open = false;
}
