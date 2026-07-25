/*
 * jce_panel_hierarchy_node.cpp  Entity tree node drawing + helpers.
 */

#include "jce_panel_hierarchy_internal.h"
#include "jce_panel_common.h"
#include "ui/jce_editor_dnd.h"

#include <algorithm>     /* std::sort — O(n log n) root sort at full-load */
#include <unordered_set> /* streamed-id set for chunk grouping */

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
    return jce_panel_filter_match_ci(text, filter);
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

    /* O(n log n) — was insertion sort, which is O(n^2) and blows up on the
     * thousands of roots a full-loaded world produces.  compare_entities_for_sort
     * is a strict-weak comparator (name/type with id tiebreak). */
    std::sort(ids, ids + count, [](uint32_t a, uint32_t b) {
        return compare_entities_for_sort(a, b) < 0;
    });
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

/* ── Flatten (build the clipped row list) ────────────────────────── */

/* Is this node currently displayed expanded?  Reads ImGui's persisted
 * per-node open state (the same bit the TreeNodeEx arrow toggles), falling
 * back to a default of "open" while a search is active or the node lies on the
 * reveal path — mirroring the DefaultOpen / SetNextItemOpen behaviour the row
 * renderer applies, so the flattened structure stays in sync with what the
 * tree nodes draw. */
static bool hierarchy_node_is_open(uint32_t id)
{
    int default_open =
        (s_hier.search_buf[0] || node_in_reveal_path(id)) ? 1 : 0;
    ImGuiStorage *storage = ImGui::GetStateStorage();
    ImGuiID node_imgui_id = ImGui::GetID((void *)(intptr_t)id);
    return storage->GetInt(node_imgui_id, default_open) != 0;
}

static void hierarchy_flatten_recurse(uint32_t id, int depth)
{
    if (id == 0 || !jce_state_entity_exists(id)) return;

    /* Same search filter the per-row renderer used to apply: hide a node (and
     * its whole subtree) when neither it nor any descendant matches. */
    if (s_hier.search_buf[0]
        && !entity_matches_search_recursive(id, s_hier.search_buf))
        return;

    /* Same tag filter: a tag-excluded node skips itself and its subtree. */
    JceTagColor tag_color = jce_state_entity_tag_color(id);
    if (s_hier.tag_filter > 0 && (int)tag_color != s_hier.tag_filter)
        return;

    if (s_hier.flat_count >= HIERARCHY_MAX_DISPLAY)
        return;

    /* Record into both the flat row list and the full display order (used by
     * shift-range select and alpha-jump — must reflect EVERY visible row, not
     * just the clipper-visible subset). */
    s_hier.flat[s_hier.flat_count].id           = id;
    s_hier.flat[s_hier.flat_count].depth        = depth;
    s_hier.flat[s_hier.flat_count].chunk_header = false;
    s_hier.flat_count++;
    record_display_order(id);

    int child_count = jce_state_entity_child_count(id);
    if (child_count > 0 && hierarchy_node_is_open(id)) {
        uint32_t child_ids[JCE_MAX_CHILDREN];
        int n = jce_state_entity_children(id, child_ids, JCE_MAX_CHILDREN);
        sort_entity_ids(child_ids, n);
        for (int i = 0; i < n; i++)
            hierarchy_flatten_recurse(child_ids[i], depth + 1);
    }
}

/* ── Streamed-chunk grouping ─────────────────────────────────────────
 * When the streaming preview is active we present each loaded chunk as a
 * synthetic collapsible header with the chunk's spawned ROOT entities nested
 * underneath (their descendants recurse normally).  This keeps the hierarchy
 * usable when Full-World loads tens of thousands of entities and makes the
 * hierarchy the chunk view.  Base-scene (non-streamed) entities are listed
 * exactly as before — streamed roots are simply rerouted out of the flat
 * root list and under their chunk header. */

bool jce_hierarchy_chunk_grouping_active(void)
{
    return jce_state_get_streaming_preview() &&
           jce_state_streaming_group_count() > 0;
}

/* Is this open?  Synthetic chunk headers default OPEN only on the reveal path
 * (full-world would otherwise expand 200+ chunks × thousands of rows); the
 * persisted bit drives normal expand/collapse.  Keyed disjoint from entity ids
 * via a salt so a chunk id never collides with an entity-node ImGui id. */
static const uintptr_t kChunkHeaderSalt = 0x6368756Bu; /* 'chuk' */
static bool chunk_header_is_open(uint32_t chunk_id)
{
    ImGuiStorage *storage = ImGui::GetStateStorage();
    ImGuiID hid = ImGui::GetID((void *)(kChunkHeaderSalt ^ (uintptr_t)chunk_id));
    return storage->GetInt(hid, /*default*/0) != 0;
}

/* Collect every streamed entity id (across all chunk groups) into a set so the
 * root pass can skip them — they appear under their chunk header instead. */
