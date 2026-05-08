/*
 * jce_panel_hierarchy.cpp  Hierarchy panel (entity tree).
 *
 * Filter bar, tree frame, shortcuts.
 * Node drawing is in jce_panel_hierarchy_node.cpp.
 * Context menus are in jce_panel_hierarchy_menu.cpp.
 */

#include "jce_panel_hierarchy_internal.h"
#include "core/jce_hotkeys.h"

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
    {
        static const char *kTagFilter[] = {
            "tagColor.all", "tagColor.red", "tagColor.orange", "tagColor.yellow",
            "tagColor.green", "tagColor.blue", "tagColor.purple", "tagColor.gray"
        };
        ImGui::Combo("##tag_filter", &s_hier.tag_filter,
                     jce_editor_i18n_combo(kTagFilter, 8));
    }
    ImGui::SameLine();
    {
        static const char *kSort[] = {
            "hierarchy.sort.default", "hierarchy.sort.byName", "hierarchy.sort.byTag"
        };
        ImGui::Combo("##sort", &s_hier.sort_mode,
                     jce_editor_i18n_combo(kSort, 3));
    }
    ImGui::PopItemWidth();

    ImGui::Separator();

    /* Entity tree */
    ImGui::BeginChild("EntityTree", ImVec2(0, 0), ImGuiChildFlags_None);

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            if (jce_state_is_selected(dragged_id)) {
                int sel_n = 0;
                const uint32_t *sel = jce_state_get_selection(&sel_n);
                uint32_t ids[256];
                int n = sel_n < 256 ? sel_n : 256;
                for (int i = 0; i < n; i++) ids[i] = sel[i];
                for (int i = 0; i < n; i++)
                    jce_state_reparent_entity(ids[i], 0);
            } else {
                jce_state_reparent_entity(dragged_id, 0);
            }
        }
        ImGui::EndDragDropTarget();
    }

    {
        s_hier.display_count = 0;
        s_hier.ctx_clicked_entity = false;

        uint32_t root_ids[HIERARCHY_MAX_DISPLAY];
        int root_total = jce_state_get_root_count();
        int root_count = 0;

        for (int i = 0; i < root_total && root_count < HIERARCHY_MAX_DISPLAY; i++) {
            uint32_t rid = jce_state_get_root_id(i);
            if (rid != 0 && jce_state_entity_exists(rid))
                root_ids[root_count++] = rid;
        }

        sort_entity_ids(root_ids, root_count);

        for (int i = 0; i < root_count; i++)
            draw_entity_node(root_ids[i]);

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

        /* Paste is allowed regardless of focus (pastes under focused if
         * any, else at scene root). */
        if (jce_hotkey_pressed(JCE_HK_EDIT_PASTE) && jce_state_has_copied()) {
            uint32_t pasted_ids[JCE_MAX_SELECTED];
            uint32_t parent = jce_state_entity_exists(focused) ? focused : 0;
            int pasted_n = jce_state_paste_entities(parent, pasted_ids,
                                                    JCE_MAX_SELECTED);
            if (pasted_n > 0) {
                jce_state_select_entity(pasted_ids[0], false);
                for (int i = 1; i < pasted_n; i++)
                    jce_state_select_entity(pasted_ids[i], true);
            }
        }

        if (focused) {
            if (jce_hotkey_pressed(JCE_HK_EDIT_RENAME)) {
                if (jce_state_entity_exists(focused))
                    begin_rename_entity(focused, jce_state_entity_name(focused));
            }
            if (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)) {
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
            if (jce_hotkey_pressed(JCE_HK_EDIT_DUPLICATE)) {
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
            if (jce_hotkey_pressed(JCE_HK_EDIT_COPY)) {
                int sel_count = 0;
                const uint32_t *sel = jce_state_get_selection(&sel_count);
                if (sel_count > 0)
                    jce_state_copy_entities(sel, sel_count, false);
                else
                    jce_state_copy_entity(focused);
            }
            if (jce_hotkey_pressed(JCE_HK_EDIT_CUT)) {
                int sel_count = 0;
                const uint32_t *sel = jce_state_get_selection(&sel_count);
                if (sel_count > 0)
                    jce_state_copy_entities(sel, sel_count, true);
                else
                    jce_state_copy_entities(&focused, 1, true);
            }
            if (jce_hotkey_pressed(JCE_HK_VIEW_FRAME_SELECTED))
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
    snprintf(title, sizeof(title), "%s###hierarchy", jce_editor_i18n("Hierarchy"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_hierarchy_content();
    ImGui::End();
}
