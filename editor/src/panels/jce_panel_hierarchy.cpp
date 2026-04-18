/*
 * jce_panel_hierarchy.cpp  Hierarchy panel (entity tree).
 *
 * Filter bar, tree frame, shortcuts.
 * Node drawing is in jce_panel_hierarchy_node.cpp.
 * Context menus are in jce_panel_hierarchy_menu.cpp.
 */

#include "jce_panel_hierarchy_internal.h"

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_hierarchy_content(void)
{
    ensure_hier_init();

    uint32_t focused_now = jce_state_get_focused();
    if (focused_now != s_hier.last_focus_seen) {
        s_hier.last_focus_seen = focused_now;
        s_hier.reveal_target = focused_now;
        s_hier.reveal_pending = (focused_now != 0);
    }

    /* Filter Bar */
    ImGui::PushItemWidth(-1);
    ImGui::InputTextWithHint("##search", jce_editor_i18n("hierarchy.search"), s_hier.search_buf,
                              sizeof(s_hier.search_buf));
    ImGui::PopItemWidth();

    ImGui::PushItemWidth(100);
    ImGui::Combo("##tag_filter", &s_hier.tag_filter,
                  "All\0Red\0Orange\0Yellow\0Green\0Blue\0Purple\0Gray\0");
    ImGui::SameLine();
    ImGui::Combo("##sort", &s_hier.sort_mode,
                  "Default\0By Name\0By Tag\0");
    ImGui::PopItemWidth();

    ImGui::Separator();

    /* Entity tree */
    ImGui::BeginChild("EntityTree", ImVec2(0, 0), ImGuiChildFlags_None);

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            jce_state_reparent_entity(dragged_id, 0);
        }
        ImGui::EndDragDropTarget();
    }

    {
        s_hier.display_count = 0;
        s_hier.ctx_clicked_entity = false;

        int total = jce_state_get_entity_count();
        uint32_t root_ids[JCE_MAX_ENTITIES];
        int root_count = 0;

        for (int i = 0; i < total; i++) {
            JceEntityInfo *e = jce_state_get_entity_by_index(i);
            if (e && e->parent_id == 0 && root_count < JCE_MAX_ENTITIES)
                root_ids[root_count++] = e->id;
        }

        sort_entity_ids(root_ids, root_count);

        for (int i = 0; i < root_count; i++) {
            JceEntityInfo *e = jce_state_get_entity(root_ids[i]);
            draw_entity_node(e);
        }

        /* Left-click on empty space: clear selection. */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered())
        {
            jce_state_clear_selection();
            s_hier.shift_anchor = 0;
            jce_editor_inspector_request_sync();
        }

        /* Right-click on empty space: context menu. */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
            && !s_hier.ctx_clicked_entity
            && !ImGui::IsAnyItemHovered())
        {
            s_hier.context_on_empty = true;
            s_hier.context_menu_id = 0;
            s_hier.want_ctx_popup = true;
        }
    }

    /* Deferred OpenPopup at consistent ID stack level. */
    if (s_hier.want_ctx_popup) {
        ImGui::OpenPopup("HierarchyContextMenu");
        s_hier.want_ctx_popup = false;
    }

    draw_hierarchy_context_menu();

    ImGui::EndChild();

    /* Keyboard shortcuts */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        uint32_t focused = jce_state_get_focused();
        if (focused) {
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                JceEntityInfo *e = jce_state_get_entity(focused);
                if (e) begin_rename_entity(focused, e->name);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
                int sel_count = 0;
                const uint32_t *sel = jce_state_get_selection(&sel_count);
                if (sel_count > 1) {
                    uint32_t ids[JCE_MAX_SELECTED];
                    int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                    for (int si = 0; si < n; si++) ids[si] = sel[si];
                    jce_editor_inspector_request_delete_confirm_many(ids, n);
                } else {
                    jce_editor_inspector_request_delete_confirm(focused);
                }
            }
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
                int sel_count = 0;
                const uint32_t *sel = jce_state_get_selection(&sel_count);
                if (sel_count > 1) {
                    uint32_t src_ids[JCE_MAX_SELECTED];
                    uint32_t dup_ids[JCE_MAX_SELECTED];
                    int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                    int dup_count = 0;

                    for (int i = 0; i < n; i++)
                        src_ids[i] = sel[i];

                    jce_state_begin_batch_edit();
                    for (int i = 0; i < n; i++) {
                        uint32_t dup = jce_state_duplicate_entity(src_ids[i]);
                        if (dup != 0)
                            dup_ids[dup_count++] = dup;
                    }
                    jce_state_end_batch_edit();

                    if (dup_count > 0) {
                        jce_state_select_entity(dup_ids[0], false);
                        for (int i = 1; i < dup_count; i++)
                            jce_state_select_entity(dup_ids[i], true);
                    }
                } else {
                    uint32_t dup = jce_state_duplicate_entity(focused);
                    jce_state_select_entity(dup, false);
                }
            }
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
                jce_state_copy_entity(focused);
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && jce_state_has_copied()) {
                uint32_t pasted = jce_state_paste_entity(0);
                jce_state_select_entity(pasted, false);
            }
            if (ImGui::IsKeyPressed(ImGuiKey_F) && !ImGui::GetIO().KeyCtrl
                && !ImGui::GetIO().KeyAlt && !ImGui::GetIO().KeyShift)
                focus_entity_in_scene(focused);
        }
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_hierarchy(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY);
    if (!*vis) return;

    char title[256];
    snprintf(title, sizeof(title), "%s###Hierarchy", jce_editor_i18n("Hierarchy"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_hierarchy_content();
    ImGui::End();
}
