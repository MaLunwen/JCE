/*
 * jce_panel_hierarchy_node.cpp  Entity tree node drawing + helpers.
 */

#include "jce_panel_hierarchy_internal.h"

extern "C" {
#include <jce/scene/jce_scene.h>
}

/* ── Tag colors (shared via internal header) ─────────────────────── */

const ImVec4 s_tag_colors[JCE_TAG_COLOR_COUNT] = {
    ImVec4(0, 0, 0, 0),
    JCE_COLOR_TAG_RED,
    JCE_COLOR_TAG_ORANGE,
    JCE_COLOR_TAG_YELLOW,
    JCE_COLOR_TAG_GREEN,
    JCE_COLOR_TAG_BLUE,
    JCE_COLOR_TAG_PURPLE,
    JCE_COLOR_TAG_GRAY,
};

const char *s_tag_names[] = {
    "None", "Red", "Orange", "Yellow", "Green", "Blue", "Purple", "Gray"
};

/* ── State instance (shared via extern in internal header) ───────── */

HierarchyState s_hier;

/* ── Shared helpers ──────────────────────────────────────────────── */

void ensure_hier_init(void)
{
    if (s_hier.initialized) return;
    memset(&s_hier, 0, sizeof(s_hier));
    s_hier.initialized = true;
}

void begin_rename_entity(uint32_t id, const char *name)
{
    s_hier.renaming_id = id;
    s_hier.rename_focus_pending = true;
    s_hier.rename_had_focus = false;
    snprintf(s_hier.rename_buf, sizeof(s_hier.rename_buf), "%s", name ? name : "");
}

void record_display_order(uint32_t id)
{
    if (s_hier.display_count < HIERARCHY_MAX_DISPLAY)
        s_hier.display_order[s_hier.display_count++] = id;
}

void select_range(uint32_t anchor, uint32_t target)
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

bool node_in_reveal_path(uint32_t node_id)
{
    if (!s_hier.reveal_pending || s_hier.reveal_target == 0)
        return false;

    uint32_t cur = s_hier.reveal_target;
    while (cur != 0) {
        if (cur == node_id)
            return true;
        if (!jce_state_entity_exists(cur))
            break;
        cur = jce_state_entity_parent(cur);
    }

    return false;
}

int ascii_tolower(int c)
{
    return (c >= 'A' && c <= 'Z') ? (c + 32) : c;
}

bool text_matches_filter_ci(const char *text, const char *filter)
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

bool entity_matches_search_fields(uint32_t entity_id, const char *filter)
{
    if (!jce_state_entity_exists(entity_id)) return false;
    if (!filter || !filter[0]) return true;

    if (text_matches_filter_ci(jce_state_entity_name(entity_id), filter))
        return true;

    if (text_matches_filter_ci(jce_state_entity_tag(entity_id), filter))
        return true;

    JceTagColor tc = jce_state_entity_tag_color(entity_id);
    if ((int)tc >= 0 && (int)tc < JCE_TAG_COLOR_COUNT) {
        const char *tag_color_name = s_tag_names[tc];
        if (text_matches_filter_ci(tag_color_name, filter))
            return true;
    }

    return false;
}

void build_default_prefab_path(const char *entity_name,
                               char *out_path,
                               size_t out_path_size)
{
    if (!out_path || out_path_size == 0)
        return;

    char slug[128];
    int w = 0;
    if (entity_name) {
        for (int i = 0; entity_name[i] != '\0' && w < (int)sizeof(slug) - 1; i++) {
            char c = entity_name[i];
            bool is_alpha = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
            bool is_digit = (c >= '0' && c <= '9');
            if (is_alpha || is_digit) {
                if (c >= 'A' && c <= 'Z')
                    c = (char)(c + ('a' - 'A'));
                slug[w++] = c;
            } else if (w > 0 && slug[w - 1] != '_') {
                slug[w++] = '_';
            }
        }
    }

    while (w > 0 && slug[w - 1] == '_')
        --w;
    slug[w] = '\0';

    if (slug[0] == '\0')
        snprintf(slug, sizeof(slug), "entity");

    snprintf(out_path, out_path_size, "assets/prefabs/%s.jprefab", slug);
}

int name_compare_ci(const char *a, const char *b)
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

