/*
 * jce_gizmo_draw.cpp  ImDrawList rendering for T/R/S gizmo manipulators.
 *
 * All drawing is done in screen-space via world_to_screen projection.
 * Uses ImDrawList::Add* primitives — no bgfx resources needed.
 */

#include "jce_editor_colors.h"
#include "jce_editor_defaults.h"
#include "jce_editor_state.h"
#include "jce_gizmo.h"
#include "jce_gizmo_internal.h"

#include <imgui.h>
#include <math.h>

/* ── Color helpers ─────────────────────────────────────────────────── */

static ImU32 axis_color(JceGizmoAxis axis, JceGizmoAxis hovered, JceGizmoAxis dragging)
{
    if (dragging == axis)
        return ImGui::GetColorU32(JCE_COLOR_GIZMO_ACTIVE);
    if (hovered == axis)
        return ImGui::GetColorU32(JCE_COLOR_GIZMO_HOVERED);

    switch (axis) {
        case JCE_GIZMO_AXIS_X:  return ImGui::GetColorU32(JCE_COLOR_GIZMO_X);
        case JCE_GIZMO_AXIS_Y:  return ImGui::GetColorU32(JCE_COLOR_GIZMO_Y);
        case JCE_GIZMO_AXIS_Z:  return ImGui::GetColorU32(JCE_COLOR_GIZMO_Z);
        default:                 return ImGui::GetColorU32(ImVec4(0.7f,0.7f,0.7f,1.0f));
    }
}

static ImU32 plane_color(JceGizmoAxis axis, JceGizmoAxis hovered, JceGizmoAxis dragging)
{
    if (dragging == axis)
        return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 1.0f, 0.35f));
    if (hovered == axis)
        return ImGui::GetColorU32(ImVec4(1.0f, 1.0f, 0.4f, 0.30f));

    /* color by the two axes involved */
    return ImGui::GetColorU32(ImVec4(0.5f, 0.5f, 0.5f, 0.15f));
}

/* ── Axis direction vectors ────────────────────────────────────────── */

static const float s_axis_x[3] = {1,0,0};
static const float s_axis_y[3] = {0,1,0};
static const float s_axis_z[3] = {0,0,1};

/* Compute screen-space axis length that stays visually constant. */
static float screen_axis_length(const JceGizmoCamera *cam,
                                const float origin[3],
                                float scale_factor)
{
    /* Distance from camera to gizmo origin. */
    float d[3];
    gm_v3_sub(d, origin, cam->eye);
    float dist = gm_v3_len(d);
    if (dist < 0.01f) dist = 0.01f;

    /* Use a heuristic: project a unit offset and measure screen px. */
    float base_px = 120.0f;  /* target screen-space length in pixels */
    return base_px * scale_factor;
}

/* ── Translate Gizmo ───────────────────────────────────────────────── */

static void draw_translate_arrow(ImDrawList *dl,
                                  const JceGizmoCamera *cam,
                                  const float origin[3],
                                  const float axis_dir[3],
                                  float px_len,
                                  JceGizmoAxis axis_id,
                                  JceGizmoAxis hovered,
                                  JceGizmoAxis dragging)
{
    float scr_o[2];
    if (!gm_world_to_screen(cam, origin, scr_o)) return;

    /* We need to find a world length that maps to px_len on screen.
       Use distance-based approximation. */
    float d[3];
    gm_v3_sub(d, origin, cam->eye);
    float dist = gm_v3_len(d);
    if (dist < 0.01f) dist = 0.01f;

    /* Approximate world length for the desired screen length */
    float world_len = JCE_GIZMO_AXIS_LENGTH * dist * 0.07f;
    /* Apply scale factor from preferences */
    world_len *= (px_len / 120.0f);

    float tip[3];
    gm_v3_scale(tip, axis_dir, world_len);
    gm_v3_add(tip, origin, tip);

    float scr_tip[2];
    if (!gm_world_to_screen(cam, tip, scr_tip)) return;

    ImU32 col = axis_color(axis_id, hovered, dragging);
    float thickness = JCE_GIZMO_LINE_THICKNESS;

    /* Axis line */
    dl->AddLine(ImVec2(scr_o[0], scr_o[1]),
                ImVec2(scr_tip[0], scr_tip[1]),
                col, thickness);

    /* Arrow head (triangle) */
    float dx = scr_tip[0] - scr_o[0];
    float dy = scr_tip[1] - scr_o[1];
    float len = sqrtf(dx*dx + dy*dy);
    if (len < 1.0f) return;

    float nx = dx / len, ny = dy / len;
    float px = -ny, py = nx;  /* perpendicular */

    float arrow_size = 10.0f;
    ImVec2 a(scr_tip[0], scr_tip[1]);
    ImVec2 b(scr_tip[0] - nx*arrow_size + px*arrow_size*0.4f,
             scr_tip[1] - ny*arrow_size + py*arrow_size*0.4f);
    ImVec2 c2(scr_tip[0] - nx*arrow_size - px*arrow_size*0.4f,
              scr_tip[1] - ny*arrow_size - py*arrow_size*0.4f);
    dl->AddTriangleFilled(a, b, c2, col);
}

