/*
 * jce_panel_reflection_probes.cpp  Reflection Probes browser.
 *
 * Lists every entity with a JceReflectionProbeComponent. Per-row inline
 * edits for mode / resolution / intensity, plus Bake / Bake All buttons
 * (currently emit a console log; engine-side bake pipeline TBD).
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

struct RP_Row { JceEntity e; char name[64]; };

static void rp_collect_cb(JceScene *s, JceEntity e, void *ud)
{
    if (!jce_scene_has_reflection_probe(s, e)) return;
    auto *rows = (std::vector<RP_Row> *)ud;
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    RP_Row r{}; r.e = e;
    snprintf(r.name, sizeof(r.name), "%s", m && m->name[0] ? m->name : "(unnamed)");
    rows->push_back(r);
}

extern "C" void jce_editor_panel_reflection_probes_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { ImGui::TextDisabled(jce_editor_i18n("common.noScene")); return; }

    std::vector<RP_Row> rows; rows.reserve(16);
    jce_scene_each_entity(scene, rp_collect_cb, &rows);

    ImGui::Text("%s %zu", jce_editor_i18n("reflectionProbes.probesCount"), rows.size());
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("reflectionProbes.bakeAll"))) {
        for (auto &r : rows) {
            char buf[128];
            snprintf(buf, sizeof(buf), "[ReflectionProbes] bake requested for '%s'", r.name);
            jce_editor_console_log_level(JCE_CONSOLE_INFO, buf);
        }
    }
    ImGui::Separator();

    if (rows.empty()) {
        ImGui::TextWrapped("%s", jce_editor_i18n("reflectionProbes.empty"));
        return;
    }

    if (ImGui::BeginTable("##rp_tbl", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn("Name", ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Mode", ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn("Res", ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn("Intensity", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("HDR", ImGuiTableColumnFlags_WidthFixed, 50);
        ImGui::TableSetupColumn("", ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableHeadersRow();

        for (auto &r : rows) {
            JceReflectionProbeComponent *p = jce_scene_get_reflection_probe(scene, r.e);
            if (!p) continue;
            ImGui::TableNextRow();
            ImGui::PushID((int)r.e);

            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name);
            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            {
                static const char *kModes[] = {
                    "reflectionProbes.mode.baked",
                    "reflectionProbes.mode.realtime",
                    "reflectionProbes.mode.custom"
                };
                ImGui::Combo("##mode", &p->mode, jce_editor_i18n_combo(kModes, 3));
            }
            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            int res = p->resolution;
            const char *res_items = "16\0""32\0""64\0""128\0""256\0""512\0""1024\0";
            int idx = 0;
            for (int v = 16, k = 0; k < 7; v *= 2, k++) if (v == res) { idx = k; break; }
            if (ImGui::Combo("##res", &idx, res_items)) p->resolution = 16 << idx;
            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##i", &p->intensity, 0.05f, 0.0f, 8.0f, "%.2f");
            ImGui::TableSetColumnIndex(4);
            ImGui::Checkbox("##hdr", &p->hdr);
            ImGui::TableSetColumnIndex(5);
            if (ImGui::SmallButton(jce_editor_i18n("common.ping"))) jce_state_select_entity((uint32_t)r.e, false);
            ImGui::SameLine();
            if (ImGui::SmallButton(jce_editor_i18n("reflectionProbes.bake"))) {
                char buf[128];
                snprintf(buf, sizeof(buf), "[ReflectionProbes] bake requested for '%s'", r.name);
                jce_editor_console_log_level(JCE_CONSOLE_INFO, buf);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
