/*
 * jce_panel_inspector_multi_select.cpp
 *
 * Multi-entity inspector view: shown whenever the editor's selection
 * contains > 1 entity.  Surfaces shared toggles (enable / tag color),
 * a bulk Transform editor, the selected-entities list, and a delete
 * button.  The caller then FALLS THROUGH to draw the focused entity's
 * per-component sections, broadcasting each per-field edit to the other
 * selected peers (see jce_panel_inspector.cpp), so this view no longer
 * suppresses the per-entity body.
 *
 * Carved out of jce_panel_inspector.cpp; entry point is
 * `insp_draw_multi_select_view(scene)`.  It returns true when it rendered
 * the multi view — now informational only; the sole caller ignores the
 * return value and always continues to the per-component sections.
 */

#include "jce_panel_inspector_common.h"

bool insp_draw_multi_select_view(JceScene *scene)
{
    int             sel_count = 0;
    const uint32_t *sel_ids   = jce_state_get_selection(&sel_count);

    if (sel_count <= 1) return false;

    char lbl[128];

    ImGui::TextColored(ImVec4(0.6f, 0.8f, 1.0f, 1.0f),
                       "%d %s", sel_count, jce_editor_i18n("inspector.entitiesSelected"));
    ImGui::Separator();

    bool all_enabled = true, all_disabled = true;
    for (int i = 0; i < sel_count; i++) {
        if (!jce_state_entity_exists(sel_ids[i])) continue;
        bool en = jce_state_entity_enabled(sel_ids[i]);
        if (en)  all_disabled = false;
        else     all_enabled  = false;
    }

    bool mixed = (!all_enabled && !all_disabled);
    bool chk = all_enabled;
    if (mixed) {
        ImGui::Text("%s", jce_editor_i18n("inspector.enabledMixed"));
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("inspector.enableAll"))) {
            jce_state_begin_batch_edit();
            for (int i = 0; i < sel_count; i++)
                jce_state_set_entity_enabled(sel_ids[i], true);
            jce_state_end_batch_edit();
        }
        ImGui::SameLine();
        if (ImGui::SmallButton(jce_editor_i18n("inspector.disableAll"))) {
            jce_state_begin_batch_edit();
            for (int i = 0; i < sel_count; i++)
                jce_state_set_entity_enabled(sel_ids[i], false);
            jce_state_end_batch_edit();
        }
    } else {
        snprintf(lbl, sizeof(lbl), "%s###multi_enabled", jce_editor_i18n("inspector.enabled"));
        if (ImGui::Checkbox(lbl, &chk)) {
            jce_state_begin_batch_edit();
            for (int i = 0; i < sel_count; i++)
                jce_state_set_entity_enabled(sel_ids[i], chk);
            jce_state_end_batch_edit();
        }
    }

    bool same_color = jce_state_entity_exists(sel_ids[0]);
    JceTagColor first_color = same_color ? jce_state_entity_tag_color(sel_ids[0])
                                         : JCE_TAG_NONE;
    for (int i = 1; same_color && i < sel_count; i++) {
        if (!jce_state_entity_exists(sel_ids[i])) continue;
        if (jce_state_entity_tag_color(sel_ids[i]) != first_color) {
            same_color = false;
            break;
        }
    }

    if (same_color) {
        int tag_color = (int)first_color;
        const char *tag_items[] = {
            jce_editor_i18n("hierarchy.noTag"),
            jce_editor_i18n("hierarchy.tag.red"),
            jce_editor_i18n("hierarchy.tag.orange"),
            jce_editor_i18n("hierarchy.tag.yellow"),
            jce_editor_i18n("hierarchy.tag.green"),
            jce_editor_i18n("hierarchy.tag.blue"),
            jce_editor_i18n("hierarchy.tag.purple"),
            jce_editor_i18n("hierarchy.tag.gray")
        };
        ImGui::PushItemWidth(-1);
        snprintf(lbl, sizeof(lbl), "%s###multi_tag_color", jce_editor_i18n("inspector.tagColor"));
        if (ImGui::Combo(lbl, &tag_color, tag_items, 8)) {
            for (int i = 0; i < sel_count; i++)
                jce_state_set_entity_tag_color(sel_ids[i], (JceTagColor)tag_color);
        }
        ImGui::PopItemWidth();
    } else {
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.mixedTagColors"));
    }

    ImGui::Separator();

    /* ── Bulk Transform editor (multi-select) ──────────────────────── */
    if (scene) {
        if (ImGui::CollapsingHeader(jce_editor_i18n("inspector.bulk.section"), ImGuiTreeNodeFlags_DefaultOpen)) {
            ImGui::PushID("##jce_bulk_xform_section");
            static float pos_delta[3] = {0,0,0};
            static float rot_set[3]   = {0,0,0};
            static float scl_set[3]   = {1,1,1};
            static bool  scl_uniform  = true;

            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bulk.posOffset"));
            ImGui::DragFloat3("##bulk_pos_delta", pos_delta, 0.1f);
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.apply", "bulk_pos"))) {
                /* One undo entry for the whole bulk apply. */
                jce_state_begin_batch_edit();
                for (int i = 0; i < sel_count; i++) {
                    JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                    JceTransform *t = jce_scene_get_transform(scene, e);
                    if (t) {
                        t->position.x += pos_delta[0];
                        t->position.y += pos_delta[1];
                        t->position.z += pos_delta[2];
                    }
                }
                jce_state_end_batch_edit();
                pos_delta[0] = pos_delta[1] = pos_delta[2] = 0.0f;
            }

            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bulk.rotAbsolute"));
            ImGui::DragFloat3("##bulk_rot_set", rot_set, 1.0f);
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.apply", "bulk_rot"))) {
                jce_state_begin_batch_edit();
                for (int i = 0; i < sel_count; i++) {
                    JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                    JceTransform *t = jce_scene_get_transform(scene, e);
                    if (t) {
                        t->rotation = editor_q_from_euler_deg(rot_set);
                        jce_editor_set_cached_euler_deg(sel_ids[i], t->rotation, rot_set);
                    }
                }
                jce_state_end_batch_edit();
            }

            ImGui::TextDisabled("%s", jce_editor_i18n("inspector.bulk.scaleAbsolute"));
            ImGui::Checkbox(jce_editor_i18n_id("inspector.bulk.uniform", "bulk_scl_uni"), &scl_uniform);
            if (scl_uniform) {
                ImGui::DragFloat("##bulk_scl_uniform_v", &scl_set[0], 0.01f, 0.001f, 1000.0f);
                scl_set[1] = scl_set[2] = scl_set[0];
            } else {
                ImGui::DragFloat3("##bulk_scl_set", scl_set, 0.01f, 0.001f, 1000.0f);
            }
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.apply", "bulk_scl"))) {
                jce_state_begin_batch_edit();
                for (int i = 0; i < sel_count; i++) {
                    JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                    JceTransform *t = jce_scene_get_transform(scene, e);
                    if (t) {
                        t->scale.x = scl_set[0];
                        t->scale.y = scl_set[1];
                        t->scale.z = scl_set[2];
                    }
                }
                jce_state_end_batch_edit();
            }

            ImGui::Separator();
            if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.resetPos", "bulk_reset_pos"))) {
                jce_state_begin_batch_edit();
                for (int i = 0; i < sel_count; i++) {
                    JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                    JceTransform *t = jce_scene_get_transform(scene, e);
                    if (t) { t->position.x = t->position.y = t->position.z = 0.0f; }
                }
                jce_state_end_batch_edit();
            }
            ImGui::SameLine();
            if (ImGui::Button(jce_editor_i18n_id("inspector.bulk.resetScale", "bulk_reset_scl"))) {
                jce_state_begin_batch_edit();
                for (int i = 0; i < sel_count; i++) {
                    JceEntity e = jce_state_to_ecs_entity(sel_ids[i]);
                    JceTransform *t = jce_scene_get_transform(scene, e);
                    if (t) { t->scale.x = t->scale.y = t->scale.z = 1.0f; }
                }
                jce_state_end_batch_edit();
            }
            ImGui::PopID();
        }

        ImGui::Separator();
    }

    {
        uint32_t focused_id = jce_state_get_focused();
        uint32_t to_focus = 0;
        uint32_t to_select = 0;
        uint32_t to_remove = 0;
        int max_show = sel_count < 20 ? sel_count : 20;
        for (int i = 0; i < max_show; i++) {
            ImGui::PushID(i);
            uint32_t id = sel_ids[i];
            const char *nm = jce_state_entity_name(id);
            if (!nm) nm = "(unnamed)";
            bool is_focused = (id == focused_id);

            /* Reserve room for the two SmallButtons on the right so
             * the Selectable does not (a) push them past the visible
             * region and (b) intercept their clicks across the row. */
            const char *focus_lbl  = jce_editor_i18n_id("inspector.bulk.entity.focus", "focus");
            const char *remove_lbl = jce_editor_i18n_id("inspector.bulk.entity.remove", "remove");

            /* Single line: full-width name on the left; compact Focus / Remove
             * ICON buttons RIGHT-ALIGNED on the same line. The icons are drawn as
             * primitives (crosshair = focus, X = remove) so they stay tiny on a
             * narrow inspector and never crowd/cover the name — with hover
             * tooltips for clarity (wide text buttons squeezed names to ~2 chars). */
            float ico    = ImGui::GetFrameHeight();
            float btns_w = ico * 2.0f + ImGui::GetStyle().ItemSpacing.x;
            float full_w = ImGui::GetContentRegionAvail().x;
            float name_w = full_w - btns_w - ImGui::GetStyle().ItemSpacing.x;
            if (name_w < 40.0f) name_w = 40.0f;

            /* Name (click = preview this entity within the multi-selection). */
            if (ImGui::Selectable(nm, is_focused, 0, ImVec2(name_w, 0.0f))) {
                to_focus = id;
            }

            /* Focus icon (crosshair) — drill into this entity: make it the SOLE
             * selection so the Inspector enters its full single-entity panel and
             * frame the camera on it. */
            ImGui::SameLine();
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##focus", ImVec2(ico, ico)))
                    to_select = id;
                bool hov = ImGui::IsItemHovered();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImVec2 cc = ImVec2(p.x + ico * 0.5f, p.y + ico * 0.5f);
                ImU32 col = hov ? IM_COL32(255, 220, 120, 255)
                                : IM_COL32(200, 200, 200, 255);
                dl->AddCircle(cc, ico * 0.30f, col, 16, 1.5f);
                dl->AddCircleFilled(cc, 1.5f, col, 8);
                if (hov) ImGui::SetTooltip("%s", focus_lbl);
            }

            /* Remove icon (X) — drop this entity from the selection. */
            ImGui::SameLine();
            {
                ImVec2 p = ImGui::GetCursorScreenPos();
                if (ImGui::InvisibleButton("##remove", ImVec2(ico, ico)))
                    to_remove = id;
                bool hov = ImGui::IsItemHovered();
                ImDrawList *dl = ImGui::GetWindowDrawList();
                ImU32 col = hov ? IM_COL32(255, 120, 120, 255)
                                : IM_COL32(200, 200, 200, 255);
                dl->AddLine(ImVec2(p.x + ico * 0.30f, p.y + ico * 0.30f),
                            ImVec2(p.x + ico * 0.70f, p.y + ico * 0.70f), col, 1.8f);
                dl->AddLine(ImVec2(p.x + ico * 0.70f, p.y + ico * 0.30f),
                            ImVec2(p.x + ico * 0.30f, p.y + ico * 0.70f), col, 1.8f);
                if (hov) ImGui::SetTooltip("%s", remove_lbl);
            }
            ImGui::PopID();
        }
        if (sel_count > 20)
            ImGui::Text("... %d %s", sel_count - 20, jce_editor_i18n("inspector.andMore"));

        if (to_remove) jce_state_deselect_entity(to_remove);
        else if (to_select) {
            /* Drill-in: collapse the multi-selection to this one entity so the
             * Inspector enters its single-entity panel, and frame the camera.
             * (Previously "focus" only moved the camera + retargeted the focused
             * id, which left the bulk view up and read as "just camera
             * tracking" instead of entering the object's inspector.) */
            jce_state_select_entity(to_select, false);
            jce_editor_scene_camera_focus_entity(to_select);
        }
        else if (to_focus) {
            /* Row click = preview within the multi-selection: just retarget the
             * focused id; do NOT remove+re-add, which would shove the clicked
             * entity to the tail of the selection array and make this list
             * visually "swap" the clicked row with the bottom row each click. */
            jce_state_set_focused(to_focus);
        }
    }

    ImGui::Separator();

    ImGui::PushStyleColor(ImGuiCol_Button, ImVec4(0.6f, 0.1f, 0.1f, 1.0f));
    ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(0.8f, 0.2f, 0.2f, 1.0f));
    float btn_w = ImGui::GetContentRegionAvail().x;
    if (ImGui::Button(jce_editor_i18n("inspector.deleteSelected"), ImVec2(btn_w, 0)))
        jce_editor_inspector_request_delete_confirm_many(sel_ids, sel_count);
    ImGui::PopStyleColor(2);

    return true;
}