static void collect_streamed_ids(std::unordered_set<uint32_t> &out)
{
    uint32_t chunk_ids[JCE_SCENE_MAX_STREAM_CHUNKS];
    uint32_t nc = jce_state_streaming_group_chunk_ids(chunk_ids,
                                                      JCE_SCENE_MAX_STREAM_CHUNKS);
    for (uint32_t i = 0; i < nc; ++i) {
        uint32_t cnt = 0;
        const uint32_t *ents =
            jce_state_streaming_chunk_entities(chunk_ids[i], &cnt);
        for (uint32_t j = 0; j < cnt; ++j) out.insert(ents[j]);
    }
}

static void flatten_chunk_groups(void)
{
    uint32_t chunk_ids[JCE_SCENE_MAX_STREAM_CHUNKS];
    uint32_t nc = jce_state_streaming_group_chunk_ids(chunk_ids,
                                                      JCE_SCENE_MAX_STREAM_CHUNKS);
    for (uint32_t i = 0; i < nc; ++i) {
        uint32_t chunk_id = chunk_ids[i];
        uint32_t cnt = 0;
        const uint32_t *ents = jce_state_streaming_chunk_entities(chunk_id, &cnt);
        if (!ents || cnt == 0) continue;

        if (s_hier.flat_count >= HIERARCHY_MAX_DISPLAY) break;

        /* Synthetic header row. */
        s_hier.flat[s_hier.flat_count].id           = chunk_id;
        s_hier.flat[s_hier.flat_count].depth        = 0;
        s_hier.flat[s_hier.flat_count].chunk_header = true;
        s_hier.flat_count++;
        /* NOTE: not recorded into display_order (alpha-jump / range-select
         * operate on real entities only). */

        if (!chunk_header_is_open(chunk_id)) continue;

        /* Emit the chunk's ROOT entities (parent == none) at depth 1; their
         * descendants recurse normally.  Children of streamed roots have a
         * real ECS parent so they are not themselves roots. */
        for (uint32_t j = 0; j < cnt; ++j) {
            uint32_t e = ents[j];
            if (!jce_state_entity_exists(e)) continue;
            if (jce_state_entity_parent(e) != 0) continue;   /* not a root */
            hierarchy_flatten_recurse(e, 1);
        }
    }
}

void jce_hierarchy_flatten(const uint32_t *root_ids, int root_count)
{
    s_hier.flat_count    = 0;
    s_hier.display_count = 0;

    const bool grouping = jce_hierarchy_chunk_grouping_active();
    std::unordered_set<uint32_t> streamed;
    if (grouping) collect_streamed_ids(streamed);

    /* Base-scene roots first (skip streamed roots when grouping — they go under
     * their chunk header). */
    for (int i = 0; i < root_count; i++) {
        if (grouping && streamed.find(root_ids[i]) != streamed.end())
            continue;
        hierarchy_flatten_recurse(root_ids[i], 0);
    }

    /* Then one collapsible node per loaded chunk. */
    if (grouping) flatten_chunk_groups();
}

/* ── Streamed-chunk group header row (synthetic, clipper-friendly) ─── */

/* Switch the preview into FILTER mode, seeding the filter from the chunks that
 * are currently loaded (so flipping one chunk off doesn't unload everything),
 * then toggle `chunk_id`.  Used by the chunk-header eye when not already in
 * FILTER mode so the eye is a one-click "isolate / hide this chunk". */
static void chunk_eye_toggle(uint32_t chunk_id, bool want_shown)
{
    if (jce_state_streaming_get_preview_mode() != JCE_STREAM_PREVIEW_FILTER) {
        uint32_t loaded_ids[JCE_SCENE_MAX_STREAM_CHUNKS];
        uint32_t n = jce_state_streaming_group_chunk_ids(
            loaded_ids, JCE_SCENE_MAX_STREAM_CHUNKS);
        jce_state_streaming_filter_set_all(loaded_ids, n);  /* current = loaded */
        jce_state_streaming_set_preview_mode(JCE_STREAM_PREVIEW_FILTER);
    }
    jce_state_streaming_filter_set(chunk_id, want_shown);
}

