/*
 * jce_gizmo.cpp  Main gizmo orchestration — state, update loop, draw dispatch.
 *
 * Owns the internal gizmo state struct and orchestrates hit-testing,
 * drag application, and draw dispatch each frame.
 */

#include "jce_gizmo.h"
#include "jce_gizmo_compound_collider.h"

#include "core/jce_editor_defaults.h"

#include <jce/tools/jce_imgui.hpp>
#include <math.h>
#include <string.h>

/* ── Internal state ────────────────────────────────────────────────── */

typedef struct {
    bool         active;         /* gizmo is being dragged */
    JceGizmoAxis hovered_axis;   /* axis under mouse */
    JceGizmoAxis drag_axis;      /* axis being dragged */
    float        drag_prev_mouse[2]; /* previous mouse position */
    bool         initialized;
    float        axes[3][3];     /* current frame axis directions (local or world) */
    JceGizmoDimension dimension;
} JceGizmoState;

static JceGizmoState s_gizmo;

/* ── Internal state accessors (used by draw/interact modules) ──────── */

extern "C" JceGizmoAxis jce_gizmo_internal_hovered(void)
{
    return s_gizmo.hovered_axis;
}

extern "C" bool jce_gizmo_internal_dragging(void)
{
    return s_gizmo.active;
}

extern "C" JceGizmoAxis jce_gizmo_internal_drag_axis(void)
{
    return s_gizmo.drag_axis;
}

/* ── Axes accessor (used by draw/interact modules) ─────────────────── */

extern "C" void jce_gizmo_internal_get_axes(float ax_x[3], float ax_y[3], float ax_z[3])
{
    memcpy(ax_x, s_gizmo.axes[0], 3 * sizeof(float));
    memcpy(ax_y, s_gizmo.axes[1], 3 * sizeof(float));
    memcpy(ax_z, s_gizmo.axes[2], 3 * sizeof(float));
}

extern "C" JceGizmoDimension jce_gizmo_internal_dimension(void)
{
    return s_gizmo.dimension;
}

/* Compute gizmo axis directions based on space and entity rotation. */
static void compute_gizmo_axes(int gizmo_space, const float *rotation)
{
    if (gizmo_space == 0 /* LOCAL */ && rotation) {
        float rx = s_gizmo.dimension == JCE_GIZMO_DIMENSION_2D
                 ? 0.0f
                 : rotation[0] * JCE_DEG2RAD;
        float ry = s_gizmo.dimension == JCE_GIZMO_DIMENSION_2D
                 ? 0.0f
                 : rotation[1] * JCE_DEG2RAD;
        float rz = rotation[2] * JCE_DEG2RAD;

        /* Build rotation via engine euler convention (YXZ — same order as
         * jce_q_from_euler / jce_q_to_euler). The gizmo display MUST use the
         * same convention as ECS storage, otherwise the visible ring axes
         * diverge from the object's actual rotation axes each frame and
         * dragging produces visible jitter. */
        jce_quat q = jce_q_from_euler(rx, ry, rz);
        jce_mat4 m = jce_q_to_mat4(q);

        /* Extract rotated axes from matrix columns. */
        s_gizmo.axes[0][0] = m.col[0].x; s_gizmo.axes[0][1] = m.col[0].y; s_gizmo.axes[0][2] = m.col[0].z;
        s_gizmo.axes[1][0] = m.col[1].x; s_gizmo.axes[1][1] = m.col[1].y; s_gizmo.axes[1][2] = m.col[1].z;
        s_gizmo.axes[2][0] = m.col[2].x; s_gizmo.axes[2][1] = m.col[2].y; s_gizmo.axes[2][2] = m.col[2].z;
    } else {
        /* World space: identity axes */
        s_gizmo.axes[0][0] = 1; s_gizmo.axes[0][1] = 0; s_gizmo.axes[0][2] = 0;
        s_gizmo.axes[1][0] = 0; s_gizmo.axes[1][1] = 1; s_gizmo.axes[1][2] = 0;
        s_gizmo.axes[2][0] = 0; s_gizmo.axes[2][1] = 0; s_gizmo.axes[2][2] = 1;
    }
}

/* ── Hit-test functions (defined in jce_gizmo_interact.cpp) ────────── */

JceGizmoAxis jce_gizmo_hit_test_translate(const JceGizmoCamera *cam,
                                           const float *position,
                                           float scale_factor,
                                           float mouse_x, float mouse_y);
