/*
 * jce_dialog_open_bundle.cpp  Shim — the Open-Bundle UI lives in the
 * dockable jce_panel_bundle_browser.cpp (Open tab). This shim preserves
 * the public symbol so the menu / hotkey path keeps linking.
 */

#include "jce_editor_dialogs.h"
#include "ui/jce_editor_panels.h"

extern "C" {
void jce_editor_panel_bundle_browser_show_open(void);
}

extern "C" void jce_editor_dialog_open_bundle(bool *p_open)
{
    if (!p_open || !*p_open) return;
    jce_editor_panel_bundle_browser_show_open();
    *p_open = false;
}