static void draw_translate_plane_handle(ImDrawList *dl,
                                         const JceGizmoCamera *cam,
                                         const float origin[3],
                                         const float a1[3],
                                         const float a2[3],
                                         float world_len,
                                         JceGizmoAxis axis_id,
                                         JceGizmoAxis hovered,
                                         JceGizmoAxis dragging)
{
    float offset = world_len * 0.25f;
    float size   = world_len * 0.15f;

    /* Four corners of the plane handle quad */
    float p0[3], p1[3], p2[3], p3[3];
    float t1[3], t2[3];
    gm_v3_scale(t1, a1, offset);
    gm_v3_scale(t2, a2, offset);

    gm_v3_add(p0, origin, t1);
    gm_v3_add(p0, p0, t2);

    float s1[3], s2[3];
    gm_v3_scale(s1, a1, size);
    gm_v3_scale(s2, a2, size);

    gm_v3_add(p1, p0, s1);
    gm_v3_add(p3, p0, s2);
    gm_v3_add(p2, p1, s2);

    float sp0[2], sp1[2], sp2[2], sp3[2];
    if (!gm_world_to_screen(cam, p0, sp0)) return;
    if (!gm_world_to_screen(cam, p1, sp1)) return;
    if (!gm_world_to_screen(cam, p2, sp2)) return;
    if (!gm_world_to_screen(cam, p3, sp3)) return;

    ImU32 col = plane_color(axis_id, hovered, dragging);
    dl->AddQuadFilled(ImVec2(sp0[0],sp0[1]), ImVec2(sp1[0],sp1[1]),
                      ImVec2(sp2[0],sp2[1]), ImVec2(sp3[0],sp3[1]), col);
}

void jce_gizmo_draw_translate(ImDrawList *dl,
                               const JceGizmoCamera *cam,
                               float scale_factor,
                               const float *position)
{
    JceGizmoAxis hovered  = jce_gizmo_internal_hovered();
    JceGizmoAxis dragging = jce_gizmo_internal_dragging()
                          ? jce_gizmo_internal_drag_axis()
                          : JCE_GIZMO_AXIS_NONE;

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);

    float px_len = screen_axis_length(cam, position, scale_factor);

    draw_translate_arrow(dl, cam, position, ax_x, px_len,
                         JCE_GIZMO_AXIS_X, hovered, dragging);
    draw_translate_arrow(dl, cam, position, ax_y, px_len,
                         JCE_GIZMO_AXIS_Y, hovered, dragging);
    draw_translate_arrow(dl, cam, position, ax_z, px_len,
                         JCE_GIZMO_AXIS_Z, hovered, dragging);

    /* Plane handles */
    float d[3];
    gm_v3_sub(d, position, cam->eye);
    float dist = gm_v3_len(d);
    if (dist < 0.01f) dist = 0.01f;
    float world_len = JCE_GIZMO_AXIS_LENGTH * dist * 0.07f * (px_len / 120.0f);

    draw_translate_plane_handle(dl, cam, position, ax_x, ax_y,
                                world_len, JCE_GIZMO_AXIS_XY, hovered, dragging);
    draw_translate_plane_handle(dl, cam, position, ax_x, ax_z,
                                world_len, JCE_GIZMO_AXIS_XZ, hovered, dragging);
    draw_translate_plane_handle(dl, cam, position, ax_y, ax_z,
                                world_len, JCE_GIZMO_AXIS_YZ, hovered, dragging);

    /* Center dot */
    float scr_o[2];
    if (gm_world_to_screen(cam, position, scr_o)) {
        ImU32 center_col = (hovered == JCE_GIZMO_AXIS_XYZ || dragging == JCE_GIZMO_AXIS_XYZ)
                         ? ImGui::GetColorU32(JCE_COLOR_GIZMO_HOVERED)
                         : ImGui::GetColorU32(ImVec4(0.9f,0.9f,0.9f,1.0f));
        dl->AddCircleFilled(ImVec2(scr_o[0], scr_o[1]), 5.0f, center_col);
    }
}

