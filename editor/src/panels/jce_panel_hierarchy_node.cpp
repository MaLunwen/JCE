/*
 * jce_panel_hierarchy_node.cpp  Entity tree node drawing + helpers.
 */

#include "jce_panel_hierarchy_internal.h"
#include "ui/jce_editor_dnd.h"

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
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
        jce_editor_scene_camera_focus_entity(id);
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
                                ImGuiTreeNodeFlags_SpanAvailWidth |
                                ImGuiTreeNodeFlags_AllowOverlap;
    if (is_leaf)     flags |= ImGuiTreeNodeFlags_Leaf | ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (is_selected) flags |= ImGuiTreeNodeFlags_Selected;
    if (s_hier.search_buf[0]) flags |= ImGuiTreeNodeFlags_DefaultOpen;

    if (!enabled)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.5f);

    /* Ghost rows for entities currently held in the cut clipboard so the
     * user has visual feedback before pasting (mirrors asset browser). */
    bool is_cut_pending = false;
    if (jce_state_clipboard_is_cut()) {
        int cut_n = 0;
        const uint32_t *cut_ids = jce_state_clipboard_source_ids(&cut_n);
        for (int i = 0; i < cut_n; i++) {
            if (cut_ids[i] == id) { is_cut_pending = true; break; }
        }
    }
    if (is_cut_pending)
        ImGui::PushStyleVar(ImGuiStyleVar_Alpha, enabled ? 0.55f : 0.3f);

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

    /* Capture row click state IMMEDIATELY after the tree node — later
       SameLine widgets (variant icon, eye toggle, rename input) would
       otherwise overwrite the "last item" used by IsItemClicked. */
    bool node_clicked_left   = ImGui::IsItemClicked(ImGuiMouseButton_Left);
    /* AllowWhenBlockedByPopup so right-clicking another entity while the
       context menu is already open re-targets the menu instead of just
       closing it (default IsItemClicked is suppressed by the open popup). */
    bool node_clicked_right  = ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenBlockedByPopup)
                               && ImGui::IsMouseClicked(ImGuiMouseButton_Right);
    bool node_double_clicked = ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left)
                               && ImGui::IsItemHovered();

    /* Drag-drop reparent MUST be issued HERE — right after the tree-node row,
       BEFORE the SameLine widgets (variant diamond, eye toggle, rename input).
       ImGui binds BeginDragDropSource/Target to the LAST submitted item; when
       these blocks lived further down (after the eye InvisibleButton) they
       silently attached to the eye button, so dragging the row did nothing and
       no parent-child link was ever made. Same hazard the click capture above
       guards against. */
    if (ImGui::BeginDragDropSource(ImGuiDragDropFlags_SourceAllowNullID)) {
        ImGui::SetDragDropPayload(JCE_DND_ENTITY, &id, sizeof(uint32_t));
        if (jce_state_is_selected(id)) {
            int sel_n = 0;
            jce_state_get_selection(&sel_n);
            if (sel_n > 1)
                ImGui::Text("%s  (+%d)", name, sel_n - 1);
            else
                ImGui::Text("%s", name);
        } else {
            ImGui::Text("%s", name);
        }
        ImGui::EndDragDropSource();
    }

    if (ImGui::BeginDragDropTarget()) {
        const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(JCE_DND_ENTITY);
        if (payload) {
            uint32_t dragged_id = *(uint32_t *)payload->Data;
            if (jce_state_is_selected(dragged_id)) {
                int sel_n = 0;
                const uint32_t *sel = jce_state_get_selection(&sel_n);
                /* Copy out: reparenting may invalidate the pointer. */
                uint32_t ids[256];
                int n = sel_n < 256 ? sel_n : 256;
                for (int i = 0; i < n; i++) ids[i] = sel[i];
                for (int i = 0; i < n; i++) {
                    if (ids[i] == id) continue;
                    jce_state_reparent_entity(ids[i], id);
                }
            } else {
                jce_state_reparent_entity(dragged_id, id);
            }
        }
        ImGui::EndDragDropTarget();
    }

    /* Variant indicator: cyan diamond drawn as a primitive so it never
       depends on whether the active font ships Geometric Shapes (KaiTi
       and many CJK fonts don't, which would render as '?'/tofu). */
    {
        const char *vparent = jce_state_get_variant_parent(id);
        if (vparent) {
            ImGui::SameLine(0.0f, 4.0f);
            ImDrawList *dl = ImGui::GetWindowDrawList();
            float h = ImGui::GetTextLineHeight();
            ImVec2 p = ImGui::GetCursorScreenPos();
            float cx = p.x + h * 0.5f;
            float cy = p.y + h * 0.5f;
            float r  = h * 0.32f;
            ImU32 col = IM_COL32(102, 217, 242, 255);
            ImVec2 pts[4] = {
                ImVec2(cx,     cy - r),
                ImVec2(cx + r, cy    ),
                ImVec2(cx,     cy + r),
                ImVec2(cx - r, cy    ),
            };
            dl->AddConvexPolyFilled(pts, 4, col);
            ImGui::Dummy(ImVec2(h, h));
            if (ImGui::IsItemHovered()) {
                ImGui::BeginTooltip();
                ImGui::TextUnformatted(jce_editor_i18n("hierarchy.tooltip.prefabVariant"));
                ImGui::Separator();
                ImGui::Text(jce_editor_i18n("hierarchy.tooltip.parent"), vparent);
                ImGui::EndTooltip();
            }
        }
    }

    if (s_hier.reveal_pending && id == s_hier.reveal_target) {
        ImGui::SetScrollHereY(0.35f);
        s_hier.reveal_pending = false;
    }

    /* Right-aligned eye toggle: click to flip per-entity enabled state.
     * Open eye (◉) = visible/enabled, hollow circle (○) = disabled.
     * Sits at row's right edge so it doesn't shift the name column. */
    /* Right-aligned eye toggle: filled circle = visible, hollow = hidden.
       Drawn as primitives instead of glyphs so the indicator is always
       visible regardless of font coverage. */
    if (!is_renaming) {
        float h = ImGui::GetTextLineHeight();
        float btn_w = h;
        float row_right = ImGui::GetWindowContentRegionMax().x;
        float btn_x = row_right - btn_w - 4.0f;
        ImGui::SameLine(btn_x);
        ImGui::PushID((int)id ^ 0x4000);
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.20f));
        if (!enabled)
            ImGui::PushStyleVar(ImGuiStyleVar_Alpha, 0.55f);

        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemAllowOverlap();
        bool clicked = ImGui::InvisibleButton("##eye", ImVec2(btn_w, h));
        bool hovered = ImGui::IsItemHovered();

        /* If the click landed inside this eye button, suppress the row's
           selection click captured earlier so the toggle is a "pure"
           interaction. */
        if (hovered && (clicked || ImGui::IsItemActive())) {
            node_clicked_left = false;
        }

        ImDrawList *dl = ImGui::GetWindowDrawList();
        float cx = p.x + btn_w * 0.5f;
        float cy = p.y + h * 0.5f;
        float r  = h * 0.32f;
        ImU32 col = enabled ? IM_COL32(220, 220, 220, 255)
                            : IM_COL32(140, 140, 140, 200);
        if (hovered) col = IM_COL32(255, 255, 255, 255);
        if (enabled) {
            dl->AddCircleFilled(ImVec2(cx, cy), r, col, 16);
        } else {
            dl->AddCircle(ImVec2(cx, cy), r, col, 16, 1.5f);
        }

        if (clicked) {
            jce_state_set_entity_enabled(id, !enabled);
        }
        if (!enabled)
            ImGui::PopStyleVar();
        if (hovered)
            ImGui::SetTooltip("%s", jce_editor_i18n(enabled
                ? "hierarchy.tooltip.hide"
                : "hierarchy.tooltip.show"));
        ImGui::PopStyleColor(3);
        ImGui::PopID();
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
    if (node_clicked_left && !is_renaming) {
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
    if (node_double_clicked && !is_renaming)
    {
        jce_state_select_entity(id, false);
        s_hier.shift_anchor = id;
        jce_editor_inspector_request_sync();
        focus_entity_in_scene(id);
    }

    /* ── Right-click: defer popup open ───────────────────────────── */
    if (node_clicked_right) {
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

            const ImGuiPayload *payload = ImGui::AcceptDragDropPayload(JCE_DND_ENTITY);
            if (payload) {
                uint32_t dragged_id = *(uint32_t *)payload->Data;
                if (jce_state_is_selected(dragged_id)) {
                    int sel_n = 0;
                    const uint32_t *sel = jce_state_get_selection(&sel_n);
                    uint32_t ids[256];
                    int n = sel_n < 256 ? sel_n : 256;
                    for (int i = 0; i < n; i++) ids[i] = sel[i];
                    /* Insert in reverse so first selected ends up just after id. */
                    for (int i = n - 1; i >= 0; i--) {
                        if (ids[i] == id) continue;
                        jce_state_reorder_sibling(ids[i], id, true);
                    }
                } else if (dragged_id != id) {
                    jce_state_reorder_sibling(dragged_id, id, true);
                }
            }
            ImGui::EndDragDropTarget();
        }
        ImGui::PopID();
    }

    if (is_cut_pending)
        ImGui::PopStyleVar();
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
