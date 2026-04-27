/*
 * jce_gizmo_interact.cpp  Hit-testing & drag logic for transform gizmos.
 *
 * Determines which axis the mouse hovers, and computes transform deltas
 * when dragging.  All logic works in world-space via ray unprojection.
 */

#include "core/jce_editor_defaults.h"
#include "jce_gizmo.h"
#include "jce_gizmo_internal.h"

#include <jce/tools/jce_imgui.h>
#include <math.h>

/* ── Constants ─────────────────────────────────────────────────────── */

static const float s_axis_dirs[3][3] = {
    {1,0,0}, {0,1,0}, {0,0,1}
};
static const JceGizmoAxis s_axis_ids[3] = {
    JCE_GIZMO_AXIS_X, JCE_GIZMO_AXIS_Y, JCE_GIZMO_AXIS_Z
};

/* Plane normals for XY/XZ/YZ plane handles */
static const float s_plane_normals[3][3] = {
    {0,0,1}, /* XY plane: normal = Z */
    {0,1,0}, /* XZ plane: normal = Y */
    {1,0,0}, /* YZ plane: normal = X */
};
static const JceGizmoAxis s_plane_ids[3] = {
    JCE_GIZMO_AXIS_XY, JCE_GIZMO_AXIS_XZ, JCE_GIZMO_AXIS_YZ
};

/* ── Helper: compute world-space axis length for a gizmo ───────────── */

static float compute_world_axis_len(const JceGizmoCamera *cam,
                                     const float origin[3],
                                     float scale_factor)
{
    float d[3];
    gm_v3_sub(d, origin, cam->eye);
    float dist = gm_v3_len(d);
    if (dist < 0.01f) dist = 0.01f;
    return JCE_GIZMO_AXIS_LENGTH * dist * 0.07f * scale_factor;
}

/* ── Hit test: single axis arrow (translate / scale) ───────────────── */

static float hit_test_axis(const JceGizmoCamera *cam,
                            const float origin[3],
                            const float axis_dir[3],
                            float world_len,
                            float mouse_x, float mouse_y)
{
    /* Screen project origin and tip */
    float scr_o[2], scr_tip[2];
    if (!gm_world_to_screen(cam, origin, scr_o)) return 1e9f;

    float tip[3];
    gm_v3_scale(tip, axis_dir, world_len);
    gm_v3_add(tip, origin, tip);
    if (!gm_world_to_screen(cam, tip, scr_tip)) return 1e9f;

    return gm_point_segment_dist_2d(mouse_x, mouse_y,
                                     scr_o[0], scr_o[1],
                                     scr_tip[0], scr_tip[1]);
}

/* ── Hit test: plane handle (translate) ────────────────────────────── */

static float hit_test_plane(const JceGizmoCamera *cam,
                             const float origin[3],
                             const float a1[3],
                             const float a2[3],
                             float world_len,
                             float mouse_x, float mouse_y)
{
    float offset = world_len * 0.25f;
    float size   = world_len * 0.15f;

    float t1[3], t2[3], s1[3], s2[3];
    gm_v3_scale(t1, a1, offset);
    gm_v3_scale(t2, a2, offset);

    float p0[3];
    gm_v3_add(p0, origin, t1);
    gm_v3_add(p0, p0, t2);

    gm_v3_scale(s1, a1, size);
    gm_v3_scale(s2, a2, size);

    float p1[3], p2[3], p3[3];
    gm_v3_add(p1, p0, s1);
    gm_v3_add(p3, p0, s2);
    gm_v3_add(p2, p1, s2);

    /* Project all 4 corners */
    float sp0[2], sp1[2], sp2[2], sp3[2];
    if (!gm_world_to_screen(cam, p0, sp0)) return 1e9f;
    if (!gm_world_to_screen(cam, p1, sp1)) return 1e9f;
    if (!gm_world_to_screen(cam, p2, sp2)) return 1e9f;
    if (!gm_world_to_screen(cam, p3, sp3)) return 1e9f;

    /* Simple AABB check on the projected quad */
    float min_x = sp0[0], max_x = sp0[0];
    float min_y = sp0[1], max_y = sp0[1];
    float pts[4][2] = {{sp0[0],sp0[1]},{sp1[0],sp1[1]},{sp2[0],sp2[1]},{sp3[0],sp3[1]}};
    for (int i = 1; i < 4; i++) {
        if (pts[i][0] < min_x) min_x = pts[i][0];
        if (pts[i][0] > max_x) max_x = pts[i][0];
        if (pts[i][1] < min_y) min_y = pts[i][1];
        if (pts[i][1] > max_y) max_y = pts[i][1];
    }

    if (mouse_x >= min_x && mouse_x <= max_x &&
        mouse_y >= min_y && mouse_y <= max_y) {
        return 0.0f;  /* inside the quad */
    }
    return 1e9f;
}

