/*
 * jce_panel_asset_labels.cpp  Minimal Asset Labels management panel.
 *
 * Lists every label registered via jce_asset_label_bit() in the
 * project and lets the user create new ones.  Tagging individual
 * assets with these labels lives elsewhere (file_viewer / assets
 * panel) — this panel is the project-wide "label dictionary".
 *
 * Wired via the editor plugin API so it shows up under Window >
 * Plugins automatically.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_plugin.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <cstring>

extern "C" {
#include <jce/resource/jce_asset_labels.h>
}

namespace {

void asset_labels_panel_content(void)
{
    ImGui::TextWrapped("Project-wide asset labels (Unity Addressables-style). "
                       "Code uses jce_asset_label_bit(\"name\") to get a "
                       "bitmask the runtime registry can query.");
    ImGui::Separator();

    uint32_t n = jce_asset_label_count();
    ImGui::Text("Registered: %u / %u", n, JCE_ASSET_LABEL_MAX);
    if (n == 0) {
        ImGui::TextDisabled("(none yet)");
    } else {
        if (ImGui::BeginTable("##labels", 2,
                ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg)) {
            ImGui::TableSetupColumn("Bit", ImGuiTableColumnFlags_WidthFixed, 50);
            ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
            ImGui::TableHeadersRow();
            for (uint32_t i = 0; i < n; ++i) {
                const char *nm = jce_asset_label_name(i);
                if (!nm) continue;
                ImGui::TableNextRow();
                ImGui::TableSetColumnIndex(0); ImGui::Text("%u", i);
                ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(nm);
            }
            ImGui::EndTable();
        }
    }

    ImGui::Separator();
    static char new_label[32] = {0};
    ImGui::InputText("##new", new_label, sizeof(new_label));
    ImGui::SameLine();
    bool can_add = new_label[0] && n < JCE_ASSET_LABEL_MAX;
    if (!can_add) ImGui::BeginDisabled();
    if (ImGui::Button("Register Label")) {
        jce_asset_label_bit(new_label);
        new_label[0] = '\0';
    }
    if (!can_add) ImGui::EndDisabled();
    if (n >= JCE_ASSET_LABEL_MAX) {
        ImGui::TextDisabled("Registry full (cap = %u).", JCE_ASSET_LABEL_MAX);
    }
}

} /* namespace */

extern "C" void jce_editor_register_asset_labels_panel(void)
{
    JceEditorPluginPanel desc = {};
    desc.id              = "jce.assets.labels";
    desc.display_name    = "Asset Labels";
    desc.content_fn      = asset_labels_panel_content;
    desc.default_visible = false;
    jce_editor_plugin_register_panel(&desc);
}
