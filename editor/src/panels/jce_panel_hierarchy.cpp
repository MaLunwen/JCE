/*
 * jce_panel_hierarchy.cpp  Hierarchy panel (entity tree).
 * Extracted from jce_editor_panels.cpp.
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_i18n.h"
#include "jce_editor_layout.h"
#include "jce_editor_scene_render.h"

#include <imgui.h>
#include <string.h>
#include <stdio.h>
#include <stdlib.h>

/* ── Tag colors (display data) ────────────────────────────────────── */

static const ImVec4 s_tag_colors[JCE_TAG_COLOR_COUNT] = {
    ImVec4(0, 0, 0, 0),
    JCE_COLOR_TAG_RED,
    JCE_COLOR_TAG_ORANGE,
    JCE_COLOR_TAG_YELLOW,
    JCE_COLOR_TAG_GREEN,
    JCE_COLOR_TAG_BLUE,
    JCE_COLOR_TAG_PURPLE,
    JCE_COLOR_TAG_GRAY,
};

static const char *s_tag_names[] = {
    "None", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Gray"
};

/* ── Hierarchy state ──────────────────────────────────────────────── */

static struct {
    char     search_buf[128];
    int      sort_mode;
    int      tag_filter;
    uint32_t renaming_id;
    char     rename_buf[JCE_MAX_ENTITY_NAME];
    uint32_t context_menu_id;
    bool     context_on_empty;
    bool     want_ctx_popup;     /* deferred OpenPopup flag */
    bool     ctx_clicked_entity; /* right-click captured by an entity this frame */
    bool     initialized;

    /* Flat display-order list rebuilt each frame for Shift-range selection. */
    uint32_t display_order[JCE_MAX_ENTITIES];
    int      display_count;
    uint32_t shift_anchor;   /* last click without Shift (range start) */
    uint32_t last_focus_seen;
    uint32_t reveal_target;
    bool     reveal_pending;
} s_hier;

static void ensure_init(void)
{
    if (s_hier.initialized) return;
    memset(&s_hier, 0, sizeof(s_hier));
    s_hier.initialized = true;
}

/* ── Helpers ──────────────────────────────────────────────────────── */

/* Record entity id into display-order list (called as nodes are drawn). */
static void record_display_order(uint32_t id)
{
    if (s_hier.display_count < JCE_MAX_ENTITIES)
        s_hier.display_order[s_hier.display_count++] = id;
}

/* Select a contiguous range in display order between anchor and target. */
static void select_range(uint32_t anchor, uint32_t target)
{
    int a = -1, b = -1;
    for (int i = 0; i < s_hier.display_count; i++) {
        if (s_hier.display_order[i] == anchor) a = i;
        if (s_hier.display_order[i] == target) b = i;
    }
    if (a < 0) a = 0;
    if (b < 0) return;
    if (a > b) { int tmp = a; a = b; b = tmp; }

    jce_state_clear_selection();
    for (int i = a; i <= b; i++)
        jce_state_select_entity(s_hier.display_order[i], true);
}

/* True if node_id is on the parent chain of reveal_target (including itself). */
static bool node_in_reveal_path(uint32_t node_id)
{
    if (!s_hier.reveal_pending || s_hier.reveal_target == 0)
        return false;

    uint32_t cur = s_hier.reveal_target;
    while (cur != 0) {
        if (cur == node_id)
            return true;
        JceEntityInfo *e = jce_state_get_entity(cur);
        if (!e)
            break;
        cur = e->parent_id;
    }

    return false;
}

static int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

static bool text_matches_filter_ci(const char *text, const char *filter)
{
    if (!filter || !filter[0]) return true;
    if (!text || !text[0]) return false;

    for (const char *h = text; *h; h++) {
        const char *a = h;
        const char *b = filter;
        while (*a && *b && ascii_tolower((unsigned char)*a) == ascii_tolower((unsigned char)*b)) {
            a++;
            b++;
        }
        if (!*b) return true;
    }

    return false;
}