/* ── Rotate Gizmo ──────────────────────────────────────────────────── */

static void draw_rotation_ring(ImDrawList *dl,
                                const JceGizmoCamera *cam,
                                const float origin[3],
                                const float normal[3],
                                const float tangent[3],
                                const float bitangent[3],
                                float world_radius,
                                JceGizmoAxis axis_id,
                                JceGizmoAxis hovered,
                                JceGizmoAxis dragging)
{
    ImU32 col = axis_color(axis_id, hovered, dragging);
    float thickness = JCE_GIZMO_LINE_THICKNESS;

    const int segments = 64;
    float prev[2];
    bool prev_ok = false;

    for (int i = 0; i <= segments; i++) {
        float angle = (float)i / (float)segments * 2.0f * JCE_PI;
        float cs = cosf(angle), sn = sinf(angle);

        float p[3];
        p[0] = origin[0] + (tangent[0]*cs + bitangent[0]*sn) * world_radius;
        p[1] = origin[1] + (tangent[1]*cs + bitangent[1]*sn) * world_radius;
        p[2] = origin[2] + (tangent[2]*cs + bitangent[2]*sn) * world_radius;

        float scr[2];
        bool ok = gm_world_to_screen(cam, p, scr);

        if (ok && prev_ok) {
            /* Check if this segment faces the camera (dot of segment-center
               to camera vs normal). If behind, draw faded. */
            float mid[3] = {
                origin[0] + (tangent[0]*cosf(angle-0.05f) + bitangent[0]*sinf(angle-0.05f)) * world_radius,
                origin[1] + (tangent[1]*cosf(angle-0.05f) + bitangent[1]*sinf(angle-0.05f)) * world_radius,
                origin[2] + (tangent[2]*cosf(angle-0.05f) + bitangent[2]*sinf(angle-0.05f)) * world_radius
            };
            float to_eye[3];
            gm_v3_sub(to_eye, cam->eye, mid);
            float facing = gm_v3_dot(to_eye, normal);
            float alpha = (facing > 0.0f) ? 1.0f : 0.2f;

            ImU32 seg_col = (col & 0x00FFFFFF) | ((uint32_t)(((col >> 24) & 0xFF) * alpha) << 24);
            dl->AddLine(ImVec2(prev[0], prev[1]), ImVec2(scr[0], scr[1]),
                        seg_col, thickness);
        }

        prev[0] = scr[0]; prev[1] = scr[1];
        prev_ok = ok;
    }
}