/* ── Hit test: rotation ring ───────────────────────────────────────── */

static float hit_test_ring(const JceGizmoCamera *cam,
                            const float origin[3],
                            const float tangent[3],
                            const float bitangent[3],
                            float world_radius,
                            float mouse_x, float mouse_y)
{
    float best_dist = 1e9f;
    const int segments = 32;

    for (int i = 0; i < segments; i++) {
        float angle = (float)i / (float)segments * 2.0f * JCE_PI;
        float cs = cosf(angle), sn = sinf(angle);

        float p[3];
        p[0] = origin[0] + (tangent[0]*cs + bitangent[0]*sn) * world_radius;
        p[1] = origin[1] + (tangent[1]*cs + bitangent[1]*sn) * world_radius;
        p[2] = origin[2] + (tangent[2]*cs + bitangent[2]*sn) * world_radius;

        float scr[2];
        if (!gm_world_to_screen(cam, p, scr)) continue;

        float d = gm_dist_2d(mouse_x, mouse_y, scr[0], scr[1]);
        if (d < best_dist) best_dist = d;
    }
    return best_dist;
}

/* ── Hit test: center dot / cube ───────────────────────────────────── */

static float hit_test_center(const JceGizmoCamera *cam,
                              const float origin[3],
                              float mouse_x, float mouse_y)
{
    float scr[2];
    if (!gm_world_to_screen(cam, origin, scr)) return 1e9f;
    return gm_dist_2d(mouse_x, mouse_y, scr[0], scr[1]);
}

/* ── Public: translate hit test ────────────────────────────────────── */

JceGizmoAxis jce_gizmo_hit_test_translate(const JceGizmoCamera *cam,
                                           const float *position,
                                           float scale_factor,
                                           float mouse_x, float mouse_y)
{
    float threshold = JCE_GIZMO_SELECT_THRESHOLD;
    float world_len = compute_world_axis_len(cam, position, scale_factor);

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);
    const float *axes[3] = { ax_x, ax_y, ax_z };

    /* Center dot first (highest priority, enlarged hit area) */
    float center_d = hit_test_center(cam, position, mouse_x, mouse_y);
    if (center_d < 12.0f) return JCE_GIZMO_AXIS_XYZ;

    /* Plane handles (priority over single axes) */
    const float *plane_axes[3][2] = {
        { ax_x, ax_y }, /* XY */
        { ax_x, ax_z }, /* XZ */
        { ax_y, ax_z }, /* YZ */
    };
    for (int i = 0; i < 3; i++) {
        float d = hit_test_plane(cam, position, plane_axes[i][0], plane_axes[i][1],
                                  world_len, mouse_x, mouse_y);
        if (d < threshold) return s_plane_ids[i];
    }

    /* Single axes */
    JceGizmoAxis best_axis = JCE_GIZMO_AXIS_NONE;
    float best_dist = threshold;
    for (int i = 0; i < 3; i++) {
        float d = hit_test_axis(cam, position, axes[i], world_len,
                                 mouse_x, mouse_y);
        if (d < best_dist) {
            best_dist = d;
            best_axis = s_axis_ids[i];
        }
    }
    return best_axis;
}

/* ── Public: rotate hit test ───────────────────────────────────────── */