int compare_entities_for_sort(uint32_t lhs_id, uint32_t rhs_id)
{
    bool lhs_ok = jce_state_entity_exists(lhs_id);
    bool rhs_ok = jce_state_entity_exists(rhs_id);
    if (!lhs_ok && !rhs_ok) return 0;
    if (!lhs_ok) return 1;
    if (!rhs_ok) return -1;

    const char *lhs_name = jce_state_entity_name(lhs_id);
    const char *rhs_name = jce_state_entity_name(rhs_id);

    if (s_hier.sort_mode == 1)
        return name_compare_ci(lhs_name, rhs_name);

    if (s_hier.sort_mode == 2) {
        int tag_cmp = (int)jce_state_entity_tag_color(lhs_id)
                    - (int)jce_state_entity_tag_color(rhs_id);
        if (tag_cmp != 0) return tag_cmp;
        return name_compare_ci(lhs_name, rhs_name);
    }

    return 0;
}

void sort_entity_ids(uint32_t *ids, int count)
{
    if (!ids || count <= 1 || s_hier.sort_mode == 0)
        return;

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

bool entity_matches_search_recursive(uint32_t entity_id, const char *filter)
{
    if (!jce_state_entity_exists(entity_id)) return false;

    if (entity_matches_search_fields(entity_id, filter))
        return true;

    uint32_t children[JCE_MAX_CHILDREN];
    int n = jce_state_entity_children(entity_id, children, JCE_MAX_CHILDREN);
    for (int i = 0; i < n; i++) {
        if (entity_matches_search_recursive(children[i], filter))
            return true;
    }

    return false;
}

void focus_entity_in_scene(uint32_t id)
{
    JceScene *scene = jce_state_get_scene();
    if (scene && jce_state_entity_exists(id)) {
        JceEntity e = jce_state_to_ecs_entity(id);
        JceTransform *t = jce_scene_get_transform(scene, e);
        if (t) {
            jce_editor_scene_camera_set_target(
                t->position.x, t->position.y, t->position.z);
        }
    }
    jce_editor_layout_request_focus_inspector();
}

/* ── Entity tree node ────────────────────────────────────────────── */

void draw_entity_node(uint32_t id)
{
    if (id == 0 || !jce_state_entity_exists(id)) return;

    if (s_hier.search_buf[0] && !entity_matches_search_recursive(id, s_hier.search_buf))
        return;

    JceTagColor tag_color = jce_state_entity_tag_color(id);
    if (s_hier.tag_filter > 0 && (int)tag_color != s_hier.tag_filter)
        return;

    record_display_order(id);

    int  child_count  = jce_state_entity_child_count(id);
    bool is_leaf      = (child_count == 0);
    bool is_selected  = jce_state_is_selected(id);
    bool is_renaming  = (s_hier.renaming_id == id);
    bool enabled      = jce_state_entity_enabled(id);
    bool is_prefab    = jce_state_entity_is_prefab(id);
    const char *name  = jce_state_entity_name(id);
    if (!name) name = "";

    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                ImGuiTreeNodeFlags_SpanAvailWidth;
    if (is_leaf)     flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (s_hier.search_buf[0]) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    if (!enabled)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

    if (tag_color != JCE_TAG_NONE) {
        ImVec2 pos = ImGui::GetCursorScreenPos();
        ImDrawList *dl = ImGui::GetWindowDrawList();
        ImVec4 tc = s_tag_colors[tag_color];
        dl->AddCircleFilled(ImVec2(pos.x + 9, pos.y + ImGui::GetTextLineHeight() * 0.5f),
                            8.0f, ImGui::ColorConvertFloat4ToU32(tc));
        ImGui::SetCursorPosX(ImGui::GetCursorPosX() + 18);
    }

    if (node_in_reveal_path(id))
        ImGui::SetNextItemOpen(true, ImGuiCond_Always);

    if (is_prefab)
        ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_HIER_PREFAB);
    bool node_open = ImGui::TreeNodeEx((void *)(intptr_t)id, flags,
                                        "%s", is_renaming ? "" : name);
    if (is_prefab)
        ImGui::PopStyleColor();

    if (s_hier.reveal_pending && id == s_hier.reveal_target) {
        ImGui::SetScrollHereY(0.35f);
        s_hier.reveal_pending = false;
    }

    if (is_renaming) {
        ImGui::SameLine();
        if (s_hier.rename_focus_pending)
            ImGui::SetKeyboardFocusHere();
        if (ImGui::InputText("##rename", s_hier.rename_buf, sizeof(s_hier.rename_buf),
                             ImGuiInputTextFlags_EnterReturnsTrue |
                             ImGuiInputTextFlags_AutoSelectAll)) {
            jce_state_rename_entity(id, s_hier.rename_buf);
            s_hier.renaming_id = 0;
            s_hier.rename_focus_pending = false;
            s_hier.rename_had_focus = false;
        }
        if (ImGui::IsItemActive()) {
            s_hier.rename_focus_pending = false;
            s_hier.rename_had_focus = true;
        }
        if (!s_hier.rename_focus_pending && s_hier.rename_had_focus && ImGui::IsItemDeactivated()) {
            if (s_hier.rename_buf[0])
                jce_state_rename_entity(id, s_hier.rename_buf);
            s_hier.renaming_id = 0;
            s_hier.rename_had_focus = false;
        }
    }

    /* ── Left-click: Windows Explorer selection logic ────────────── */
    if (ImGui::IsItemClicked(ImGuiMouseButton_Left) && !is_renaming) {
        bool ctrl  = ImGui::GetIO().KeyCtrl;
        bool shift = ImGui::GetIO().KeyShift;

        if (shift && s_hier.shift_anchor) {
            select_range(s_hier.shift_anchor, id);
        } else if (ctrl) {
            if (jce_state_is_selected(id))
                jce_state_deselect_entity(id);
            else
                jce_state_select_entity(id, true);
            s_hier.shift_anchor = id;
        } else {
            jce_state_select_entity(id, false);
            s_hier.shift_anchor = id;
        }
        jce_editor_inspector_request_sync();
    }

    /* Double-click: focus camera on entity. */
    if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
        && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
        && !is_renaming)
    {
        jce_state_select_entity(id, false);
        s_hier.shift_anchor = id;
        jce_editor_inspector_request_sync();
        focus_entity_in_scene(id);
    }

    /* ── Right-click: defer popup open ───────────────────────────── */
    bool item_hovered_ctx = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup);

    if (item_hovered_ctx && ImGui::IsMouseClicked(ImGuiMouseButton_Right)) {
        s_hier.context_on_empty = false;
        s_hier.context_menu_id = id;
        s_hier.ctx_clicked_entity = true;

        if (jce_state_is_selected(id))
            jce_state_select_entity(id, true);
        else
            jce_state_select_entity(id, false);

        s_hier.shift_anchor = id;
        jce_editor_inspector_request_sync();
        s_hier.want_ctx_popup = true;
    }

    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload("JCE_ENTITY", &id, sizeof(uint32_t));
        ImGui::Text("%s", name);
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            jce_state_reparent_entity(dragged_id, id);
        }
        ImGui::EndDragDropTarget();
    }

    /* ── Drop-between zone ───────────────────────────────────────── */
    {
        ImGui::PushID((int)id + 0x100000);
        float avail = ImGui::GetContentRegionAvail().x;
        ImGui::InvisibleButton("##drop_after", ImVec2(avail, 3.0f));

        if (ImGui::BeginDragDropTarget()) {
            ImVec2 p0 = ImGui::GetItemRectMin();
            ImVec2 p1 = ImGui::GetItemRectMax();
            ImGui::GetWindowDrawList()->AddLine(
                ImVec2(p0.x, p0.y + 1), ImVec2(p1.x, p0.y + 1),
                IM_COL32(100, 160, 255, 200), 2.0f);

            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload("JCE_ENTITY");
            if (payload) {
                uint32_t dragged_id = *(uint32_t *)payload->Data;
                if (dragged_id != id)
                    jce_state_reorder_sibling(dragged_id, id, true);
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    if (!enabled)
        ImGui::PopStyleVar();

    if (node_open && !is_leaf) {
        uint32_t child_ids[JCE_MAX_CHILDREN];
        int n = jce_state_entity_children(id, child_ids, JCE_MAX_CHILDREN);
        sort_entity_ids(child_ids, n);

        for (int i = 0; i < n; i++)
            draw_entity_node(child_ids[i]);
        ImGui::TreePop();
    }
}
