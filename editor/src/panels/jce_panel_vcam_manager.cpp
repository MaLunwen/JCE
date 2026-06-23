/*
 * jce_panel_vcam_manager.cpp  Virtual Camera Manager (Cinemachine Brain).
 *
 * Lists all entities in the scene that carry a JCE_COMP_FLAG_VIRTUAL_CAMERA
 * component, sorted by priority descending. Highest-priority active vcam
 * wins (matches engine vcam manager semantics). Inline edit priority and
 * Active flag; Ping selects the entity in Hierarchy/Inspector.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>
#include <algorithm>
#include <cstdio>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/middleware/scene/jce_virtual_camera.h>
}

namespace {

struct VcamRow {
    JceEntity                  e;
    JceVirtualCameraComponent *c;
    char                       owner_name[64];
};

void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    auto *out = (std::vector<VcamRow> *)ud;
    if (!jce_scene_has_virtual_camera(s, e)) return;
    VcamRow r{};
    r.e = e;
    r.c = jce_scene_get_virtual_camera(s, e);
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    snprintf(r.owner_name, sizeof(r.owner_name), "%s",
             (m && m->name[0]) ? m->name : "(unnamed)");
    out->push_back(r);
}

} /* namespace */

extern "C" void jce_editor_panel_vcam_manager_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { ImGui::TextDisabled(jce_editor_i18n("common.noSceneLoaded")); return; }

    std::vector<VcamRow> rows;
    jce_scene_each_entity(scene, collect_cb, &rows);
    std::sort(rows.begin(), rows.end(),
              [](const VcamRow &a, const VcamRow &b) {
                  int pa = a.c ? a.c->priority : INT32_MIN;
                  int pb = b.c ? b.c->priority : INT32_MIN;
                  return pa > pb;
              });

    ImGui::Text("%s %zu", jce_editor_i18n("vcamManager.count"), rows.size());
    ImGui::TextDisabled(jce_editor_i18n("vcamManager.priorityHint"));
    ImGui::Separator();

    if (rows.empty()) {
        ImGui::TextDisabled(jce_editor_i18n("vcamManager.empty"));
        return;
    }

    if (ImGui::BeginTable("##vcam_tbl", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn(jce_editor_i18n("vcamManager.col.owner"),    ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("vcamManager.col.vcam"),     ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("vcamManager.col.priority"), ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(jce_editor_i18n("vcamManager.col.active"),   ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn(jce_editor_i18n("vcamManager.col.fov"),      ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("",         ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableHeadersRow();

        bool first_active = true;
        for (auto &r : rows) {
            if (!r.c) continue;
            ImGui::PushID((int)r.e);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0);
            if (first_active && r.c->active) {
                ImGui::TextColored(ImVec4(0.4f,1.0f,0.4f,1.0f), "* %s", r.owner_name);
                first_active = false;
            } else {
                ImGui::TextUnformatted(r.owner_name);
            }

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            ImGui::InputText("##nm", r.c->vcam_name, sizeof(r.c->vcam_name));

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragInt("##pr", &r.c->priority, 1.0f, -1000, 1000);

            ImGui::TableSetColumnIndex(3);
            bool act = r.c->active;
            if (ImGui::Checkbox("##ac", &act)) r.c->active = act;

            ImGui::TableSetColumnIndex(4);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##fov", &r.c->fov_deg, 0.1f, 1.0f, 179.0f, "%.1f");

            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton(jce_editor_i18n("common.ping")))
                jce_state_select_entity((uint32_t)r.e, false);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
