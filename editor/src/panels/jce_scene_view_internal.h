/*
 * jce_scene_view_internal.h  Shared state for scene view panel files.
 */

#ifndef JCE_SCENE_VIEW_INTERNAL_H
#define JCE_SCENE_VIEW_INTERNAL_H

#include "gizmo/jce_gizmo.h"
#include "ui/jce_editor_colors.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_layout.h"
#include "ui/jce_editor_panels.h"
#include "core/jce_editor_state.h"
#include "scene/jce_editor_scene_render.h"
#include "jce_scene_view_input_policy.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/tools/jce_imgui_internal.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/middleware/scene/jce_scene.h>
#include <jce/os/core/jce_math.h>
}

/* ── Local mesh shape constants (procedural primitives) ───────────── */

#ifndef JCE_MESH_SHAPE_CUBE
#define JCE_MESH_SHAPE_CUBE     0
#define JCE_MESH_SHAPE_SPHERE   1
#define JCE_MESH_SHAPE_PLANE    2
#define JCE_MESH_SHAPE_CAPSULE  3
#define JCE_MESH_SHAPE_CYLINDER 4
#endif

/* ── Quat ↔ Euler degrees (shared editor helper) ──────────────────── */

#include "core/jce_editor_quat.h"

/* ── Shared viewport context ─────────────────────────────────────── */

struct SceneViewCtx {
    ImVec2      avail;
    ImVec2      screen_pos;
    ImDrawList *dl;
    bool        viewport_hovered;
    bool        viewport_active;
    bool        viewport_left_clicked;
    bool        viewport_right_clicked;
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

/* Per-entity persistent euler cache. JceTransform stores rotation as a
 * quaternion, but the gizmo + inspector both operate in euler degrees.
 * Round-tripping quat→euler→quat every frame collapses rotations whenever
 * pitch crosses the YXZ gimbal-lock branch at ±90° — X drag past 90°
 * would suddenly push 180° into Y/Z values. To avoid this we keep the
 * editor's own authoritative euler for the focused entity, only
 * re-decomposing from the quaternion when the transform was modified
 * externally (undo, scene reload, focus change). */
bool jce_editor_get_cached_euler_deg(uint32_t entity_id, jce_quat current_q, float out_deg[3]);
void jce_editor_set_cached_euler_deg(uint32_t entity_id, jce_quat q, const float deg[3]);

extern bool  s_gizmo_raw_dragging;
extern float s_gizmo_raw_pos[3];
extern float s_gizmo_raw_rot[3];
extern float s_gizmo_raw_scale[3];
extern bool  s_gizmo_transaction_open;

/* ── Functions from jce_scene_view_helpers.cpp ────────────────────── */

void clear_stale_gizmo_interaction_state(void);
bool has_valid_gizmo_target(void);

void draw_scene_helper_icons(ImDrawList *dl, const JceGizmoCamera *cam);

void set_entity_mesh_shape(uint32_t entity_id, int mesh_shape);

/* extra_comp_flag = 0 means "no additional component beyond the default
 * Transform + EditorMeta added by jce_state_create_entity". Otherwise
 * pass a single JCE_COMP_FLAG_* value. */
uint32_t create_default_scene_entity(const char *name,
                                     uint32_t parent_id,
                                     uint64_t extra_comp_flag,
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
