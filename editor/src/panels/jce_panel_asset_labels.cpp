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
#include "core/jce_editor_asset_label_map.h"

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

    ImGui::Separator();
    /* Per-asset tagging.  User pastes (or drags-into) an asset path
     * and toggles which registered labels apply.  Editor-side map is
     * persisted to <project>/.jce/asset_labels.json so it round-trips
     * across editor sessions. */
    ImGui::TextWrapped("Tag a specific asset:");
    static char asset_path[512] = {0};
    ImGui::InputText("Asset path", asset_path, sizeof(asset_path));
    if (asset_path[0] && n > 0) {
        uint64_t bits = jce_editor_asset_labels_get(asset_path);
        bool dirty = false;
        for (uint32_t i = 0; i < n; ++i) {
            const char *lbl = jce_asset_label_name(i);
            if (!lbl) continue;
            uint64_t bit = ((uint64_t)1u << i);
            bool on = (bits & bit) != 0;
            if (ImGui::Checkbox(lbl, &on)) {
                bits = on ? (bits | bit) : (bits & ~bit);
                dirty = true;
            }
        }
        if (dirty) jce_editor_asset_labels_set(asset_path, bits);
        ImGui::TextDisabled("Mask: 0x%llx", (unsigned long long)bits);
    } else if (n == 0) {
        ImGui::TextDisabled("(register at least one label first)");
    }

    ImGui::Separator();
    if (ImGui::Button("Save Map"))   { jce_editor_asset_labels_save(); }
    ImGui::SameLine();
    if (ImGui::Button("Reload Map")) { jce_editor_asset_labels_load(); }
    ImGui::SameLine();
    ImGui::TextDisabled("(%u entries)",
                        jce_editor_asset_labels_entry_count());
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
