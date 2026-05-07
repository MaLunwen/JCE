/*
 * jce_panel_light_explorer.cpp  Light Explorer panel.
 *
 * Unity-style table listing every light in the active scene with quick
 * inline edits for color / intensity / shadows / enabled, plus a "ping"
 * button to focus the entity in Hierarchy + Inspector.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

struct LE_Row {
    JceEntity e;
    int       type; /* 0 dir, 1 point, 2 spot */
    jce_vec3  color;
    float     intensity;
    bool      casts_shadow;
    bool      enabled;
    char      name[64];
};

static void le_collect_cb(JceScene *s, JceEntity e, void *ud)
{
    auto *rows = (std::vector<LE_Row> *)ud;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    const char *nm = m ? m->name : "(unnamed)";
    bool en = m ? m->enabled : true;

    if (jce_scene_has_dir_light(s, e)) {
        JceDirectionalLight *l = jce_scene_get_dir_light(s, e);
        if (l) {
            LE_Row r{}; r.e = e; r.type = 0; r.color = l->color;
            r.intensity = l->intensity; r.casts_shadow = l->casts_shadow;
            r.enabled = en; snprintf(r.name, sizeof(r.name), "%s", nm);
            rows->push_back(r);
        }
    }
    if (jce_scene_has_point_light(s, e)) {
        JcePointLight *l = jce_scene_get_point_light(s, e);
        if (l) {
            LE_Row r{}; r.e = e; r.type = 1; r.color = l->color;
            r.intensity = l->intensity; r.casts_shadow = false;
            r.enabled = en; snprintf(r.name, sizeof(r.name), "%s", nm);
            rows->push_back(r);
        }
    }
    if (jce_scene_has_spot_light(s, e)) {
        JceSpotLight *l = jce_scene_get_spot_light(s, e);
        if (l) {
            LE_Row r{}; r.e = e; r.type = 2; r.color = l->color;
            r.intensity = l->intensity; r.casts_shadow = false;
            r.enabled = en; snprintf(r.name, sizeof(r.name), "%s", nm);
            rows->push_back(r);
        }
    }
}

extern "C" void jce_editor_panel_light_explorer_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { ImGui::TextDisabled("%s", jce_editor_i18n("common.noScene")); return; }

    static int filter_type = -1; /* -1 all */
    ImGui::SetNextItemWidth(150);
    ImGui::Combo(jce_editor_i18n("lightExplorer.typeFilter"), &filter_type, "All\0Directional\0Point\0Spot\0");

    std::vector<LE_Row> rows;
    rows.reserve(64);
    jce_scene_each_entity(scene, le_collect_cb, &rows);

    ImGui::Text("%s %zu", jce_editor_i18n("lightExplorer.lightsCount"), rows.size());
    ImGui::Separator();

    if (ImGui::BeginTable("##le_tbl", 7,
            ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Type", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Color", ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableSetupColumn("Intensity", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Shadows", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("Enabled", ImGuiTableColumnFlags_WidthFixed, 70);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableHeadersRow();

        const char *type_names[] = {"Dir", "Point", "Spot"};
        for (auto &r : rows) {
            if (filter_type >= 0 && r.type != filter_type) continue;
            ImGui::TableNextRow();
            ImGui::PushID((int)r.e);

            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name);
            ImGui::TableSetColumnIndex(1); ImGui::TextUnformatted(type_names[r.type]);

            ImGui::TableSetColumnIndex(2);
            float col[3] = { r.color.x, r.color.y, r.color.z };
            if (ImGui::ColorEdit3("##c", col, ImGuiColorEditFlags_NoInputs |
                                              ImGuiColorEditFlags_NoLabel)) {
                jce_vec3 nc{ col[0], col[1], col[2] };
                if (r.type == 0) { JceDirectionalLight *l = jce_scene_get_dir_light(scene, r.e); if (l) l->color = nc; }
                else if (r.type == 1) { JcePointLight *l = jce_scene_get_point_light(scene, r.e); if (l) l->color = nc; }
                else                  { JceSpotLight  *l = jce_scene_get_spot_light(scene, r.e); if (l) l->color = nc; }
            }

            ImGui::TableSetColumnIndex(3);
            float intensity = r.intensity;
            ImGui::SetNextItemWidth(-1);
            if (ImGui::DragFloat("##i", &intensity, 0.05f, 0.0f, 100.0f, "%.2f")) {
                if (r.type == 0) { JceDirectionalLight *l = jce_scene_get_dir_light(scene, r.e); if (l) l->intensity = intensity; }
                else if (r.type == 1) { JcePointLight *l = jce_scene_get_point_light(scene, r.e); if (l) l->intensity = intensity; }
                else                  { JceSpotLight  *l = jce_scene_get_spot_light(scene, r.e); if (l) l->intensity = intensity; }
            }

            ImGui::TableSetColumnIndex(4);
            if (r.type == 0) {
                bool s = r.casts_shadow;
                if (ImGui::Checkbox("##sh", &s)) {
                    JceDirectionalLight *l = jce_scene_get_dir_light(scene, r.e);
                    if (l) l->casts_shadow = s;
                }
            } else ImGui::TextDisabled("--");

            ImGui::TableSetColumnIndex(5);
            bool en = r.enabled;
            if (ImGui::Checkbox("##en", &en))
                jce_state_set_entity_enabled((uint32_t)r.e, en);

            ImGui::TableSetColumnIndex(6);
            if (ImGui::SmallButton(jce_editor_i18n("common.ping")))
                jce_state_select_entity((uint32_t)r.e, false);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