JceGizmoAxis jce_gizmo_hit_test_rotate(const JceGizmoCamera *cam,
                                        const float *position,
                                        float scale_factor,
                                        float mouse_x, float mouse_y)
{
    /* Rotation rings need a larger hover envelope for stable picking. */
    float threshold = JCE_GIZMO_SELECT_THRESHOLD * 1.4f;
    float world_radius = compute_world_axis_len(cam, position, scale_factor);

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);

    /* Ring tangent/bitangent using dynamic axes */
    const float *rings[3][2] = {
        { ax_y, ax_z }, /* X ring: tangent=Y, bitangent=Z */
        { ax_z, ax_x }, /* Y ring: tangent=Z, bitangent=X */
        { ax_x, ax_y }, /* Z ring: tangent=X, bitangent=Y */
    };

    JceGizmoAxis best_axis = JCE_GIZMO_AXIS_NONE;
    float best_dist = threshold;

    for (int i = 0; i < 3; i++) {
        float d = hit_test_ring(cam, position, rings[i][0], rings[i][1],
                                 world_radius, mouse_x, mouse_y);
        if (d < best_dist) {
            best_dist = d;
            best_axis = s_axis_ids[i];
        }
    }
    return best_axis;
}

/* ── Public: scale hit test ────────────────────────────────────────── */

JceGizmoAxis jce_gizmo_hit_test_scale(const JceGizmoCamera *cam,
                                       const float *position,
                                       float scale_factor,
                                       float mouse_x, float mouse_y)
{
    float threshold = JCE_GIZMO_SELECT_THRESHOLD;
    float world_len = compute_world_axis_len(cam, position, scale_factor);

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);
    const float *axes[3] = { ax_x, ax_y, ax_z };

    /* Center cube first (enlarged hit area) */
    float center_d = hit_test_center(cam, position, mouse_x, mouse_y);
    if (center_d < 12.0f) return JCE_GIZMO_AXIS_XYZ;

    /* Single axes */
    JceGizmoAxis best_axis = JCE_GIZMO_AXIS_NONE;
    float best_dist = threshold;
    for (int i = 0; i < 3; i++) {
        float d = hit_test_axis(cam, position, axes[i], world_len,
                                 mouse_x, mouse_y);
        if (d < best_dist) {
            best_dist = d;
            best_axis = s_axis_ids[i];
        }
    }
    return best_axis;
}

/* ── Drag helpers ──────────────────────────────────────────────────── */

static void get_drag_plane_normal(JceGizmoAxis axis, const float cam_dir[3],
                                   float out_normal[3])
{
    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);

    /* For plane constraints, use the rotated third axis as normal. */
    switch (axis) {
        case JCE_GIZMO_AXIS_XY:
            gm_v3_copy(out_normal, ax_z);
            return;
        case JCE_GIZMO_AXIS_XZ:
            gm_v3_copy(out_normal, ax_y);
            return;
        case JCE_GIZMO_AXIS_YZ:
            gm_v3_copy(out_normal, ax_x);
            return;
        case JCE_GIZMO_AXIS_XYZ:
        case JCE_GIZMO_AXIS_VIEW: {
            /* Use camera forward as plane normal */
            float neg[3] = {-cam_dir[0], -cam_dir[1], -cam_dir[2]};
            gm_v3_normalize(out_normal, neg);
            return;
        }
        default: break;
    }

    /* Single axis: use the rotated axis direction to find best plane. */
    float ax[3] = {0,0,0};
    if (axis & JCE_GIZMO_AXIS_X) gm_v3_copy(ax, ax_x);
    else if (axis & JCE_GIZMO_AXIS_Y) gm_v3_copy(ax, ax_y);
    else if (axis & JCE_GIZMO_AXIS_Z) gm_v3_copy(ax, ax_z);

    /* Cross with camera direction to get a perpendicular, then cross
       again with axis to get the plane normal. */
    float perp[3];
    gm_v3_cross(perp, ax, cam_dir);
    float len = gm_v3_len(perp);
    if (len < 1e-6f) {
        float up[3] = {0,1,0};
        if (fabsf(gm_v3_dot(ax, up)) > 0.99f) {
            up[0] = 1; up[1] = 0; up[2] = 0;
        }
        gm_v3_cross(perp, ax, up);
    }
    gm_v3_cross(out_normal, ax, perp);
    gm_v3_normalize(out_normal, out_normal);
}

/* ── Public: drag translate ────────────────────────────────────────── */

