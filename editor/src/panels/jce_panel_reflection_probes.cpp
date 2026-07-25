/*
 * jce_panel_reflection_probes.cpp  Reflection Probes browser.
 *
 * Lists every entity with a JceReflectionProbeComponent. Per-row inline
 * edits for mode / resolution / intensity, plus Bake / Bake All buttons
 * (currently emit a console log; engine-side bake pipeline TBD).
 */

#include "jce_panel_common.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "core/jce_editor_i18n.h"

#include <jce/tools/jce_imgui.hpp>
#include <cstdio>
#include <vector>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/renderer/jce_reflection_probe_bake.h>
#include <jce/os/core/jce_math.h>
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

/* Submit a bake for one probe; engine rejects further submits while one
 * is in flight, so the panel-level "Bake All" walks them sequentially
 * across frames. We push onto a static queue; the next-frame visit
 * dispatches one bake whenever the previous has reached a terminal
 * state. v1 model: one-at-a-time, matches the engine's single-slot
 * worker. */
static std::vector<JceEntity> &rp_bake_queue(void)
{
    static std::vector<JceEntity> q;
    return q;
}
static JceReflectionProbeBakeHandle &rp_active_handle(void)
{
    static JceReflectionProbeBakeHandle h = 0u;
    return h;
}

static void rp_kick_next(JceScene *scene)
{
    auto &q = rp_bake_queue();
    if (q.empty()) return;
    auto &h = rp_active_handle();
    if (h != 0u) {
        JceReflectionProbeBakeProgress p{};
        bool live = jce_reflection_probe_bake_poll(h, &p);
        if (live && p.status != JCE_BAKE_STATUS_DONE &&
                    p.status != JCE_BAKE_STATUS_FAILED &&
                    p.status != JCE_BAKE_STATUS_CANCELLED) {
            return;
        }
        h = 0u;
    }
    JceEntity e = q.back(); q.pop_back();
    JceReflectionProbeComponent *p = jce_scene_get_reflection_probe(scene, e);
    if (!p) return;
    char out[256];
    snprintf(out, sizeof(out),
             "ReflectionProbes/probe_%u.ktx", (unsigned)e);
    JceReflectionProbeBakeDesc desc{};
    desc.position                = jce_v3(p->box_offset[0],
                                          p->box_offset[1],
                                          p->box_offset[2]);
    desc.cubemap_size            = (uint32_t)(p->resolution > 0 ? p->resolution : 256);
    if (desc.cubemap_size > 512u) desc.cubemap_size = 512u;
    desc.specular_mip_count      = 5u;
    desc.output_path_ktx2        = out;
    desc.include_skybox          = true;
    desc.include_dynamic_objects = true;
    h = jce_reflection_probe_bake_submit(&desc);
    if (h != 0u) snprintf(p->baked_cubemap_path,
                          sizeof p->baked_cubemap_path, "%s", out);
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
        auto &q = rp_bake_queue();
        q.clear();
        for (auto it = rows.rbegin(); it != rows.rend(); ++it) q.push_back(it->e);
        jce_editor_console_log_level(JCE_CONSOLE_INFO,
            "[ReflectionProbes] bake-all queued");
    }
    rp_kick_next(scene);
    ImGui::Separator();

    if (rows.empty()) {
        ImGui::TextWrapped("%s", jce_editor_i18n("reflectionProbes.empty"));
        return;
    }

    if (ImGui::BeginTable("##rp_tbl", 6,
            ImGuiTableFlags_Borders | ImGuiTableFlags_Resizable |
            ImGuiTableFlags_RowBg | ImGuiTableFlags_ScrollY)) {
        ImGui::TableSetupColumn(jce_editor_i18n("reflectionProbes.col.name"), ImGuiTableColumnFlags_WidthStretch);
        ImGui::TableSetupColumn(jce_editor_i18n("reflectionProbes.col.mode"), ImGuiTableColumnFlags_WidthFixed, 100);
        ImGui::TableSetupColumn(jce_editor_i18n("reflectionProbes.col.res"), ImGuiTableColumnFlags_WidthFixed, 80);
        ImGui::TableSetupColumn(jce_editor_i18n("reflectionProbes.col.intensity"), ImGuiTableColumnFlags_WidthFixed, 90);
        ImGui::TableSetupColumn(jce_editor_i18n("reflectionProbes.col.hdr"), ImGuiTableColumnFlags_WidthFixed, 50);
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
                rp_bake_queue().clear();
                rp_bake_queue().push_back(r.e);
            }
            ImGui::PopID();
        }
        ImGui::EndTable();
    }
}

/* Shim: Reflection Probes has been merged into the Lighting Settings
 * "Rendering" workbench as a tab.  Activating this panel now redirects
 * to that workbench and requests the Reflection Probes tab.  Symbol
 * kept so menu/hotkey entries registered against JCE_PANEL_REFLECTION_PROBES
 * keep working. */
extern "C" void jce_editor_panel_reflection_probes(void)
{
    if (jce_panel_redirect_to_workbench(JCE_PANEL_REFLECTION_PROBES,
                                        JCE_PANEL_LIGHTING_SETTINGS,
                                        "panel.lighting.title",
                                        "lighting_settings"))
        jce_panel_lighting_settings_request_tab(3);
}
