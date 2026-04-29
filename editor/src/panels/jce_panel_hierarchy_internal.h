/*
 * jce_panel_hierarchy_internal.h  Shared state for hierarchy panel files.
 */

#ifndef JCE_PANEL_HIERARCHY_INTERNAL_H
#define JCE_PANEL_HIERARCHY_INTERNAL_H

#include "ui/jce_editor_colors.h"
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

/* ── Hierarchy state ──────────────────────────────────────────────── */

#define HIERARCHY_MAX_DISPLAY 4096

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

    uint32_t display_order[HIERARCHY_MAX_DISPLAY];
    int      display_count;
    uint32_t shift_anchor;
    uint32_t last_focus_seen;
    uint32_t reveal_target;
    bool     reveal_pending;
};

extern HierarchyState s_hier;

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

void draw_entity_node(uint32_t entity_id);

/* ── Functions from jce_panel_hierarchy_menu.cpp ──────────────────── */

void draw_hierarchy_context_menu(void);

#endif /* JCE_PANEL_HIERARCHY_INTERNAL_H */
