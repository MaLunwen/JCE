/*
 * jce_panel_shader_graph.cpp  Shader Graph panel.
 *
 * JCE's node-based shader authoring shares infrastructure with the
 * existing Material Graph panel. This panel is a thin shell that
 * documents the relationship and offers a one-click jump.
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstdio>

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

/* Shim: Shader Graph has been merged into the Material Graph
 * "Graph Authoring" workbench as a tab.  Activating this panel now
 * redirects to that workbench and requests the Shader tab.  Symbol
 * kept so menu/hotkey entries registered against JCE_PANEL_SHADER_GRAPH
 * keep working. */
extern "C" void jce_editor_panel_shader_graph(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_SHADER_GRAPH,
                                        JCE_PANEL_MATERIAL_GRAPH,
                                        "materialGraph.title",
                                        "jce_material_graph"))
        jce_panel_material_graph_request_tab(1);
}
