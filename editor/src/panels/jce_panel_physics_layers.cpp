/*
 * jce_panel_physics_layers.cpp  Shim (P6-A.4).
 *
 * Physics layer name + collision matrix editing has been folded into the
 * Project Settings panel under the Physics tab (it already hosts
 * `draw_layer_collision_matrix(...)`). This file preserves the public
 * `_content` / panel symbols so existing menu / hotkey / layout callers
 * still link — invoking either focuses Project Settings with the Physics
 * tab pre-selected and hides the legacy window.
 */

#include "ui/jce_editor_panels.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" void jce_editor_project_settings_focus_tab_physics(void);

extern "C" void jce_editor_panel_physics_layers_content(void)
{
    bool *self = jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_LAYERS);
    bool *host = jce_editor_panel_visible_ptr(JCE_PANEL_PROJECT_SETTINGS);
    if (host) *host = true;
    if (self) *self = false;
    jce_editor_project_settings_focus_tab_physics();
    ImGui::SetWindowFocus("###project_settings");
}

extern "C" void jce_editor_panel_physics_layers(void)
{
    bool *visible = jce_editor_panel_visible_ptr(JCE_PANEL_PHYSICS_LAYERS);
    if (!visible || !*visible) return;
    /* Trigger the redirect via the standard content path. */
    jce_editor_panel_physics_layers_content();
}