void jce_gizmo_drag_translate(const JceGizmoCamera *cam,
                               JceGizmoAxis axis,
                               const float origin[3],
                               float mouse_x, float mouse_y,
                               float prev_mouse_x, float prev_mouse_y,
                               float out_delta[3])
{
    out_delta[0] = out_delta[1] = out_delta[2] = 0.0f;

    /* Get view forward direction from view matrix */
    float cam_dir[3] = {cam->view[2], cam->view[6], cam->view[10]};

    float plane_n[3];
    get_drag_plane_normal(axis, cam_dir, plane_n);

    /* Intersect current and previous mouse ray with the drag plane */
    float ro_cur[3], rd_cur[3], ro_prev[3], rd_prev[3];
    gm_screen_to_ray(cam, mouse_x, mouse_y, ro_cur, rd_cur);
    gm_screen_to_ray(cam, prev_mouse_x, prev_mouse_y, ro_prev, rd_prev);

    float hit_cur[3], hit_prev[3];
    if (!gm_ray_plane_intersect(ro_cur, rd_cur, plane_n, origin, hit_cur)) return;
    if (!gm_ray_plane_intersect(ro_prev, rd_prev, plane_n, origin, hit_prev)) return;

    float delta[3];
    gm_v3_sub(delta, hit_cur, hit_prev);

    /* Project delta onto constrained axis directions (supports both
       world and local space via the dynamic axes). */
    float lx[3], ly[3], lz[3];
    jce_gizmo_internal_get_axes(lx, ly, lz);

    if (axis == JCE_GIZMO_AXIS_X) {
        float d = gm_v3_dot(delta, lx);
        gm_v3_scale(delta, lx, d);
    } else if (axis == JCE_GIZMO_AXIS_Y) {
        float d = gm_v3_dot(delta, ly);
        gm_v3_scale(delta, ly, d);
    } else if (axis == JCE_GIZMO_AXIS_Z) {
        float d = gm_v3_dot(delta, lz);
        gm_v3_scale(delta, lz, d);
    } else if (axis == JCE_GIZMO_AXIS_XY) {
        /* Remove Z component in local space */
        float dz = gm_v3_dot(delta, lz);
        float sub[3]; gm_v3_scale(sub, lz, dz);
        gm_v3_sub(delta, delta, sub);
    } else if (axis == JCE_GIZMO_AXIS_XZ) {
        float dy = gm_v3_dot(delta, ly);
        float sub[3]; gm_v3_scale(sub, ly, dy);
        gm_v3_sub(delta, delta, sub);
    } else if (axis == JCE_GIZMO_AXIS_YZ) {
        float dx = gm_v3_dot(delta, lx);
        float sub[3]; gm_v3_scale(sub, lx, dx);
        gm_v3_sub(delta, delta, sub);
    }

    gm_v3_copy(out_delta, delta);
}

/* ── Public: drag rotate ───────────────────────────────────────────── */

