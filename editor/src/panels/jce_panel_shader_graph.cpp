/*
 * jce_panel_shader_graph.cpp  Shader Graph panel.
 *
 * JCE's node-based shader authoring shares infrastructure with the
 * existing Material Graph panel. This panel is a thin shell that
 * documents the relationship and offers a one-click jump.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

extern "C" void jce_editor_panel_shader_graph_content(void)
{
    ImGui::TextWrapped("%s", jce_editor_i18n("shaderGraph.intro"));
    ImGui::Spacing();
    if (ImGui::Button(jce_editor_i18n("shaderGraph.openMaterialGraph"))) {
        bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH);
        if (vis) *vis = true;
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::TextDisabled("%s", jce_editor_i18n("shaderGraph.future1"));
    ImGui::TextDisabled("%s", jce_editor_i18n("shaderGraph.future2"));
}