void draw_chunk_group_row(uint32_t chunk_id, int depth)
{
    uint32_t ent_count = 0;
    (void)jce_state_streaming_chunk_entities(chunk_id, &ent_count);

    float indent_w = depth * ImGui::GetTreeNodeToLabelSpacing();
    if (indent_w > 0.0f) ImGui::Indent(indent_w);

    /* In FILTER mode the eye reflects membership; otherwise the chunk is
     * loaded (shown) by definition of being grouped here. */
    bool in_filter_mode =
        jce_state_streaming_get_preview_mode() == JCE_STREAM_PREVIEW_FILTER;
    bool shown = in_filter_mode
               ? jce_state_streaming_filter_contains(chunk_id)
               : true;

    /* Collapsible header — keyed by the same salted id the flatten pass uses so
     * the persisted open bit matches.  Use a distinct void* key. */
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                ImGuiTreeNodeFlags_SpanAvailWidth |
                                ImGuiTreeNodeFlags_AllowOverlap |
                                ImGuiTreeNodeFlags_NoTreePushOnOpen;

    ImGui::PushStyleColor(ImGuiCol_Text, JCE_COLOR_HIER_PREFAB);
    ImGui::TreeNodeEx((void *)(kChunkHeaderSalt ^ (uintptr_t)chunk_id), flags,
                      "%s %u  (%u)",
                      jce_editor_i18n_or("hierarchy.chunk", "Chunk"),
                      chunk_id, ent_count);
    ImGui::PopStyleColor();

    /* Right-aligned eye: filled = shown, hollow = hidden.  Drives the FILTER. */
    {
        float h = ImGui::GetTextLineHeight();
        float btn_w = h;
        float row_right = ImGui::GetWindowContentRegionMax().x;
        float btn_x = row_right - btn_w - 4.0f;
        ImGui::SameLine(btn_x);
        ImGui::PushID((int)(chunk_id ^ 0x5000u));
        ImGui::PushStyleColor(ImGuiCol_Button,        ImVec4(0,0,0,0));
        ImGui::PushStyleColor(ImGuiCol_ButtonHovered, ImVec4(1,1,1,0.10f));
        ImGui::PushStyleColor(ImGuiCol_ButtonActive,  ImVec4(1,1,1,0.20f));

        ImVec2 p = ImGui::GetCursorScreenPos();
        ImGui::SetNextItemAllowOverlap();
        bool clicked = ImGui::InvisibleButton("##chunk_eye", ImVec2(btn_w, h));
        bool hovered = ImGui::IsItemHovered();

        ImDrawList *dl = ImGui::GetWindowDrawList();
        float cx = p.x + btn_w * 0.5f;
        float cy = p.y + h * 0.5f;
        float r  = h * 0.32f;
        ImU32 col = shown ? IM_COL32(220, 220, 220, 255)
                          : IM_COL32(140, 140, 140, 200);
        if (hovered) col = IM_COL32(255, 255, 255, 255);
        if (shown) dl->AddCircleFilled(ImVec2(cx, cy), r, col, 16);
        else       dl->AddCircle(ImVec2(cx, cy), r, col, 16, 1.5f);

        if (clicked) chunk_eye_toggle(chunk_id, !shown);
        if (hovered)
            ImGui::SetTooltip("%s", jce_editor_i18n_or(
                shown ? "hierarchy.chunk.hide" : "hierarchy.chunk.show",
                shown ? "Hide this chunk (filter)" : "Show this chunk (filter)"));
        ImGui::PopStyleColor(3);
        ImGui::PopID();
    }

    if (indent_w > 0.0f) ImGui::Unindent(indent_w);
}

/* ── Entity tree row (single, clipper-friendly) ──────────────────── */

void draw_entity_row(uint32_t id, int depth)
{
    if (id == 0 || !jce_state_entity_exists(id)) return;

    /* Search / tag filtering and display-order recording already happened in
     * jce_hierarchy_flatten(); the row renderer just draws. */

    /* Manual indent: the flatten pass owns the tree structure, so each row
     * indents itself by depth (Indent here, matching Unindent at the tail). */
    float indent_w = depth * ImGui::GetTreeNodeToLabelSpacing();
    if (indent_w > 0.0f)
        ImGui::Indent(indent_w);

    JceTagColor tag_color = jce_state_entity_tag_color(id);

    int  child_count  = jce_state_entity_child_count(id);
    bool is_leaf      = (child_count == 0);
    bool is_selected  = jce_state_is_selected(id);
    bool is_renaming  = (s_hier.renaming_id == id);
    bool enabled      = jce_state_entity_enabled(id);
    bool is_prefab    = jce_state_entity_is_prefab(id);
    const char *name  = jce_state_entity_name(id);
    if (!name) name = "";

    /* NoTreePushOnOpen on EVERY node (leaf and non-leaf): the flatten pass
     * handles children, so TreeNodeEx must never push a tree level here.  The
     * arrow still toggles the persisted open bit, which the next frame's
     * flatten reads (a 1-frame expand lag is acceptable). */
    ImGuiTreeNodeFlags flags = ImGuiTreeNodeFlags_OpenOnArrow |
                                ImGuiTreeNodeFlags_SpanAvailWidth |
                                ImGuiTreeNodeFlags_AllowOverlap |
                                ImGuiTreeNodeFlags_NoTreePushOnOpen;
    if (is_leaf)     flags |= ImGuiTreeNodeFlags_Leaf;
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
    /* With NoTreePushOnOpen the return value (open?) is unused — the flatten
     * pass reads the persisted open bit directly, and there is no tree level
     * to pop.  The call still draws the arrow + toggles that bit. */
    ImGui::TreeNodeEx((void *)(intptr_t)id, flags,
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

    /* Match the manual indent applied at the top.  Children are emitted as
     * their own flattened rows (with their own depth), not recursed here. */
    if (indent_w > 0.0f)
        ImGui::Unindent(indent_w);
}