static bool entity_matches_search_fields(const JceEntityInfo *e, const char *filter)
{
    if (!e) return false;
    if (!filter || !filter[0]) return true;

    if (text_matches_filter_ci(e->name, filter))
        return true;

    if (text_matches_filter_ci(e->tag, filter))
        return true;

    if (e->tag_color >= 0 && e->tag_color < JCE_TAG_COLOR_COUNT) {
        const char *tag_color_name = s_tag_names[e->tag_color];
        if (text_matches_filter_ci(tag_color_name, filter))
            return true;
    }

    return false;
}

static int name_compare_ci(const char *a, const char *b)
{
    if (!a) a = "";
    if (!b) b = "";

    while (*a && *b) {
        int ca = ascii_tolower((unsigned char)*a);
        int cb = ascii_tolower((unsigned char)*b);
        if (ca != cb) return ca - cb;
        a++;
        b++;
    }

    return ascii_tolower((unsigned char)*a) - ascii_tolower((unsigned char)*b);
}

static int compare_entities_for_sort(uint32_t lhs_id, uint32_t rhs_id)
{
    JceEntityInfo *lhs = jce_state_get_entity(lhs_id);
    JceEntityInfo *rhs = jce_state_get_entity(rhs_id);
    if (!lhs && !rhs) return 0;
    if (!lhs) return 1;
    if (!rhs) return -1;

    if (s_hier.sort_mode == 1)
        return name_compare_ci(lhs->name, rhs->name);

    if (s_hier.sort_mode == 2) {
        int tag_cmp = (int)lhs->tag_color - (int)rhs->tag_color;
        if (tag_cmp != 0) return tag_cmp;
        return name_compare_ci(lhs->name, rhs->name);
    }

    return 0;
}

static void sort_entity_ids(uint32_t *ids, int count)
{
    if (!ids || count <= 1 || s_hier.sort_mode == 0)
        return;

    /* Stable insertion sort: entity lists are small in editor hierarchy. */
    for (int i = 1; i < count; i++) {
        uint32_t key = ids[i];
        int j = i - 1;
        while (j >= 0 && compare_entities_for_sort(ids[j], key) > 0) {
            ids[j + 1] = ids[j];
            j--;
        }
        ids[j + 1] = key;
    }
}

static bool entity_matches_search_recursive(uint32_t entity_id, const char *filter)
{
    JceEntityInfo *e = jce_state_get_entity(entity_id);
    if (!e) return false;

    if (entity_matches_search_fields(e, filter))
        return true;

    for (int i = 0; i < e->child_count; i++) {
        if (entity_matches_search_recursive(e->children[i], filter))
            return true;
    }

    return false;
}

static void focus_entity_in_scene(uint32_t id)
{
    JceComponentInfo comps[JCE_MAX_COMPONENTS];
    int cc = jce_state_get_components(id, comps, JCE_MAX_COMPONENTS);
    for (int i = 0; i < cc; i++) {
        if (comps[i].type == JCE_COMP_TRANSFORM) {
            jce_editor_scene_camera_set_target(
                comps[i].data.transform.pos[0],
                comps[i].data.transform.pos[1],
                comps[i].data.transform.pos[2]);
            break;
        }
    }
    jce_editor_layout_request_focus_inspector();
}