void jce_gizmo_draw_rotate(ImDrawList *dl,
                             const JceGizmoCamera *cam,
                             float scale_factor,
                             const float *position)
{
    JceGizmoAxis hovered  = jce_gizmo_internal_hovered();
    JceGizmoAxis dragging = jce_gizmo_internal_dragging()
                          ? jce_gizmo_internal_drag_axis()
                          : JCE_GIZMO_AXIS_NONE;

    float d[3];
    gm_v3_sub(d, position, cam->eye);
    float dist = gm_v3_len(d);
    if (dist < 0.01f) dist = 0.01f;
    float world_radius = JCE_GIZMO_AXIS_LENGTH * dist * 0.07f * scale_factor;

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);

    /* X ring: normal=X, tangent=Y, bitangent=Z */
    draw_rotation_ring(dl, cam, position, ax_x, ax_y, ax_z, world_radius,
                       JCE_GIZMO_AXIS_X, hovered, dragging);

    /* Y ring: normal=Y, tangent=Z, bitangent=X */
    draw_rotation_ring(dl, cam, position, ax_y, ax_z, ax_x, world_radius,
                       JCE_GIZMO_AXIS_Y, hovered, dragging);

    /* Z ring: normal=Z, tangent=X, bitangent=Y */
    draw_rotation_ring(dl, cam, position, ax_z, ax_x, ax_y, world_radius,
                       JCE_GIZMO_AXIS_Z, hovered, dragging);
}

/* ── Scale Gizmo ───────────────────────────────────────────────────── */

static void draw_scale_axis(ImDrawList *dl,
                             const JceGizmoCamera *cam,
                             const float origin[3],
                             const float axis_dir[3],
                             float px_len,
                             JceGizmoAxis axis_id,
                             JceGizmoAxis hovered,
                             JceGizmoAxis dragging)
{
    float scr_o[2];
    if (!gm_world_to_screen(cam, origin, scr_o)) return;

    float d[3];
    gm_v3_sub(d, origin, cam->eye);
    float dist = gm_v3_len(d);
    if (dist < 0.01f) dist = 0.01f;
    float world_len = JCE_GIZMO_AXIS_LENGTH * dist * 0.07f * (px_len / 120.0f);

    float tip[3];
    gm_v3_scale(tip, axis_dir, world_len);
    gm_v3_add(tip, origin, tip);

    float scr_tip[2];
    if (!gm_world_to_screen(cam, tip, scr_tip)) return;

    ImU32 col = axis_color(axis_id, hovered, dragging);
    float thickness = JCE_GIZMO_LINE_THICKNESS;

    /* Axis line */
    dl->AddLine(ImVec2(scr_o[0], scr_o[1]),
                ImVec2(scr_tip[0], scr_tip[1]),
                col, thickness);

    /* End cube (draw as filled rect) */
    float cube_sz = 4.0f;
    dl->AddRectFilled(ImVec2(scr_tip[0]-cube_sz, scr_tip[1]-cube_sz),
                      ImVec2(scr_tip[0]+cube_sz, scr_tip[1]+cube_sz),
                      col);
}

void jce_gizmo_draw_scale(ImDrawList *dl,
                            const JceGizmoCamera *cam,
                            float scale_factor,
                            const float *position)
{
    JceGizmoAxis hovered  = jce_gizmo_internal_hovered();
    JceGizmoAxis dragging = jce_gizmo_internal_dragging()
                          ? jce_gizmo_internal_drag_axis()
                          : JCE_GIZMO_AXIS_NONE;

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);

    float px_len = screen_axis_length(cam, position, scale_factor);

    draw_scale_axis(dl, cam, position, ax_x, px_len,
                    JCE_GIZMO_AXIS_X, hovered, dragging);
    draw_scale_axis(dl, cam, position, ax_y, px_len,
                    JCE_GIZMO_AXIS_Y, hovered, dragging);
    draw_scale_axis(dl, cam, position, ax_z, px_len,
                    JCE_GIZMO_AXIS_Z, hovered, dragging);

    /* Center cube */
    float scr_o[2];
    if (gm_world_to_screen(cam, position, scr_o)) {
        ImU32 center_col = (hovered == JCE_GIZMO_AXIS_XYZ || dragging == JCE_GIZMO_AXIS_XYZ)
                         ? ImGui::GetColorU32(JCE_COLOR_GIZMO_HOVERED)
                         : ImGui::GetColorU32(ImVec4(0.9f,0.9f,0.9f,1.0f));
        dl->AddRectFilled(ImVec2(scr_o[0]-5, scr_o[1]-5),
                          ImVec2(scr_o[0]+5, scr_o[1]+5), center_col);
    }
}
