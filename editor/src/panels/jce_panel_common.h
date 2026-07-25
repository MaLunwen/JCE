/*
 * jce_panel_common.h
 *
 * Boilerplate shared by the dockable editor panels:
 *   - jce_panel_redirect_to_workbench() — the "this panel is now a tab of
 *     <workbench>" shim that every merged panel's entry point runs.
 *   - JcePanelTabState + helpers        — the per-user persisted tab
 *     selection every workbench panel keeps for its outer TabBar.
 *   - jce_panel_contains_ci() / _filter_match_ci() — the ASCII-folding
 *     substring test behind every panel filter box.
 *   - jce_panel_fmt_size()              — "%.2f KB" byte formatting.
 *   - jce_panel_typeahead_key() / _scan() — the Explorer-style
 *     press-a-letter-to-jump primitives.
 *   - jce_panel_selection_*() / _duplicate_selection() / _delete_selection()
 *     — the "apply this to the whole selection as one undo entry"
 *     orchestration every multi-select entry point runs.
 *
 * Header-only on purpose: each body is a handful of statements and the
 * panels (plus the menu bar, which drives the very same commands) are the
 * only translation units that need them.  Do not include engine private
 * headers or SDL/bgfx here.
 */

#pragma once

#include "core/jce_editor_i18n.h"
#include "core/jce_editor_state.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_editor_ui_state.h"

#include <jce/tools/jce_imgui.hpp>

#include <cstddef>
#include <cstdint>
#include <cstdio>

/* ── Merged-panel redirect shim ───────────────────────────────────── */

/*
 * Several panels were folded into a workbench panel as tabs, but their
 * JCE_PANEL_* ids stay registered so the menu / hotkey entries bound to
 * them keep working.  Activating such a panel clears its own visibility
 * flag, shows the workbench and focuses it.
 *
 * `workbench_title_key` is the i18n key of the workbench window title and
 * `workbench_window_id` its stable "###" suffix; together they rebuild the
 * exact window name the workbench passes to ImGui::Begin().
 *
 * Returns true when the redirect happened — i.e. when the caller should
 * follow up with the workbench's *_request_tab() call.
 */
inline bool jce_panel_redirect_to_workbench(JceEditorPanel panel,
                                            JceEditorPanel workbench,
                                            const char *workbench_title_key,
                                            const char *workbench_window_id)
{
    bool *vis = jce_editor_panel_visible_ptr(panel);
    if (!vis || !*vis)
        return false;
    *vis = false;

    bool *wb_vis = jce_editor_panel_visible_ptr(workbench);
    if (wb_vis) *wb_vis = true;

    char title[128];
    std::snprintf(title, sizeof(title), "%s###%s",
                  jce_editor_i18n(workbench_title_key), workbench_window_id);
    ImGui::SetWindowFocus(title);
    return true;
}

/* ── Workbench tab selection (persisted per user) ─────────────────── */

/*
 * One instance per workbench translation unit (file-static): this is the
 * panel's own state moved into a named struct, not state shared between
 * panels.
 *   key      — jce_editor_ui_state_* storage key, unique per panel
 *   max_tab  — inclusive upper bound of the valid tab indices
 *   request  — tab to force-select on the next draw (-1 = none)
 *   current  — mirror of the active TabItem, for menu markers
 *   loaded   — `current` has been read back from disk at least once
 */
struct JcePanelTabState {
    const char *key     = nullptr;
    int         max_tab = 0;
    int         request = -1;
    int         current = 0;
    bool        loaded  = false;
};

inline bool jce_panel_tab_valid(const JcePanelTabState &st, int idx)
{
    return idx >= 0 && idx <= st.max_tab;
}

inline void jce_panel_tab_ensure_loaded(JcePanelTabState &st)
{
    if (st.loaded)
        return;
    st.current = jce_editor_ui_state_load_int(st.key, 0, 0, st.max_tab);
    st.request = st.current;
    st.loaded  = true;
}

inline ImGuiTabItemFlags jce_panel_tab_flags(const JcePanelTabState &st, int idx)
{
    return (st.request == idx) ? ImGuiTabItemFlags_SetSelected : 0;
}