JceGizmoAxis jce_gizmo_hit_test_rotate(const JceGizmoCamera *cam,
                                        const float *position,
                                        float scale_factor,
                                        float mouse_x, float mouse_y);
JceGizmoAxis jce_gizmo_hit_test_scale(const JceGizmoCamera *cam,
                                       const float *position,
                                       float scale_factor,
                                       float mouse_x, float mouse_y);

/* Drag functions (defined in jce_gizmo_interact.cpp) */
void jce_gizmo_drag_translate(const JceGizmoCamera *cam,
                               JceGizmoAxis axis,
                               const float origin[3],
                               float mouse_x, float mouse_y,
                               float prev_mouse_x, float prev_mouse_y,
                               float out_delta[3]);
void jce_gizmo_drag_rotate(const JceGizmoCamera *cam,
                             JceGizmoAxis axis,
                             const float origin[3],
                             float mouse_x, float mouse_y,
                             float prev_mouse_x, float prev_mouse_y,
                             float out_delta_euler[3]);
void jce_gizmo_drag_scale(const JceGizmoCamera *cam,
                            JceGizmoAxis axis,
                            const float origin[3],
                            float mouse_x, float mouse_y,
                            float prev_mouse_x, float prev_mouse_y,
                            float out_delta_scale[3]);

/* Draw functions (defined in jce_gizmo_draw.cpp) — C++ linkage due to ImDrawList */
void jce_gizmo_draw_translate(ImDrawList *dl,
                               const JceGizmoCamera *cam,
                               float scale_factor,
                               const float *position);
void jce_gizmo_draw_rotate(ImDrawList *dl,
                             const JceGizmoCamera *cam,
                             float scale_factor,
                             const float *position);
void jce_gizmo_draw_scale(ImDrawList *dl,
                            const JceGizmoCamera *cam,
                            float scale_factor,
                            const float *position);

/* ── Lifecycle ─────────────────────────────────────────────────────── */

extern "C" void jce_gizmo_init(void)
{
    memset(&s_gizmo, 0, sizeof(s_gizmo));
    s_gizmo.initialized = true;
    s_gizmo.dimension = JCE_GIZMO_DIMENSION_3D;
}

extern "C" void jce_gizmo_shutdown(void)
{
    /* Drop the compound-collider gizmo's cooked-wireframe cache so it
     * doesn't leak across the session. */
    jce_gizmo_compound_collider_clear_cache();
    memset(&s_gizmo, 0, sizeof(s_gizmo));
}

extern "C" void jce_gizmo_set_dimension(JceGizmoDimension dimension)
{
    s_gizmo.dimension = dimension == JCE_GIZMO_DIMENSION_2D
                      ? JCE_GIZMO_DIMENSION_2D
                      : JCE_GIZMO_DIMENSION_3D;
}

extern "C" JceGizmoDimension jce_gizmo_get_dimension(void)
{
    return s_gizmo.dimension;
}

/* ── Update ────────────────────────────────────────────────────────── */

