/*
 * jce_panel_light_explorer.cpp  Shim (P6-A.3).
 *
 * The Light Explorer table has been merged into the Lighting Settings
 * panel as a "Light Explorer" tab (see jce_panel_lighting_settings.cpp).
 * This file preserves the public `_content` symbol so existing menu /
 * hotkey / layout callers still link — invoking it focuses the Lighting
 * Settings panel with the Light Explorer tab pre-selected and hides the
 * legacy window.
 */

#include "ui/jce_editor_panels.h"

extern "C" void jce_editor_lighting_settings_focus_tab_light_explorer(void);

extern "C" void jce_editor_panel_light_explorer_content(void)
{
    bool *self = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHT_EXPLORER);
    bool *host = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
    if (host) *host = true;
    if (self) *self = false;
    jce_editor_lighting_settings_focus_tab_light_explorer();
    jce_editor_panel_request_focus("###lighting_settings");
}