void jce_gizmo_drag_rotate(const JceGizmoCamera *cam,
                             JceGizmoAxis axis,
                             const float origin[3],
                             float mouse_x, float mouse_y,
                             float prev_mouse_x, float prev_mouse_y,
                             float out_delta_euler[3])
{
    out_delta_euler[0] = out_delta_euler[1] = out_delta_euler[2] = 0.0f;

    float ax_x[3], ax_y[3], ax_z[3];
    jce_gizmo_internal_get_axes(ax_x, ax_y, ax_z);

    float plane_n[3] = {0.0f, 0.0f, 1.0f};
    if (axis & JCE_GIZMO_AXIS_X) {
        gm_v3_copy(plane_n, ax_x);
    } else if (axis & JCE_GIZMO_AXIS_Y) {
        gm_v3_copy(plane_n, ax_y);
    } else if (axis & JCE_GIZMO_AXIS_Z) {
        gm_v3_copy(plane_n, ax_z);
    }
    gm_v3_normalize(plane_n, plane_n);

    float deg = 0.0f;

    /* Blender-like ring drag: signed angle on the selected ring plane. */
    {
        float ro_cur[3], rd_cur[3], ro_prev[3], rd_prev[3];
        gm_screen_to_ray(cam, mouse_x, mouse_y, ro_cur, rd_cur);
        gm_screen_to_ray(cam, prev_mouse_x, prev_mouse_y, ro_prev, rd_prev);

        float hit_cur[3], hit_prev[3];
        bool ok_cur = gm_ray_plane_intersect(ro_cur, rd_cur, plane_n, origin, hit_cur);
        bool ok_prev = gm_ray_plane_intersect(ro_prev, rd_prev, plane_n, origin, hit_prev);

        if (ok_cur && ok_prev) {
            float v_cur[3], v_prev[3];
            gm_v3_sub(v_cur, hit_cur, origin);
            gm_v3_sub(v_prev, hit_prev, origin);

            float len_cur = gm_v3_len(v_cur);
            float len_prev = gm_v3_len(v_prev);
            if (len_cur > 1e-6f && len_prev > 1e-6f) {
                gm_v3_scale(v_cur, v_cur, 1.0f / len_cur);
                gm_v3_scale(v_prev, v_prev, 1.0f / len_prev);

                float c[3];
                gm_v3_cross(c, v_prev, v_cur);
                float sin_term = gm_v3_dot(plane_n, c);
                float cos_term = gm_v3_dot(v_prev, v_cur);
                if (cos_term > 1.0f) cos_term = 1.0f;
                if (cos_term < -1.0f) cos_term = -1.0f;
                deg = atan2f(sin_term, cos_term) * JCE_RAD2DEG;
            }
        }
    }

    if (fabsf(deg) < 1e-6f) {
        /* Fallback when ring plane intersection is degenerate. */
        float scr_o[2];
        if (!gm_world_to_screen(cam, origin, scr_o)) return;
        float angle_cur  = atan2f(mouse_y - scr_o[1], mouse_x - scr_o[0]);
        float angle_prev = atan2f(prev_mouse_y - scr_o[1], prev_mouse_x - scr_o[0]);
        float da = angle_cur - angle_prev;
        if (da >  JCE_PI) da -= 2.0f * JCE_PI;
        if (da < -JCE_PI) da += 2.0f * JCE_PI;
        deg = da * JCE_RAD2DEG;
    }

    /* Shift = precision rotate, similar to Blender fine control. */
    if (ImGui::GetIO().KeyShift)
        deg *= 0.2f;

    /* Reject occasional spikes when camera/plane is near-singular. */
    if (deg > 45.0f) deg = 45.0f;
    if (deg < -45.0f) deg = -45.0f;

    if (axis & JCE_GIZMO_AXIS_X) out_delta_euler[0] = deg;
    if (axis & JCE_GIZMO_AXIS_Y) out_delta_euler[1] = deg;
    if (axis & JCE_GIZMO_AXIS_Z) out_delta_euler[2] = deg;
}

/* ── Public: drag scale ────────────────────────────────────────────── */

void jce_gizmo_drag_scale(const JceGizmoCamera *cam,
                            JceGizmoAxis axis,
                            const float origin[3],
                            float mouse_x, float mouse_y,
                            float prev_mouse_x, float prev_mouse_y,
                            float out_delta_scale[3])
{
    out_delta_scale[0] = out_delta_scale[1] = out_delta_scale[2] = 0.0f;

    /* Project origin to screen */
    float scr_o[2];
    if (!gm_world_to_screen(cam, origin, scr_o)) return;

    /* Scale by distance change from origin */
    float dist_cur  = gm_dist_2d(mouse_x, mouse_y, scr_o[0], scr_o[1]);
    float dist_prev = gm_dist_2d(prev_mouse_x, prev_mouse_y, scr_o[0], scr_o[1]);

    if (dist_prev < 1.0f) dist_prev = 1.0f;

    float ratio = (dist_cur - dist_prev) * 0.01f;

    if (axis == JCE_GIZMO_AXIS_XYZ || axis == JCE_GIZMO_AXIS_VIEW) {
        out_delta_scale[0] = out_delta_scale[1] = out_delta_scale[2] = ratio;
    } else {
        if (axis & JCE_GIZMO_AXIS_X) out_delta_scale[0] = ratio;
        if (axis & JCE_GIZMO_AXIS_Y) out_delta_scale[1] = ratio;
        if (axis & JCE_GIZMO_AXIS_Z) out_delta_scale[2] = ratio;
    }
}