/* Record the tab the user just clicked.  Persists only once the stored
 * value has been read back, so a first-frame default never overwrites it. */
inline void jce_panel_tab_set_current(JcePanelTabState &st, int idx)
{
    if (!jce_panel_tab_valid(st, idx) || st.current == idx)
        return;
    st.current = idx;
    if (st.loaded)
        jce_editor_ui_state_save_int(st.key, idx);
}

/* Force-select `idx` on the next draw — backs the *_request_tab() entries. */
inline void jce_panel_tab_request(JcePanelTabState &st, int idx)
{
    if (!jce_panel_tab_valid(st, idx))
        return;
    st.request = idx;
    st.current = idx;
    jce_editor_ui_state_save_int(st.key, idx);
}

inline int jce_panel_tab_current(JcePanelTabState &st)
{
    jce_panel_tab_ensure_loaded(st);
    return st.current;
}

/* ── Case-insensitive substring match ─────────────────────────────── */

/*
 * Folding is deliberately ASCII-only.  The editor never calls
 * setlocale(), so tolower()/_strnicmp() fold exactly 'A'..'Z' too — and
 * touching nothing above 0x7F keeps UTF-8 sequences byte-exact, so a CJK
 * query still matches a CJK label.
 */
inline char jce_panel_ascii_lower(char c)
{
    return (c >= 'A' && c <= 'Z') ? (char)(c - 'A' + 'a') : c;
}

/* True when `needle` occurs anywhere in `hay`, ignoring ASCII case.  An
 * empty or NULL needle never matches — filter boxes, where an empty box
 * means "show everything", want jce_panel_filter_match_ci() instead. */
inline bool jce_panel_contains_ci(const char *hay, const char *needle)
{
    if (!hay || !needle || !*needle)
        return false;
    for (const char *h = hay; *h; ++h) {
        const char *a = h;
        const char *b = needle;
        while (*a && *b &&
               jce_panel_ascii_lower(*a) == jce_panel_ascii_lower(*b)) {
            ++a;
            ++b;
        }
        if (!*b)
            return true;
    }
    return false;
}

/* Filter-box flavour: an empty / NULL filter accepts every row. */
inline bool jce_panel_filter_match_ci(const char *text, const char *filter)
{
    if (!filter || !*filter)
        return true;
    return jce_panel_contains_ci(text, filter);
}

/* ── Byte-size formatting ─────────────────────────────────────────── */

/* Writes "<value> <unit>" with two decimals and 1024-steps into `out`,
 * capped at TB, and returns `out` so it can be passed straight to an
 * ImGui::Text* call. */
inline const char *jce_panel_fmt_size(uint64_t b, char *out, size_t n)
{
    const char *u[] = { "B", "KB", "MB", "GB", "TB" };
    double v = (double)b;
    int k = 0;
    while (v >= 1024.0 && k < 4) { v /= 1024.0; ++k; }
    std::snprintf(out, n, "%.2f %s", v, u[k]);
    return out;
}

/* ── Explorer-style type-ahead ────────────────────────────────────── */

/*
 * Returns the character typed this frame for a type-ahead jump, or '\0'
 * when none was.  Covers letters (reported lowercase), digits, numpad
 * digits, space, minus and period — the set Windows Explorer reacts to.
 * Modifier and focus guards stay with the caller: this only reads keys.
 */
inline char jce_panel_typeahead_key(void)
{
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
    return typed;
}

/* Wrap-around scan over [0,count): tries [start,count) first, then
 * [0,start), and returns the first index `match` accepts, or -1.  Only
 * `match` differs per panel — the hierarchy tests an entity name, the
 * asset browser a grid row name. */
template <typename MatchFn>
inline int jce_panel_typeahead_scan(int count, int start, MatchFn match)
{
    for (int pass = 0; pass < 2; ++pass) {
        int from = (pass == 0) ? start : 0;
        int to   = (pass == 0) ? count : start;
        for (int k = from; k < to; ++k)
            if (match(k))
                return k;
    }
    return -1;
}

/* ── Multi-select operation orchestration ─────────────────────────── */

