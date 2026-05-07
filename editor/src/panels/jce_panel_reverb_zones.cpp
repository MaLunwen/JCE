/*
 * jce_panel_reverb_zones.cpp  Audio Reverb Zone Explorer.
 *
 * Lists every entity carrying a JceAudioReverbZoneComponent so the
 * user can audit and tune all reverb zones in one place. Inline edit
 * preset / min_distance / max_distance; Ping selects the entity.
 */

#include "ui/jce_editor_panels.h"
#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
}

namespace {

struct RZRow {
    JceEntity                    e;
    JceAudioReverbZoneComponent *c;
    char                         name[64];
};

const char *kPresetNames[] = {
    "Off","Generic","Padded Cell","Room","Bathroom","Living Room","Stone Room",
    "Auditorium","Concert Hall","Cave","Arena","Hangar","Hallway",
    "Stone Corridor","Alley","Forest","City","Mountains","Quarry","Plain",
    "Parking Lot","Sewer Pipe","Underwater","-","-","-","User"
};

void collect_cb(JceScene *s, JceEntity e, void *ud)
{
    auto *out = (std::vector<RZRow> *)ud;
    if (!jce_scene_has_audio_reverb_zone(s, e)) return;
    RZRow r{};
    r.e = e;
    r.c = jce_scene_get_audio_reverb_zone(s, e);
    JceEditorMeta *m = jce_scene_get_editor_meta(s, e);
    snprintf(r.name, sizeof(r.name), "%s",
             (m && m->name[0]) ? m->name : "(unnamed)");
    out->push_back(r);
}

} /* namespace */

extern "C" void jce_editor_panel_reverb_zones_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { ImGui::TextDisabled(jce_editor_i18n("common.noSceneLoaded")); return; }

    std::vector<RZRow> rows;
    jce_scene_each_entity(scene, collect_cb, &rows);

    ImGui::Text("%s %zu", jce_editor_i18n("reverbZones.count"), rows.size());
    ImGui::Separator();
    if (rows.empty()) {
        ImGui::TextDisabled(jce_editor_i18n("reverbZones.empty"));
        return;
    }

    if (ImGui::BeginTable("##rz_tbl", 5,
            ImGuiTableFlags_Borders | ImGuiTableFlags_RowBg |
            ImGuiTableFlags_ScrollY | ImGuiTableFlags_Resizable)) {
        ImGui::TableSetupColumn("Owner",   ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn("Preset",  ImGuiTableColumnFlags_WidthFixed, 140);
        ImGui::TableSetupColumn("Min",     ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("Max",     ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn("",        ImGuiTableColumnFlags_WidthFixed, 60);
        ImGui::TableHeadersRow();

        for (auto &r : rows) {
            if (!r.c) continue;
            ImGui::PushID((int)r.e);
            ImGui::TableNextRow();
            ImGui::TableSetColumnIndex(0); ImGui::TextUnformatted(r.name);

            ImGui::TableSetColumnIndex(1);
            ImGui::SetNextItemWidth(-1);
            int idx = r.c->preset;
            int n = (int)(sizeof(kPresetNames)/sizeof(kPresetNames[0]));
            if (idx < 0 || idx >= n) idx = 0;
            if (ImGui::Combo("##pre", &idx, kPresetNames, n)) r.c->preset = idx;

            ImGui::TableSetColumnIndex(2);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##mn", &r.c->min_distance, 0.1f, 0.0f, 100000.0f, "%.1f");

            ImGui::TableSetColumnIndex(3);
            ImGui::SetNextItemWidth(-1);
            ImGui::DragFloat("##mx", &r.c->max_distance, 0.1f, 0.0f, 100000.0f, "%.1f");

            ImGui::TableSetColumnIndex(4);
            if (ImGui::SmallButton(jce_editor_i18n("common.ping")))
                jce_state_select_entity((uint32_t)r.e, false);

            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}
