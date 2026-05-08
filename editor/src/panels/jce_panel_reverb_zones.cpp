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

/* Spawn a new entity with a default Reverb Zone component, ready for
 * the user to position via the Inspector / scene gizmo. */
static void create_default_reverb_zone(JceScene *scene)
{
    if (!scene) return;
    JceEntity e = jce_scene_create_entity(scene, "Reverb Zone");
    if (e == JCE_ENTITY_INVALID) return;

    /* Identity transform at the origin. */
    JceTransform t = {};
    t.scale.x = t.scale.y = t.scale.z = 1.0f;
    t.rotation.w = 1.0f;
    jce_scene_set_transform(scene, e, &t);

    JceAudioReverbZoneComponent rz{};
    rz.preset       = 1;       /* Generic */
    rz.min_distance = 5.0f;
    rz.max_distance = 25.0f;
    /* User preset defaults — in case user switches preset to USER. */
    rz.room               = -1000.0f;
    rz.room_hf            = -100.0f;
    rz.decay_time         = 1.49f;
    rz.decay_hf_ratio     = 0.83f;
    rz.reflections        = -2602.0f;
    rz.reflections_delay  = 0.007f;
    rz.reverb             = 200.0f;
    rz.reverb_delay       = 0.011f;
    rz.hf_reference       = 5000.0f;
    rz.diffusion          = 100.0f;
    rz.density            = 100.0f;
    jce_scene_set_audio_reverb_zone(scene, e, &rz);

    jce_state_select_entity((uint32_t)e, false);
}

extern "C" void jce_editor_panel_reverb_zones_content(void)
{
    JceScene *scene = jce_state_get_scene();
    if (!scene) { ImGui::TextDisabled(jce_editor_i18n("common.noSceneLoaded")); return; }

    std::vector<RZRow> rows;
    jce_scene_each_entity(scene, collect_cb, &rows);

    if (ImGui::Button("+ Create Zone")) create_default_reverb_zone(scene);
    ImGui::SameLine();
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