static void draw_entity_node(JceEntityInfo *e)
{
    if (!e) return;

    if (s_hier.search_buf[0] && !entity_matches_search_recursive(e->id, s_hier.search_buf))
        return;

    if (s_hier.tag_filter > 0 && (int)e->tag_color != s_hier.tag_filter)
        return;

    /* Track draw-order index for Shift-range selection. */
    record_display_order(e->id);

    bool is_leaf     = (e->child_count == 0);
    bool is_selected = jce_state_is_selected(e->id);
    bool is_renaming = (s_hier.renaming_id == e->id);

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                ImGuiTreeNodeFlags_SpanAvailWidth;
    if (is_leaf)     flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (s_hier.search_buf[0]) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    if (!e->enabled)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

    if (e->tag_color != JCE_TAG_NONE) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec4 tc = s_tag_colors[e->tag_color];
        dl->AddCircleFilled(ImVec2(pos.x + 9, pos.y + ImGui::GetTextLineHeight() * 0.5f),
                            8.0f, ImGui::ColorConvertFloat4ToU32(tc));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18);
    }

    if (node_in_reveal_path(e->id))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    bool node_open = ImGui::TreeNodeEx((void *)(intptr_t)e->id, flags,
                                        "%s", is_renaming ? "" : e->name);

    if (s_hier.reveal_pending && e->id == s_hier.reveal_target) {
        ImGui::SetScrollHereY(0.35f);
        s_hier.reveal_pending = false;
    }

    if (is_renaming) {
        ImGui::SameLine();
        ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", s_hier.rename_buf, sizeof(s_hier.rename_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll)) {
            jce_state_rename_entity(e->id, s_hier.rename_buf);
            s_hier.renaming_id = 0;
        }
        if (ImGui::IsItemDeactivatedAfterEdit() || (!ImGui::IsItemActive() && s_hier.renaming_id)) {
            if (s_hier.rename_buf[0])
                jce_state_rename_entity(e->id, s_hier.rename_buf);
            s_hier.renaming_id = 0;
        }
    }

    /* ── Left-click: Windows Explorer selection logic ────────────── */
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !is_renaming) {
        bool ctrl  = ImGui::GetIO().KeyCtrl;
        bool shift = ImGui::GetIO().KeyShift;

        if (shift && s_hier.shift_anchor) {
            /* Shift+Click: range-select from anchor to this node. */
            select_range(s_hier.shift_anchor, e->id);
        } else if (ctrl) {
            /* Ctrl+Click: toggle this node in/out of selection. */
            if (jce_state_is_selected(e->id))
                jce_state_deselect_entity(e->id);
            else
                jce_state_select_entity(e->id, true);
            s_hier.shift_anchor = e->id;
        } else {
            /* Plain click: exclusive select. */
            jce_state_select_entity(e->id, false);
            s_hier.shift_anchor = e->id;
        }
        jce_editor_inspector_request_sync();
    }

    /* Double-click: focus camera on entity. */
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && !is_renaming)
    {
        jce_state_select_entity(e->id, false);
        s_hier.shift_anchor = e->id;
        jce_editor_inspector_request_sync();
        focus_entity_in_scene(e->id);
    }

    /* ── Right-click: defer popup open (avoid ID stack mismatch) ── */
    bool item_hovered_ctx = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    if (item_hovered_ctx && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        s_hier.context_on_empty = false;
        s_hier.context_menu_id = e->id;
        s_hier.ctx_clicked_entity = true;

        /* Match Asset Browser semantics:
         * - right-click on selected item keeps multi-selection
         * - right-click on non-selected item switches to exclusive selection
         * In both cases, update focused entity to the right-clicked one. */
        if (jce_state_is_selected(e->id))
            jce_state_select_entity(e->id, true);
        else
            jce_state_select_entity(e->id, false);

        s_hier.shift_anchor = e->id;
        jce_editor_inspector_request_sync();
        s_hier.want_ctx_popup = true;
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("JCE_ENTITY", &e->id, sizeof(uint32_t));
        ImGui::Text("%s", e->name);
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            jce_state_reparent_entity(dragged_id, e->id);
        }
        ImGui::EndDragDropTarget();
    }

    if (!e->enabled)
        ImGui::PopStyleVar();

    if (node_open && !is_leaf) {
        uint32_t child_ids[JCE_MAX_CHILDREN];
        int child_count = 0;
        for (int i = 0; i < e->child_count && child_count < JCE_MAX_CHILDREN; i++) {
            child_ids[child_count++] = e->children[i];
        }
        sort_entity_ids(child_ids, child_count);

        for (int i = 0; i < child_count; i++) {
            JceEntityInfo *child = jce_state_get_entity(child_ids[i]);
            draw_entity_node(child);
        }
        ImGui::TreePop();
    }
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_hierarchy_content(void)
{
    ensure_init();

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
        /* Reset display-order list each frame (rebuilt by draw_entity_node). */
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

    /* Deferred OpenPopup at consistent ID stack level (outside entity loop). */
    if (s_hier.want_ctx_popup) {
        ImGui::OpenPopup("HierarchyContextMenu");
        s_hier.want_ctx_popup = false;
    }

    /* Context Menu */
    if (ImGui::BeginPopup("HierarchyContextMenu")) {
        JceEntityInfo *ctx_e = jce_state_get_entity(s_hier.context_menu_id);

        if (s_hier.context_on_empty || !ctx_e) {
            /* ── Empty-area context menu (matches reference) ──────── */
            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.createEmpty"))) {
                uint32_t id = jce_state_create_entity("New Entity", 0);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create3D"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube"))) {
                    uint32_t id = jce_state_create_entity("Cube", 0);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere"))) {
                    uint32_t id = jce_state_create_entity("Sphere", 0);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane"))) {
                    uint32_t id = jce_state_create_entity("Plane", 0);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem("Cylinder")) {
                    uint32_t id = jce_state_create_entity("Cylinder", 0);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create2D"))) {
                if (ImGui::MenuItem("Sprite")) {
                    uint32_t id = jce_state_create_entity("Sprite", 0);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem("Text")) {
                    uint32_t id = jce_state_create_entity("Text", 0);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                ImGui::EndMenu();
            }

            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera"))) {
                uint32_t id = jce_state_create_entity("Camera", 0);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }
            if (ImGui::MenuItem("Light")) {
                uint32_t id = jce_state_create_entity("Light", 0);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
                jce_editor_layout_request_focus_inspector();
            }

            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false,
                                jce_state_has_copied())) {
                uint32_t pasted = jce_state_paste_entity(0);
                if (pasted != 0) {
                    jce_state_select_entity(pasted, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
            }

            ImGui::Separator();
            if (ImGui::MenuItem("Select All", "Ctrl+A")) {
                jce_state_clear_selection();
                bool first = true;
                int total = jce_state_get_entity_count();
                for (int i = 0; i < total; i++) {
                    JceEntityInfo *se = jce_state_get_entity_by_index(i);
                    if (!se) continue;
                    jce_state_select_entity(se->id, !first);
                    first = false;
                }
                if (!first) {
                    s_hier.shift_anchor = jce_state_get_focused();
                    jce_editor_inspector_request_sync();
                }
            }
            if (ImGui::MenuItem("Deselect All", "Esc")) {
                jce_state_clear_selection();
                s_hier.shift_anchor = 0;
                jce_editor_inspector_request_sync();
            }
        } else {
            /* ── Object context menu (matches reference) ──────────── */

            int sel_count = 0;
            const uint32_t *sel = jce_state_get_selection(&sel_count);
            bool ctx_in_selection = false;
            for (int si = 0; si < sel_count; si++) {
                if (sel[si] == ctx_e->id) {
                    ctx_in_selection = true;
                    break;
                }
            }
            bool multi_on_ctx = ctx_in_selection && sel_count > 1;

            ImGui::TextDisabled("%s", ctx_e->name);
            ImGui::Separator();

            if (ImGui::MenuItem("Focus", "F")) {
                jce_state_select_entity(ctx_e->id, false);
                s_hier.shift_anchor = ctx_e->id;
                jce_editor_inspector_request_sync();
                focus_entity_in_scene(ctx_e->id);
            }

            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.rename"), "F2")) {
                s_hier.renaming_id = ctx_e->id;
                snprintf(s_hier.rename_buf, sizeof(s_hier.rename_buf), "%s", ctx_e->name);
            }

            if (ImGui::MenuItem(jce_editor_i18n("hierarchy.duplicate"), "Ctrl+D")) {
                uint32_t dup_ids[JCE_MAX_SELECTED];
                int dup_count = 0;

                if (multi_on_ctx) {
                    uint32_t src_ids[JCE_MAX_SELECTED];
                    int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                    for (int si = 0; si < n; si++)
                        src_ids[si] = sel[si];

                    for (int si = 0; si < n; si++) {
                        uint32_t dup = jce_state_duplicate_entity(src_ids[si]);
                        if (dup != 0 && dup_count < JCE_MAX_SELECTED)
                            dup_ids[dup_count++] = dup;
                    }
                } else {
                    uint32_t dup = jce_state_duplicate_entity(ctx_e->id);
                    if (dup != 0)
                        dup_ids[dup_count++] = dup;
                }

                if (dup_count > 0) {
                    jce_state_select_entity(dup_ids[0], false);
                    for (int di = 1; di < dup_count; di++)
                        jce_state_select_entity(dup_ids[di], true);
                    s_hier.shift_anchor = dup_ids[dup_count - 1];
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.createChild"))) {
                if (ImGui::MenuItem(jce_editor_i18n("hierarchy.createEmpty"))) {
                    uint32_t child = jce_state_create_entity("New Entity", ctx_e->id);
                    jce_state_select_entity(child, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create3D"))) {
                    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube"))) {
                        uint32_t id = jce_state_create_entity("Cube", ctx_e->id);
                        jce_state_select_entity(id, false);
                        jce_editor_inspector_request_sync();
                        jce_editor_layout_request_focus_inspector();
                    }
                    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere"))) {
                        uint32_t id = jce_state_create_entity("Sphere", ctx_e->id);
                        jce_state_select_entity(id, false);
                        jce_editor_inspector_request_sync();
                        jce_editor_layout_request_focus_inspector();
                    }
                    if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane"))) {
                        uint32_t id = jce_state_create_entity("Plane", ctx_e->id);
                        jce_state_select_entity(id, false);
                        jce_editor_inspector_request_sync();
                        jce_editor_layout_request_focus_inspector();
                    }
                    if (ImGui::MenuItem("Cylinder")) {
                        uint32_t id = jce_state_create_entity("Cylinder", ctx_e->id);
                        jce_state_select_entity(id, false);
                        jce_editor_inspector_request_sync();
                        jce_editor_layout_request_focus_inspector();
                    }
                    ImGui::EndMenu();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera"))) {
                    uint32_t id = jce_state_create_entity("Camera", ctx_e->id);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                if (ImGui::MenuItem("Light")) {
                    uint32_t id = jce_state_create_entity("Light", ctx_e->id);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
                ImGui::EndMenu();
            }

            /* Tag Color — inline colored circle buttons (matches reference image) */
            {
                ImGui::Text("%s", jce_editor_i18n("hierarchy.tagColor"));
                ImGui::SameLine(0.0f, 6.0f);
                const float tc_btn  = 28.0f; /* button square size (pixels) */
                const float tc_r    = 11.0f; /* circle radius */
                const float tc_gap  = 3.0f;  /* gap between circles */
                ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(tc_gap, 0.0f));
                for (int t = 1; t < JCE_TAG_COLOR_COUNT; t++) {
                    ImGui::PushID(t);
                    ImVec2 btn_pos = ImGui::GetCursorScreenPos();
                    ImGui::InvisibleButton("##tc", ImVec2(tc_btn, tc_btn));
                    bool tc_clicked = ImGui::IsItemClicked();
                    bool tc_hovered = ImGui::IsItemHovered();
                    ImDrawList *tc_dl = ImGui::GetWindowDrawList();
                    float ccx = btn_pos.x + tc_btn * 0.5f;
                    float ccy = btn_pos.y + tc_btn * 0.5f;
                    float cur_r = tc_hovered ? tc_r + 2.0f : tc_r;
                    tc_dl->AddCircleFilled(ImVec2(ccx, ccy), cur_r,
                        ImGui::ColorConvertFloat4ToU32(s_tag_colors[t]), 20);
                    if (ctx_e->tag_color == t)
                        tc_dl->AddCircle(ImVec2(ccx, ccy), cur_r + 2.5f,
                            IM_COL32(255, 255, 255, 200), 20, 1.5f);
                    if (tc_clicked) {
                        jce_state_set_entity_tag_color(ctx_e->id, (JceTagColor)t);
                        ImGui::CloseCurrentPopup();
                    }
                    if (t < JCE_TAG_COLOR_COUNT - 1) ImGui::SameLine();
                    ImGui::PopID();
                }
                ImGui::PopStyleVar(); /* ItemSpacing */
            }

            /* Set Tag */
            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.setTag"))) {
                const char *tag_presets[] = {
                    "Untagged", "Player", "Enemy", "MainCamera",
                    "Environment", "UI", "Trigger", "Respawn"
                };
                for (int t = 0; t < 8; t++) {
                    bool current = (strcmp(ctx_e->tag, tag_presets[t]) == 0);
                    if (ImGui::MenuItem(tag_presets[t], NULL, current))
                        jce_state_set_entity_tag(ctx_e->id, tag_presets[t]);
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.copy"), "Ctrl+C"))
                jce_state_copy_entity(ctx_e->id);
            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.paste"), "Ctrl+V", false, jce_state_has_copied())) {
                uint32_t pasted = jce_state_paste_entity(ctx_e->id);
                if (pasted != 0) {
                    jce_state_select_entity(pasted, false);
                    jce_editor_inspector_request_sync();
                    jce_editor_layout_request_focus_inspector();
                }
            }

            ImGui::Separator();

            ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_TEXT_ERROR);
            const char *delete_label = multi_on_ctx ? "Delete Selected" : jce_editor_i18n("hierarchy.delete");
            if (ImGui::MenuItem(delete_label, "Del")) {
                int selected_count = 0;
                const uint32_t *selected_ids = jce_state_get_selection(&selected_count);
                if (selected_count > 0) {
                    uint32_t ids[JCE_MAX_SELECTED];
                    int n = selected_count < JCE_MAX_SELECTED ? selected_count : JCE_MAX_SELECTED;
                    for (int si = 0; si < n; si++) ids[si] = selected_ids[si];
                    jce_editor_inspector_request_delete_confirm_many(ids, n);
                } else {
                    jce_editor_inspector_request_delete_confirm(ctx_e->id);
                }
            }
            ImGui::PopStyleColor();
        }
        ImGui::EndPopup();
    }

    ImGui::EndChild();

    /* Keyboard shortcuts */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        uint32_t focused = jce_state_get_focused();
        if (focused) {
            if (ImGui::IsKeyPressed(ImGuiKey_F2)) {
                s_hier.renaming_id = focused;
                JceEntityInfo *e = jce_state_get_entity(focused);
                if (e) snprintf(s_hier.rename_buf, sizeof(s_hier.rename_buf), "%s", e->name);
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
                uint32_t dup = jce_state_duplicate_entity(focused);
                jce_state_select_entity(dup, false);
            }
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_C))
                jce_state_copy_entity(focused);
            if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_V) && jce_state_has_copied()) {
                uint32_t pasted = jce_state_paste_entity(0); // paste at root
                jce_state_select_entity(pasted, false);
            }
        }
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_hierarchy(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_HIERARCHY);
    if (!*vis) return;

    if (ImGui::Begin("Hierarchy###Hierarchy", vis))
        jce_editor_panel_hierarchy_content();
    ImGui::End();
}
