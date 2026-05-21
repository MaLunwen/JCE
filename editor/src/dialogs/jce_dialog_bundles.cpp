/*
 * jce_dialog_bundles.cpp  Shim — the bundle Build UI lives in the
 * dockable jce_panel_bundle_browser.cpp (Build tab). This shim
 * preserves the public symbols so menu / hotkey paths keep linking:
 *   - jce_editor_dialog_bundles(&open)             → show + focus Build tab
 *   - jce_editor_dialog_bundles_open_for_current_scene()
 *                                                  → queue current scene
 *                                                    for single-scene pack
 */

#include "jce_editor_dialogs.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"

extern "C" {
void jce_editor_panel_bundle_browser_show_build(void);
bool jce_editor_panel_bundle_browser_request_pack_scene(const char *scene_path);
void jce_editor_console_log(const char *fmt, ...);
}

extern "C" void jce_editor_dialog_bundles(bool *p_open)
{
    if (!p_open || !*p_open) return;
    jce_editor_panel_bundle_browser_show_build();
    *p_open = false;
}

extern "C" bool jce_editor_dialog_bundles_open_for_current_scene(void)
{
    const char *spath = jce_state_get_current_scene_path();
    if (!spath || !spath[0]) {
        jce_editor_console_log("[bundle] no current scene to pack");
        return false;
    }
    return jce_editor_panel_bundle_browser_request_pack_scene(spath);
}
