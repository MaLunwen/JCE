/*
 * jce_panel_hierarchy_internal.h  Shared state for hierarchy panel files.
 */

#ifndef JCE_PANEL_HIERARCHY_INTERNAL_H
#define JCE_PANEL_HIERARCHY_INTERNAL_H

#include "ui/jce_editor_colors.h"
#include "jce_hierarchy_flat_cache.h"
#include "core/jce_editor_defaults.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_layout.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"

#include <jce/tools/jce_imgui.hpp>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

/* ── Tag colors (display data) ────────────────────────────────────── */

extern const ImVec4 s_tag_colors[JCE_TAG_COLOR_COUNT];
extern const char  *s_tag_names[];

/* Dynamic flattened rows retain the existing visible-row clipper. The three
 * backing arrays share a bounded 64 MiB budget; failure is surfaced, not a
 * silent partial hierarchy. Capacity is demand-based and reused across frames. */
struct HierarchyFlatRow { uint32_t id; int depth; bool chunk_header; };

struct HierarchyState {
    char     search_buf[128];
    int      sort_mode;
    int      tag_filter;
    uint32_t renaming_id;
    char     rename_buf[JCE_MAX_ENTITY_NAME];
    bool     rename_focus_pending;
    bool     rename_had_focus;
    uint32_t context_menu_id;
    bool     context_on_empty;
    bool     want_ctx_popup;
    bool     ctx_clicked_entity;
    bool     initialized;

    uint32_t *display_order;
    uint32_t *root_ids;
    int capacity;
    int      display_count;

    /* Flattened visible-tree row list, rebuilt on default-mode key changes by
     * jce_hierarchy_flatten().  Parallel to display_order (same ids/order)
     * but also carries the per-row indent depth so draw_entity_row() can
     * indent manually (the clipper renders rows out of recursion context).
     *
     * `chunk_header` rows are SYNTHETIC group nodes (one per loaded streamed
     * chunk) — `id` then holds the chunk id, not an entity id.  They are
     * NEVER ECS entities: the streamed entities they group are real rows
     * emitted (indented) right after.  This keeps the hierarchy usable when
     * Full-World loads tens of thousands of entities (collapsible chunk
     * nodes, expand on demand) and makes the hierarchy the chunk view —
     * without mutating the ECS scene graph or transforms. */
    HierarchyFlatRow *flat;
    int      flat_count;
    JceHierarchyFlatCache flat_cache;

    uint32_t shift_anchor;
    uint32_t last_focus_seen;
    uint32_t reveal_target;
    bool     reveal_pending;
};

extern HierarchyState s_hier;
bool jce_hierarchy_reserve(int loaded, int groups);

/* ── Shared helpers ───────────────────────────────────────────────── */

void ensure_hier_init(void);
void begin_rename_entity(uint32_t id, const char *name);
void record_display_order(uint32_t id);
void select_range(uint32_t anchor, uint32_t target);
bool node_in_reveal_path(uint32_t node_id);

int  ascii_tolower(int c);
bool text_matches_filter_ci(const char *text, const char *filter);
bool entity_matches_search_fields(uint32_t entity_id, const char *filter);
bool entity_matches_search_recursive(uint32_t entity_id, const char *filter);
void build_default_prefab_path(const char *entity_name, char *out_path, size_t out_path_size);
int  name_compare_ci(const char *a, const char *b);
int  compare_entities_for_sort(uint32_t lhs_id, uint32_t rhs_id);
void sort_entity_ids(uint32_t *ids, int count);
void focus_entity_in_scene(uint32_t id);

/* ── Functions from jce_panel_hierarchy_node.cpp ──────────────────── */

/* Walk the entity tree in display order (roots already gathered+sorted by the
 * caller, passed in here), honouring the persisted ImGui open-state, the search
 * filter and the tag filter, and populate s_hier.flat[] / s_hier.flat_count and
 * s_hier.display_order[] / s_hier.display_count with the FULL flattened visible
 * row list.  Must run before clipping. */
void jce_hierarchy_flatten(const uint32_t *root_ids, int root_count);

/* Render a single hierarchy row (one entity) at the given indent depth.  Does
 * NOT recurse into children and never pushes an ImGui tree level — the flatten
 * pass owns the tree structure.  Called per visible row by the clipper. */
void draw_entity_row(uint32_t entity_id, int depth);

/* Render a SYNTHETIC streamed-chunk group header row (collapsible label +
 * eye toggle that drives the preview FILTER set).  chunk_id is the streaming
 * chunk id; depth is the indent.  Not an ECS entity. */
void draw_chunk_group_row(uint32_t chunk_id, int depth);

/* True while the hierarchy is showing streamed chunks as group nodes (the
 * streaming preview is active and at least one chunk has spawned entities).
 * When true the flatten pass routes streamed entities under chunk headers
 * instead of listing them flat among the scene roots. */
bool jce_hierarchy_chunk_grouping_active(void);

/* ── Functions from jce_panel_hierarchy_menu.cpp ──────────────────── */

void draw_hierarchy_context_menu(void);

#endif /* JCE_PANEL_HIERARCHY_INTERNAL_H */
