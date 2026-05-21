/*
 * inspector_vfx.cpp
 * Inspector drawer for the VFX graph component (split from
 * inspector_vfx_tilemap.cpp in P6-A.5).
 */

#include "jce_panel_inspector_common.h"

void draw_comp_vfx_graph(JceVfxGraphComponent *v)
{
    if (!v) return;
    jce_draw_path_input_asset(jce_editor_i18n("inspector.vfx.graphPath"),
                              v->graph_path, sizeof v->graph_path, JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(v->graph_path, sizeof v->graph_path);

    if (ImGui::Checkbox(jce_editor_i18n("inspector.vfx.playOnAwake"), &v->play_on_awake))
        insp_undo_bool(&v->play_on_awake);
    if (ImGui::Checkbox(jce_editor_i18n("inspector.vfx.loop"), &v->loop))
        insp_undo_bool(&v->loop);

    if (v->rate_multiplier <= 0.0f) v->rate_multiplier = 1.0f;
    ImGui::DragFloat(jce_editor_i18n("inspector.vfx.rateMultiplier"),
                     &v->rate_multiplier, 0.01f, 0.0f, 10.0f, "%.2f");
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.vfx.intensity"),
                     &v->intensity, 0.01f, 0.0f, 8.0f, "%.2f");
    insp_track_edit();
}