/*
 * Every "do this to the whole selection" entry point — Ctrl+D / Del, the
 * Edit menu, the Scene View context menu, the Hierarchy — needs the same
 * three things around its actual operation, and the hand-written copies
 * had drifted on all three:
 *
 *   1. a SNAPSHOT of the selection.  jce_state_get_selection() hands back
 *      the live array, so an operation that selects (or deselects) as it
 *      goes would be walking a buffer that moves under it.
 *   2. ONE undo entry for the whole run.  With one entry per entity a
 *      single Ctrl+Z undoes only part of what the user saw happen.
 *   3. a liveness re-check per entity, because an operation may destroy
 *      entities other than the one it was handed (a parent takes its
 *      children with it).
 *
 * jce_scene_view_snap_selection_to_ground() is the reference shape; this
 * is that shape made reusable over the same depth-counted
 * jce_state_*_batch_edit() scope, not a second transaction mechanism.
 */
struct JcePanelSelection {
    uint32_t ids[JCE_MAX_SELECTED];
    int      count = 0;
};

/*
 * Copy the current selection out of editor state, dropping empty slots.
 * When nothing is selected the focused entity is used instead — the state
 * layer keeps `focused` inside the selection, so that fallback only ever
 * fires for a hypothetically desynced state, but it costs two lines and
 * matches what every hand-written copy did.  Returns out.count.
 */
inline int jce_panel_selection_snapshot(JcePanelSelection &out)
{
    out.count = 0;

    int             sel_count = 0;
    const uint32_t *sel       = jce_state_get_selection(&sel_count);
    for (int i = 0; i < sel_count && out.count < JCE_MAX_SELECTED; i++)
        if (sel[i] != 0)
            out.ids[out.count++] = sel[i];

    if (out.count == 0) {
        uint32_t focused = jce_state_get_focused();
        if (focused != 0)
            out.ids[out.count++] = focused;
    }
    return out.count;
}

/*
 * Run `op(id)` over a snapshot inside ONE undo entry, skipping ids that no
 * longer exist by the time their turn comes.  The batch scope nests and
 * discards itself again when nothing actually changed, so wrapping a
 * single-entity run adds no undo entry of its own.
 */
template <typename OpFn>
inline void jce_panel_selection_apply(const JcePanelSelection &sel, OpFn op)
{
    if (sel.count <= 0)
        return;

    jce_state_begin_batch_edit();
    for (int i = 0; i < sel.count; i++)
        if (jce_state_entity_exists(sel.ids[i]))
            op(sel.ids[i]);
    jce_state_end_batch_edit();
}

/* Make `ids` the selection, first entry focused, and resync the Inspector.
 * Zero ids are skipped: handing one to jce_state_select_entity() would
 * seat a bogus 0 as the entire selection. */
inline void jce_panel_select_ids(const uint32_t *ids, int count)
{
    bool first = true;
    for (int i = 0; i < count; i++) {
        if (ids[i] == 0)
            continue;
        jce_state_select_entity(ids[i], !first);
        first = false;
    }
    if (!first)
        jce_editor_inspector_request_sync();
}

/* Ctrl+D: duplicate every selected entity as one undo entry, then select
 * the copies. */
inline void jce_panel_duplicate_selection(void)
{
    JcePanelSelection sel;
    if (jce_panel_selection_snapshot(sel) <= 0)
        return;

    uint32_t dup_ids[JCE_MAX_SELECTED];
    int      dup_count = 0;
    jce_panel_selection_apply(sel, [&](uint32_t id) {
        uint32_t dup = jce_state_duplicate_entity(id);
        if (dup != 0 && dup_count < JCE_MAX_SELECTED)
            dup_ids[dup_count++] = dup;
    });

    jce_panel_select_ids(dup_ids, dup_count);
}

/* Del / Backspace: hand the whole selection to the Inspector's confirm
 * dialog, which owns the deletion and its undo entry. */
inline void jce_panel_delete_selection(void)
{
    JcePanelSelection sel;
    if (jce_panel_selection_snapshot(sel) > 0)
        jce_editor_inspector_request_delete_confirm_many(sel.ids, sel.count);
}
