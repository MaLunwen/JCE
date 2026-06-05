/*
 * jce_panel_hierarchy.cpp  Hierarchy panel (entity tree).
 *
 * Filter bar, tree frame, shortcuts.
 * Node drawing is in jce_panel_hierarchy_node.cpp.
 * Context menus are in jce_panel_hierarchy_menu.cpp.
 */

#include "jce_panel_hierarchy_internal.h"
#include "jce_panel_hierarchy_input.h"
#include "ui/jce_editor_dnd.h"
#include "core/jce_hotkeys.h"
#include <cctype>

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
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(JCE_DND_ENTITY);
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
        JceHotkeyChord consumed_chords[8];
        int consumed_chord_count = 0;

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
            if (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)
                || jce_hotkey_pressed(JCE_HK_EDIT_DELETE_ALT)) {
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
            if (jce_hotkey_pressed(JCE_HK_VIEW_FRAME_SELECTED)) {
                if (consumed_chord_count < (int)(sizeof(consumed_chords) /
                                                 sizeof(consumed_chords[0])))
                    consumed_chords[consumed_chord_count++] =
                        jce_hotkey_get(JCE_HK_VIEW_FRAME_SELECTED);
                focus_entity_in_scene(focused);
            }
        }

        /* Alpha-jump: press a letter/digit/symbol (no modifier) to select
         * and reveal the next entity whose name starts with that char.
         * Same key within 1.5 s cycles through subsequent matches. */
        if (!ImGui::GetIO().WantTextInput && s_hier.renaming_id == 0) {
            static char   s_jump_char   = '\0';
            static int    s_jump_start  = 0;
            static double s_jump_reset  = 0.0;
            const double  kCycleWindow  = 1.5;

            ImGuiIO &io = ImGui::GetIO();
            if (!io.KeyCtrl && !io.KeyAlt && !io.KeySuper) {
                char typed = '\0';
                for (ImGuiKey key = ImGuiKey_A; key <= ImGuiKey_Z && !typed;
                     key = (ImGuiKey)(key + 1))
                    if (ImGui::IsKeyPressed(key, false))
                        typed = (char)('a' + (key - ImGuiKey_A));
                for (ImGuiKey key = ImGuiKey_0; key <= ImGuiKey_9 && !typed;
                     key = (ImGuiKey)(key + 1))
                    if (ImGui::IsKeyPressed(key, false))
                        typed = (char)('0' + (key - ImGuiKey_0));
                for (ImGuiKey key = ImGuiKey_Keypad0; key <= ImGuiKey_Keypad9 && !typed;
                     key = (ImGuiKey)(key + 1))
                    if (ImGui::IsKeyPressed(key, false))
                        typed = (char)('0' + (key - ImGuiKey_Keypad0));
                if (!typed && ImGui::IsKeyPressed(ImGuiKey_Space, false))          typed = ' ';
                if (!typed && ImGui::IsKeyPressed(ImGuiKey_Minus, false))          typed = '-';
                if (!typed && ImGui::IsKeyPressed(ImGuiKey_Period, false))         typed = '.';
                if (!typed && ImGui::IsKeyPressed(ImGuiKey_KeypadDecimal, false))  typed = '.';
                if (!typed && ImGui::IsKeyPressed(ImGuiKey_KeypadSubtract, false)) typed = '-';

                if (jce_hierarchy_alpha_jump_key_consumed(
                        typed,
                        consumed_chords,
                        consumed_chord_count)) {
                    typed = '\0';
                }

                if (typed) {
                    char lc = (char)tolower((unsigned char)typed);
                    double now = ImGui::GetTime();
                    bool cycling = (lc == s_jump_char
                                    && (now - s_jump_reset) < kCycleWindow);
                    int start = cycling ? s_jump_start : 0;
                    int n = s_hier.display_count;
                    int found = -1;

                    for (int pass = 0; pass < 2 && found < 0; ++pass) {
                        int from = (pass == 0) ? start : 0;
                        int to   = (pass == 0) ? n     : start;
                        for (int k = from; k < to; ++k) {
                            uint32_t eid = s_hier.display_order[k];
                            const char *nm = jce_state_entity_name(eid);
                            if (nm && nm[0]
                                && tolower((unsigned char)nm[0]) == (unsigned char)lc)
                            {
                                found = k;
                                break;
                            }
                        }
                    }

                    if (found >= 0) {
                        uint32_t target = s_hier.display_order[found];
                        jce_state_select_entity(target, false);
                        s_hier.reveal_target  = target;
                        s_hier.reveal_pending = true;
                        s_jump_char  = lc;
                        s_jump_start = found + 1;
                        s_jump_reset = ImGui::GetTime();
                    }
                }
            }
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
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_hierarchy_content();
    ImGui::End();
}
