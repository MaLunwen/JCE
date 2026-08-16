/*
 * jce_panel_hierarchy.cpp  Hierarchy panel (entity tree).
 *
 * Filter bar, tree frame, shortcuts.
 * Node drawing is in jce_panel_hierarchy_node.cpp.
 * Context menus are in jce_panel_hierarchy_menu.cpp.
 */

#include "jce_panel_hierarchy_internal.h"
#include "jce_panel_common.h"
#include "jce_panel_hierarchy_input.h"
#include "ui/jce_editor_dnd.h"
#include "core/jce_editor_project_state.h"
#include "core/jce_hotkeys.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>
#include <cctype>

/* Tag-filter combo entries; the i18n ids double as the stable tokens the
 * per-project store persists the filter under (indices could drift if the
 * palette ever changes). */
static const char *kTagFilter[] = {
    "tagColor.all", "tagColor.red", "tagColor.orange", "tagColor.yellow",
    "tagColor.green", "tagColor.blue", "tagColor.purple", "tagColor.gray"
};

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_hierarchy_content(void)
{
    ensure_hier_init();

    /* Restore per-project view state once the project store is live — it is
     * inert until a project root is known, so the first draw can be too
     * early to read from it. */
    static bool s_pstate_restored = false;
    if (!s_pstate_restored && jce_editor_pstate_active()) {
        s_pstate_restored = true;
        int sm = jce_editor_pstate_get_int("hierarchy.sort", s_hier.sort_mode);
        if (sm >= 0 && sm < 3) s_hier.sort_mode = sm;
        char tag[32];
        if (jce_editor_pstate_get_str("hierarchy.tag_filter", tag, sizeof(tag))) {
            for (int i = 0; i < 8; i++) {
                if (strcmp(tag, kTagFilter[i]) == 0) { s_hier.tag_filter = i; break; }
            }
        }
    }

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
    if (ImGui::Combo("##tag_filter", &s_hier.tag_filter,
                     jce_editor_i18n_combo(kTagFilter, 8)))
        jce_editor_pstate_set_str("hierarchy.tag_filter",
                                  kTagFilter[s_hier.tag_filter]);
    ImGui::SameLine();
    {
        static const char *kSort[] = {
            "hierarchy.sort.default", "hierarchy.sort.byName", "hierarchy.sort.byTag"
        };
        if (ImGui::Combo("##sort", &s_hier.sort_mode,
                         jce_editor_i18n_combo(kSort, 3)))
            jce_editor_pstate_set_int("hierarchy.sort", s_hier.sort_mode);
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
                /* One undo entry for the whole drop: jce_state_reparent_entity
                 * opens its own edit scope, so the previous per-entity loop
                 * left Ctrl+Z able to unparent only the last entity. */
                JcePanelSelection sel;
                jce_panel_selection_snapshot(sel);
                jce_panel_selection_apply(sel, [](uint32_t id) {
                    jce_state_reparent_entity(id, 0);
                });
            } else {
                jce_state_reparent_entity(dragged_id, 0);
            }
        }
        ImGui::EndDragDropTarget();
    }

    {
        s_hier.ctx_clicked_entity = false;

        /* Gather + sort roots (display_order/flat are filled by the flatten).
         * O(n) single pass — the old get_root_count + per-index get_root_id loop
         * was O(n^2) (each get_root_id rescans g_entity_order), which became
         * catastrophic once the clipper let the display cap grow to 32768. */
        static uint32_t root_ids[HIERARCHY_MAX_DISPLAY];
        uint64_t _t0_hr = jce_time_perf_counter();
        int root_count = jce_state_get_roots(root_ids, HIERARCHY_MAX_DISPLAY);
        jce_perf_phase_add("ed_hier_roots",
            jce_time_perf_to_ms(_t0_hr, jce_time_perf_counter()));

        uint64_t _t0_hs = jce_time_perf_counter();
        sort_entity_ids(root_ids, root_count);
        jce_perf_phase_add("ed_hier_sort",
            jce_time_perf_to_ms(_t0_hs, jce_time_perf_counter()));

        /* Flatten the visible tree (full list → display_order + flat) then
         * render only the clipper-visible rows.  This is the perf fix: a
         * full-loaded scene flattens thousands of rows but submits ~30. */
        uint64_t _t0_hf = jce_time_perf_counter();
        jce_hierarchy_flatten(root_ids, root_count);
        jce_perf_phase_add("ed_hier_flat",
            jce_time_perf_to_ms(_t0_hf, jce_time_perf_counter()));

        ImGuiListClipper clip;
        clip.Begin(s_hier.flat_count);

        /* Reveal: the target row may be clipped out, so force it (and the
         * single visible row immediately after, to leave headroom for
         * SetScrollHereY centering) to be submitted this frame.  Its body's
         * SetScrollHereY then fires and clears reveal_pending.  The flatten
         * already forced the reveal-path ancestors open via
         * node_in_reveal_path(), so the target is present in flat[]. */
        if (s_hier.reveal_pending && s_hier.reveal_target != 0) {
            int reveal_idx = -1;
            for (int i = 0; i < s_hier.flat_count; i++) {
                if (!s_hier.flat[i].chunk_header &&
                    s_hier.flat[i].id == s_hier.reveal_target) {
                    reveal_idx = i;
                    break;
                }
            }
            if (reveal_idx >= 0)
                clip.IncludeItemByIndex(reveal_idx);
            else
                s_hier.reveal_pending = false; /* not visible; nothing to scroll to */
        }

        while (clip.Step()) {
            for (int i = clip.DisplayStart; i < clip.DisplayEnd; i++) {
                if (s_hier.flat[i].chunk_header)
                    draw_chunk_group_row(s_hier.flat[i].id, s_hier.flat[i].depth);
                else
                    draw_entity_row(s_hier.flat[i].id, s_hier.flat[i].depth);
            }
        }
        clip.End();

        /* Left-click on empty space: clear selection. */
        if (ImGui::IsWindowHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && !ImGui::IsAnyItemHovered())
        {
            jce_state_clear_selection();
            s_hier.shift_anchor = 0;
            jce_editor_inspector_request_sync();
        }

        /* Right-click on empty space: lean context menu (Create at root /
           Paste / Select All) — industry-standard (Unity/Unreal/Godot all show
           a menu here). The richer entity menu appears when an entity is
           right-clicked; shared Create/Paste are intentional, not redundant
           (they target root vs child). */
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
            /* Never treat Delete/Backspace as "delete entity" while a rename
             * (or any text field) is active — Backspace must edit the name, not
             * destroy the entity.  jce_hotkey_pressed() consumes the chord, so
             * short-circuit BEFORE calling it so the InputText still gets the key. */
            if (s_hier.renaming_id == 0 && !ImGui::GetIO().WantTextInput &&
                (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)
                 || jce_hotkey_pressed(JCE_HK_EDIT_DELETE_ALT))) {
                jce_panel_delete_selection();
            }
            if (jce_hotkey_pressed(JCE_HK_EDIT_DUPLICATE)) {
                jce_panel_duplicate_selection();
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
                char typed = jce_panel_typeahead_key();

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
                    int found = jce_panel_typeahead_scan(
                        s_hier.display_count, start, [lc](int k) {
                            const char *nm =
                                jce_state_entity_name(s_hier.display_order[k]);
                            return nm && nm[0]
                                && tolower((unsigned char)nm[0]) == (unsigned char)lc;
                        });

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

    /* ed_hier phase: at 150k entities the per-frame rebuild (get_roots full
     * scan + flatten) is the prime suspect for the editor's non-render
     * app_update remainder — this number decides the change-gated rebuild. */
    uint64_t _t0_hier = jce_time_perf_counter();
    char title[256];
    snprintf(title, sizeof(title), "%s###hierarchy", jce_editor_i18n("Hierarchy"));
    if (ImGui::Begin(title, vis, ImGuiWindowFlags_NoFocusOnAppearing))
        jce_editor_panel_hierarchy_content();
    ImGui::End();
    jce_perf_phase_add("ed_hier", jce_time_perf_to_ms(_t0_hier,
                                                      jce_time_perf_counter()));
}
