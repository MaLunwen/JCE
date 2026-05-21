/*
 * jce_panel_time_of_day.cpp  Shim (P6-A.2).
 *
 * The Time-of-Day controls have been merged into the Lighting Settings
 * panel as a "Time of Day" tab (see jce_panel_lighting_settings.cpp).
 * This file preserves the public `_content` symbol so existing menu /
 * hotkey / layout callers still link — invoking it focuses the Lighting
 * Settings panel with the Time of Day tab pre-selected and hides the
 * legacy window.
 */

#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" void jce_editor_lighting_settings_focus_tab_time_of_day(void);

extern "C" void jce_editor_panel_time_of_day_content(void)
{
    bool *self = jce_editor_panel_visible_ptr(JCE_PANEL_TIME_OF_DAY);
    bool *host = jce_editor_panel_visible_ptr(JCE_PANEL_LIGHTING_SETTINGS);
    if (host) *host = true;
    if (self) *self = false;
    jce_editor_lighting_settings_focus_tab_time_of_day();
    ImGui::SetWindowFocus("###lighting_settings");
}
