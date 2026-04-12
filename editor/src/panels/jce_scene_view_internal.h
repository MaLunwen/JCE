/*
 * jce_scene_view_internal.h  Shared state for scene view panel files.
 */

#ifndef JCE_SCENE_VIEW_INTERNAL_H
#define JCE_SCENE_VIEW_INTERNAL_H

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "jce_editor_layout.h"
#include "scene/jce_editor_scene_render.h"
#include "gizmo/jce_gizmo.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include <jce/core/jce_math.h>
}

/* ── Shared viewport context ─────────────────────────────────────── */

struct SceneViewCtx {
    ImVec2      avail;
    ImVec2      screen_pos;
    ImDrawList *dl;
    bool        viewport_hovered;
};

/* ── Selection box state ─────────────────────────────────────────── */

extern bool   s_is_selecting;
extern ImVec2 s_sel_start;
extern ImVec2 s_sel_current;
extern bool   s_sel_easter;
extern ImU32  s_sel_border;
extern ImU32  s_sel_fill;
extern ImU32  s_sel_inner;

extern bool   s_sel_pending;
extern ImVec2 s_sel_rect_min;
extern ImVec2 s_sel_rect_max;

extern bool   s_sel_click_pending;
extern ImVec2 s_sel_click_pos;

/* ── Gizmo raw drag state ────────────────────────────────────────── */

extern bool  s_gizmo_raw_dragging;
extern float s_gizmo_raw_pos[3];
extern float s_gizmo_raw_rot[3];
extern float s_gizmo_raw_scale[3];
extern bool  s_gizmo_history_batch_open;

/* ── Functions from jce_scene_view_helpers.cpp ────────────────────── */

void clear_stale_gizmo_interaction_state(void);
bool has_valid_gizmo_target(void);

JceComponentInfo *find_transform_component(JceComponentInfo *comps,
                                           int comp_count);
JceComponentInfo *find_component_by_type(JceComponentInfo *comps,
                                         int comp_count,
                                         JceComponentType type);

void draw_scene_helper_icons(ImDrawList *dl, const JceGizmoCamera *cam);

void set_entity_mesh_shape(uint32_t entity_id, int mesh_shape);
uint32_t create_default_scene_entity(const char *name,
                                     uint32_t parent_id,
                                     JceComponentType extra_type,
                                     int mesh_shape);

/* ── Functions from jce_scene_view_cube.cpp ───────────────────────── */

int draw_axis_indicator(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                        const float *view16);
int draw_view_cube(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                   const float *view16);

/* ── Functions from jce_scene_view_gizmo.cpp ─────────────────────── */

void handle_scene_selection_box(const SceneViewCtx *ctx);
void update_and_draw_scene_gizmo(const SceneViewCtx *ctx);

#endif /* JCE_SCENE_VIEW_INTERNAL_H */