extern "C" bool jce_gizmo_update(const JceGizmoCamera *cam,
                                  int gizmo_mode,
                                  int gizmo_space,
                                  float scale_factor,
                                  float *inout_position,
                                  float *inout_rotation,
                                  float *inout_scale)
{
    if (!s_gizmo.initialized) return false;

    /* Compute axis directions based on Local/World space. */
    compute_gizmo_axes(gizmo_space, inout_rotation);

    ImGuiIO &io = ImGui::GetIO();
    float mx = io.MousePos.x;
    float my = io.MousePos.y;

    /* Ensure mouse is within the viewport region */
    bool in_viewport = (mx >= cam->viewport_origin[0] &&
                        my >= cam->viewport_origin[1] &&
                        mx <= cam->viewport_origin[0] + cam->viewport_size[0] &&
                        my <= cam->viewport_origin[1] + cam->viewport_size[1]);

    /* ── Drag in progress ───────────────────────────────────────── */
    if (s_gizmo.active) {
        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            /* End drag */
            s_gizmo.active = false;
            s_gizmo.drag_axis = JCE_GIZMO_AXIS_NONE;
            return true;
        }

        /* Compute delta */
        float delta[3] = {0,0,0};
        switch (gizmo_mode) {
            case 0: /* Translate */
                jce_gizmo_drag_translate(cam, s_gizmo.drag_axis, inout_position,
                                          mx, my,
                                          s_gizmo.drag_prev_mouse[0],
                                          s_gizmo.drag_prev_mouse[1],
                                          delta);
                inout_position[0] += delta[0];
                inout_position[1] += delta[1];
                inout_position[2] += delta[2];
                break;

            case 1: /* Rotate */
                jce_gizmo_drag_rotate(cam, s_gizmo.drag_axis, inout_position,
                                       mx, my,
                                       s_gizmo.drag_prev_mouse[0],
                                       s_gizmo.drag_prev_mouse[1],
                                       delta);
                inout_rotation[0] += delta[0];
                inout_rotation[1] += delta[1];
                inout_rotation[2] += delta[2];
                break;

            case 2: /* Scale */
                jce_gizmo_drag_scale(cam, s_gizmo.drag_axis, inout_position,
                                      mx, my,
                                      s_gizmo.drag_prev_mouse[0],
                                      s_gizmo.drag_prev_mouse[1],
                                      delta);
                for (int i = 0; i < 3; i++) {
                    if (fabsf(delta[i]) <= 0.000001f)
                        continue;
                    float factor = 1.0f + delta[i];
                    if (factor < 0.01f)
                        factor = 0.01f;
                    inout_scale[i] *= factor;
                }
                break;
        }

        s_gizmo.drag_prev_mouse[0] = mx;
        s_gizmo.drag_prev_mouse[1] = my;
        return true;
    }

    /* ── Not dragging — hover detection ─────────────────────────── */
    if (!in_viewport) {
        s_gizmo.hovered_axis = JCE_GIZMO_AXIS_NONE;
        return false;
    }

    JceGizmoAxis hit = JCE_GIZMO_AXIS_NONE;
    switch (gizmo_mode) {
        case 0: hit = jce_gizmo_hit_test_translate(cam, inout_position, scale_factor, mx, my); break;
        case 1: hit = jce_gizmo_hit_test_rotate(cam, inout_position, scale_factor, mx, my); break;
        case 2: hit = jce_gizmo_hit_test_scale(cam, inout_position, scale_factor, mx, my); break;
    }
    s_gizmo.hovered_axis = hit;

    /* ── Start drag ─────────────────────────────────────────────── */
    if (hit != JCE_GIZMO_AXIS_NONE && ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        s_gizmo.active = true;
        s_gizmo.drag_axis = hit;
        s_gizmo.drag_prev_mouse[0] = mx;
        s_gizmo.drag_prev_mouse[1] = my;
        return true;
    }

    return (hit != JCE_GIZMO_AXIS_NONE);
}

/* ── Draw ──────────────────────────────────────────────────────────── */

extern "C" void jce_gizmo_draw(struct ImDrawList *dl,
                                const JceGizmoCamera *cam,
                                int gizmo_mode,
                                int gizmo_space,
                                float scale_factor,
                                const float *position,
                                const float *rotation,
                                const float *scale)
{
    if (!s_gizmo.initialized) return;
    (void)scale;

    /* Recompute axes for drawing (in case draw is called without update). */
    compute_gizmo_axes(gizmo_space, rotation);

    switch (gizmo_mode) {
        case 0: jce_gizmo_draw_translate(dl, cam, scale_factor, position); break;
        case 1: jce_gizmo_draw_rotate(dl, cam, scale_factor, position); break;
        case 2: jce_gizmo_draw_scale(dl, cam, scale_factor, position); break;
    }
}

/* ── Query ─────────────────────────────────────────────────────────── */

extern "C" bool jce_gizmo_is_active(void)
{
    return s_gizmo.active;
}

extern "C" JceGizmoAxis jce_gizmo_hovered_axis(void)
{
    return s_gizmo.hovered_axis;
}

extern "C" void jce_gizmo_get_axes(float ax_x[3], float ax_y[3], float ax_z[3])
{
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);
}

extern "C" void jce_gizmo_cancel_interaction(void)
{
    s_gizmo.active = false;
    s_gizmo.hovered_axis = JCE_GIZMO_AXIS_NONE;
    s_gizmo.drag_axis = JCE_GIZMO_AXIS_NONE;
    s_gizmo.drag_prev_mouse[0] = 0.0f;
    s_gizmo.drag_prev_mouse[1] = 0.0f;
}
