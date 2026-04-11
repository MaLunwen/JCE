/*
 * jce_panel_scene_view.cpp  Scene View panel (3D viewport + gizmo toolbar).
 *
 * Displays the 3D scene rendered to an off-screen FBO as an ImGui::Image().
 * Overlays: background fill, gizmo, selection box, status text.
 *
 * Reference: SceneViewWindow.java
 */

#include "jce_editor_panels.h"
#include "jce_editor_state.h"
#include "jce_editor_colors.h"
#include "jce_editor_i18n.h"
#include "jce_editor_scene_render.h"
#include "gizmo/jce_gizmo.h"

#include <imgui.h>
#include <imgui_internal.h>
#include <bgfx/c99/bgfx.h>
#include <stdio.h>
#include <math.h>
#include <string.h>
#include <stdlib.h>

extern "C" {
#include <jce/core/jce_math.h>
}

/* ── Selection box state ──────────────────────────────────────────── */

static bool  s_is_selecting   = false;
static ImVec2 s_sel_start     = ImVec2(0, 0);
static ImVec2 s_sel_current   = ImVec2(0, 0);
static bool   s_sel_easter    = false;   /* chance alternate colors */
static ImU32  s_sel_border    = 0;
static ImU32  s_sel_fill      = 0;
static ImU32  s_sel_inner     = 0;

/* Pending marquee hit-test (applied when camera matrices are available). */
static bool   s_sel_pending   = false;
static ImVec2 s_sel_rect_min  = ImVec2(0, 0);
static ImVec2 s_sel_rect_max  = ImVec2(0, 0);

/* Pending single-click ray pick (applied in camera matrices block). */
static bool   s_sel_click_pending = false;
static ImVec2 s_sel_click_pos     = ImVec2(0, 0);

/* Raw gizmo drag values (unsnapped) to keep Ctrl snapping smooth.
 * We snap only the applied output, but preserve raw drag progression. */
static bool  s_gizmo_raw_dragging = false;
static float s_gizmo_raw_pos[3]   = {0.0f, 0.0f, 0.0f};
static float s_gizmo_raw_rot[3]   = {0.0f, 0.0f, 0.0f};
static float s_gizmo_raw_scale[3] = {1.0f, 1.0f, 1.0f};
static bool  s_gizmo_history_batch_open = false;

static JceComponentInfo *find_transform_component(JceComponentInfo *comps,
                                                  int comp_count);

static JceComponentInfo *find_component_by_type(JceComponentInfo *comps,
                                                int comp_count,
                                                JceComponentType type);

static void draw_camera_scene_icon(ImDrawList *dl, ImVec2 center,
                                   float radius, bool selected);

static void draw_light_scene_icon(ImDrawList *dl, ImVec2 center,
                                  float radius, int light_type,
                                  bool selected);

static void build_helper_basis(const JceComponentInfo *xform,
                               float forward[3],
                               float right[3],
                               float up[3]);

static bool project_helper_point(const JceGizmoCamera *cam,
                                 const float world[3],
                                 ImVec2 *out);

static void draw_camera_helper_lines(ImDrawList *dl,
                                     const JceGizmoCamera *cam,
                                     const JceComponentInfo *xform,
                                     const JceComponentInfo *camera,
                                     bool selected);

static void draw_light_helper_lines(ImDrawList *dl,
                                    const JceGizmoCamera *cam,
                                    const JceComponentInfo *xform,
                                    const JceComponentInfo *light,
                                    bool selected);

static void draw_scene_helper_icons(ImDrawList *dl,
                                    const JceGizmoCamera *cam);

static void set_entity_mesh_shape(uint32_t entity_id, int mesh_shape);

static uint32_t create_default_scene_entity(const char *name,
                                            uint32_t parent_id,
                                            JceComponentType extra_type,
                                            int mesh_shape);

static void clear_stale_gizmo_interaction_state(void)
{
    if (jce_gizmo_is_active() || jce_gizmo_hovered_axis() != JCE_GIZMO_AXIS_NONE)
        jce_gizmo_cancel_interaction();

    if (s_gizmo_history_batch_open) {
        jce_state_end_batch_edit();
        s_gizmo_history_batch_open = false;
    }

    s_gizmo_raw_dragging = false;
}

static bool has_valid_gizmo_target(void)
{
    if (!jce_editor_prefs_show_gizmos())
        return false;

    uint32_t focused = jce_state_get_focused();
    if (focused == 0)
        return false;

    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(focused, &comp_count);
    return find_transform_component(comps, comp_count) != NULL;
}

static JceComponentInfo *find_transform_component(JceComponentInfo *comps, int comp_count)
{
    if (!comps || comp_count <= 0) return NULL;
    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == JCE_COMP_TRANSFORM)
            return &comps[i];
    }
    return NULL;
}

static JceComponentInfo *find_component_by_type(JceComponentInfo *comps,
                                                int comp_count,
                                                JceComponentType type)
{
    if (!comps || comp_count <= 0) return NULL;
    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == type)
            return &comps[i];
    }
    return NULL;
}

static void draw_camera_scene_icon(ImDrawList *dl, ImVec2 center,
                                   float radius, bool selected)
{
    ImU32 stroke = selected ? IM_COL32(255, 255, 255, 245)
                            : IM_COL32(114, 230, 255, 245);
    ImU32 fill   = IM_COL32(18, 30, 42, 210);
    ImU32 shadow = IM_COL32(0, 0, 0, 80);

    ImVec2 body_min(center.x - radius * 0.85f, center.y - radius * 0.45f);
    ImVec2 body_max(center.x + radius * 0.25f, center.y + radius * 0.45f);
    ImVec2 lens_a(body_max.x - 1.0f, center.y - radius * 0.40f);
    ImVec2 lens_b(body_max.x + radius * 0.70f, center.y - radius * 0.78f);
    ImVec2 lens_c(body_max.x + radius * 0.70f, center.y + radius * 0.78f);
    ImVec2 top_a(center.x - radius * 0.35f, body_min.y - radius * 0.28f);
    ImVec2 top_b(center.x + radius * 0.05f, body_min.y - radius * 0.28f);

    dl->AddCircleFilled(ImVec2(center.x + 2.0f, center.y + 2.5f), radius + 4.0f,
                        shadow, 20);
    dl->AddRectFilled(body_min, body_max, fill, 3.0f);
    dl->AddRect(body_min, body_max, stroke, 3.0f, 0, 2.0f);
    dl->AddTriangleFilled(lens_a, lens_b, lens_c, fill);
    dl->AddTriangle(lens_a, lens_b, lens_c, stroke, 2.0f);
    dl->AddRectFilled(top_a, top_b,
                      selected ? IM_COL32(255, 255, 255, 220)
                               : IM_COL32(88, 198, 224, 220),
                      2.0f);
}

static void draw_light_scene_icon(ImDrawList *dl, ImVec2 center,
                                  float radius, int light_type,
                                  bool selected)
{
    ImU32 stroke = selected ? IM_COL32(255, 255, 255, 245)
                            : IM_COL32(255, 214, 92, 245);
    ImU32 fill   = IM_COL32(56, 40, 8, 215);
    ImU32 glow   = IM_COL32(255, 214, 92, 80);
    ImU32 core   = IM_COL32(255, 236, 156, 230);

    dl->AddCircleFilled(center, radius + 2.0f, IM_COL32(0, 0, 0, 70), 20);

    if (light_type == 0) {
        const float tau = 6.28318530718f;
        dl->AddCircleFilled(center, radius * 0.42f, fill, 16);
        dl->AddCircle(center, radius * 0.42f, stroke, 16, 2.0f);
        for (int i = 0; i < 8; i++) {
            float a = (tau * (float)i) / 8.0f;
            ImVec2 p0(center.x + cosf(a) * radius * 0.62f,
                      center.y + sinf(a) * radius * 0.62f);
            ImVec2 p1(center.x + cosf(a) * radius * 1.00f,
                      center.y + sinf(a) * radius * 1.00f);
            dl->AddLine(p0, p1, stroke, 1.8f);
        }
    } else if (light_type == 1) {
        dl->AddCircle(center, radius * 0.90f, glow, 20, 2.8f);
        dl->AddCircleFilled(center, radius * 0.45f, fill, 16);
        dl->AddCircle(center, radius * 0.45f, stroke, 16, 2.0f);
        dl->AddCircleFilled(center, radius * 0.18f, core, 12);
    } else {
        ImVec2 head(center.x, center.y - radius * 0.25f);
        ImVec2 cone_l(center.x - radius * 0.65f, center.y + radius * 0.55f);
        ImVec2 cone_r(center.x + radius * 0.65f, center.y + radius * 0.55f);
        dl->AddCircleFilled(head, radius * 0.25f, core, 14);
        dl->AddTriangleFilled(head, cone_l, cone_r, fill);
        dl->AddTriangle(head, cone_l, cone_r, stroke, 2.0f);
    }
}

static void build_helper_basis(const JceComponentInfo *xform,
                               float forward[3],
                               float right[3],
                               float up[3])
{
    float pitch = 0.0f;
    float yaw = 0.0f;
    float roll = 0.0f;
    if (xform) {
        pitch = xform->data.transform.rot[0] * JCE_DEG2RAD;
        yaw   = xform->data.transform.rot[1] * JCE_DEG2RAD;
        roll  = xform->data.transform.rot[2] * JCE_DEG2RAD;
    }

    float cp = cosf(pitch), sp = sinf(pitch);
    float cy = cosf(yaw),   sy = sinf(yaw);
    float cr = cosf(roll),  sr = sinf(roll);

    forward[0] = sy * cp;
    forward[1] = -sp;
    forward[2] = -cy * cp;

    float world_up[3] = { 0.0f, 1.0f, 0.0f };
    float base_right[3] = {
        world_up[1] * forward[2] - world_up[2] * forward[1],
        world_up[2] * forward[0] - world_up[0] * forward[2],
        world_up[0] * forward[1] - world_up[1] * forward[0]
    };
    float right_len = sqrtf(base_right[0] * base_right[0]
                          + base_right[1] * base_right[1]
                          + base_right[2] * base_right[2]);
    if (right_len < 1e-5f) {
        base_right[0] = 1.0f; base_right[1] = 0.0f; base_right[2] = 0.0f;
        right_len = 1.0f;
    }
    base_right[0] /= right_len;
    base_right[1] /= right_len;
    base_right[2] /= right_len;

    float base_up[3] = {
        forward[1] * base_right[2] - forward[2] * base_right[1],
        forward[2] * base_right[0] - forward[0] * base_right[2],
        forward[0] * base_right[1] - forward[1] * base_right[0]
    };

    right[0] = base_right[0] * cr + base_up[0] * sr;
    right[1] = base_right[1] * cr + base_up[1] * sr;
    right[2] = base_right[2] * cr + base_up[2] * sr;

    up[0] = right[1] * forward[2] - right[2] * forward[1];
    up[1] = right[2] * forward[0] - right[0] * forward[2];
    up[2] = right[0] * forward[1] - right[1] * forward[0];
}

static bool project_helper_point(const JceGizmoCamera *cam,
                                 const float world[3],
                                 ImVec2 *out)
{
    float screen[2];
    if (!gm_world_to_screen(cam, world, screen))
        return false;
    out->x = screen[0];
    out->y = screen[1];
    return true;
}

static void draw_camera_helper_lines(ImDrawList *dl,
                                     const JceGizmoCamera *cam,
                                     const JceComponentInfo *xform,
                                     const JceComponentInfo *camera,
                                     bool selected)
{
    if (!dl || !cam || !xform || !camera) return;

    float pos[3] = {
        xform->data.transform.pos[0],
        xform->data.transform.pos[1],
        xform->data.transform.pos[2]
    };
    float forward[3], right[3], up[3];
    build_helper_basis(xform, forward, right, up);

    float near_d = 0.9f;
    float far_d = 2.6f;
    float fov = camera->data.camera.fov > 1.0f ? camera->data.camera.fov : 60.0f;
    float half_h = tanf(fov * JCE_DEG2RAD * 0.5f) * far_d * 0.45f;
    float half_w = half_h * 1.25f;

    float apex[3] = { pos[0], pos[1], pos[2] };
    float center[3] = {
        pos[0] + forward[0] * far_d,
        pos[1] + forward[1] * far_d,
        pos[2] + forward[2] * far_d
    };
    float corners[4][3];
    for (int i = 0; i < 4; i++) {
        float sx = (i == 0 || i == 3) ? -1.0f : 1.0f;
        float sy = (i < 2) ? -1.0f : 1.0f;
        corners[i][0] = center[0] + right[0] * half_w * sx + up[0] * half_h * sy;
        corners[i][1] = center[1] + right[1] * half_w * sx + up[1] * half_h * sy;
        corners[i][2] = center[2] + right[2] * half_w * sx + up[2] * half_h * sy;
    }

    ImVec2 apex_s;
    if (!project_helper_point(cam, apex, &apex_s))
        return;

    ImU32 line_col = selected ? IM_COL32(255, 255, 255, 180)
                              : IM_COL32(114, 230, 255, 150);
    ImVec2 corner_s[4];
    int projected = 0;
    for (int i = 0; i < 4; i++)
        projected += project_helper_point(cam, corners[i], &corner_s[i]) ? 1 : 0;
    if (projected == 4) {
        for (int i = 0; i < 4; i++) {
            dl->AddLine(apex_s, corner_s[i], line_col, 1.5f);
            dl->AddLine(corner_s[i], corner_s[(i + 1) % 4], line_col, 1.5f);
        }
    }

    float aim[3] = {
        pos[0] + forward[0] * near_d,
        pos[1] + forward[1] * near_d,
        pos[2] + forward[2] * near_d
    };
    ImVec2 aim_s;
    if (project_helper_point(cam, aim, &aim_s))
        dl->AddLine(apex_s, aim_s, line_col, 2.0f);
}

static void draw_light_helper_lines(ImDrawList *dl,
                                    const JceGizmoCamera *cam,
                                    const JceComponentInfo *xform,
                                    const JceComponentInfo *light,
                                    bool selected)
{
    if (!dl || !cam || !xform || !light) return;

    float pos[3] = {
        xform->data.transform.pos[0],
        xform->data.transform.pos[1],
        xform->data.transform.pos[2]
    };
    float forward[3], right[3], up[3];
    build_helper_basis(xform, forward, right, up);

    ImU32 line_col = selected ? IM_COL32(255, 255, 255, 180)
                              : IM_COL32(255, 214, 92, 150);
    ImVec2 pos_s;
    if (!project_helper_point(cam, pos, &pos_s))
        return;

    if (light->data.light.type == 0) {
        for (int i = -1; i <= 1; i++) {
            float offset = (float)i * 0.45f;
            float start[3] = {
                pos[0] + right[0] * offset,
                pos[1] + right[1] * offset,
                pos[2] + right[2] * offset
            };
            float end[3] = {
                start[0] + forward[0] * 2.2f,
                start[1] + forward[1] * 2.2f,
                start[2] + forward[2] * 2.2f
            };
            ImVec2 a, b;
            if (project_helper_point(cam, start, &a) && project_helper_point(cam, end, &b)) {
                dl->AddLine(a, b, line_col, 1.7f);
                ImVec2 dir(b.x - a.x, b.y - a.y);
                float len = sqrtf(dir.x * dir.x + dir.y * dir.y);
                if (len > 1.0f) {
                    dir.x /= len; dir.y /= len;
                    ImVec2 n(-dir.y, dir.x);
                    ImVec2 tip1(b.x - dir.x * 7.0f + n.x * 3.0f,
                                b.y - dir.y * 7.0f + n.y * 3.0f);
                    ImVec2 tip2(b.x - dir.x * 7.0f - n.x * 3.0f,
                                b.y - dir.y * 7.0f - n.y * 3.0f);
                    dl->AddLine(b, tip1, line_col, 1.7f);
                    dl->AddLine(b, tip2, line_col, 1.7f);
                }
            }
        }
    } else if (light->data.light.type == 1) {
        float axes[6][3] = {
            { 1, 0, 0 }, { -1, 0, 0 },
            { 0, 1, 0 }, { 0, -1, 0 },
            { 0, 0, 1 }, { 0, 0, -1 }
        };
        for (int i = 0; i < 6; i++) {
            float end[3] = {
                pos[0] + axes[i][0] * 0.9f,
                pos[1] + axes[i][1] * 0.9f,
                pos[2] + axes[i][2] * 0.9f
            };
            ImVec2 e;
            if (project_helper_point(cam, end, &e))
                dl->AddLine(pos_s, e, line_col, 1.4f);
        }
    } else {
        float base_center[3] = {
            pos[0] + forward[0] * 2.4f,
            pos[1] + forward[1] * 2.4f,
            pos[2] + forward[2] * 2.4f
        };
        float corners[4][3];
        for (int i = 0; i < 4; i++) {
            float sx = (i == 0 || i == 3) ? -1.0f : 1.0f;
            float sy = (i < 2) ? -1.0f : 1.0f;
            corners[i][0] = base_center[0] + right[0] * 0.9f * sx + up[0] * 0.6f * sy;
            corners[i][1] = base_center[1] + right[1] * 0.9f * sx + up[1] * 0.6f * sy;
            corners[i][2] = base_center[2] + right[2] * 0.9f * sx + up[2] * 0.6f * sy;
        }
        ImVec2 corner_s[4];
        int projected = 0;
        for (int i = 0; i < 4; i++)
            projected += project_helper_point(cam, corners[i], &corner_s[i]) ? 1 : 0;
        if (projected == 4) {
            for (int i = 0; i < 4; i++) {
                dl->AddLine(pos_s, corner_s[i], line_col, 1.5f);
                dl->AddLine(corner_s[i], corner_s[(i + 1) % 4], line_col, 1.5f);
            }
        }
    }
}

static void draw_scene_helper_icons(ImDrawList *dl,
                                    const JceGizmoCamera *cam)
{
    if (!dl || !cam) return;

    int total = jce_state_get_entity_count();
    for (int i = 0; i < total; i++) {
        JceEntityInfo *ent = jce_state_get_entity_by_index(i);
        if (!ent || !ent->enabled) continue;

        int comp_count = 0;
        JceComponentInfo *comps = jce_state_get_entity_components(ent->id, &comp_count);
        JceComponentInfo *xform = find_transform_component(comps, comp_count);
        if (!xform) continue;

        JceComponentInfo *camera = find_component_by_type(comps, comp_count,
                                                          JCE_COMP_CAMERA);
        JceComponentInfo *light  = find_component_by_type(comps, comp_count,
                                                          JCE_COMP_LIGHT);
        if (!camera && !light) continue;

        float world[3] = {
            xform->data.transform.pos[0],
            xform->data.transform.pos[1],
            xform->data.transform.pos[2]
        };
        float screen[2];
        if (!gm_world_to_screen(cam, world, screen))
            continue;

        const float pad = 24.0f;
        if (screen[0] < cam->viewport_origin[0] - pad
            || screen[0] > cam->viewport_origin[0] + cam->viewport_size[0] + pad
            || screen[1] < cam->viewport_origin[1] - pad
            || screen[1] > cam->viewport_origin[1] + cam->viewport_size[1] + pad)
            continue;

        bool selected = jce_state_is_selected(ent->id);
        bool skip_icon = selected && ent->id == jce_state_get_focused()
                      && jce_editor_prefs_show_gizmos();
        ImVec2 center(screen[0], screen[1]);
        if (camera)
            draw_camera_helper_lines(dl, cam, xform, camera, selected);
        if (light)
            draw_light_helper_lines(dl, cam, xform, light, selected);

        if (skip_icon) {
            continue;
        }

        if (camera && light) {
            draw_camera_scene_icon(dl, ImVec2(center.x - 14.0f, center.y),
                                   16.0f, selected);
            draw_light_scene_icon(dl, ImVec2(center.x + 14.0f, center.y),
                                  16.0f, light->data.light.type, selected);
        } else if (camera) {
            draw_camera_scene_icon(dl, center, 18.0f, selected);
        } else if (light) {
            draw_light_scene_icon(dl, center, 18.0f,
                                  light->data.light.type, selected);
        }
    }
}

static void set_entity_mesh_shape(uint32_t entity_id, int mesh_shape)
{
    int comp_count = 0;
    JceComponentInfo *comps = jce_state_get_entity_components(entity_id, &comp_count);
    if (!comps) return;

    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == JCE_COMP_MESH_RENDERER) {
            comps[i].data.mesh_renderer.mesh_shape = mesh_shape;
            break;
        }
    }
}

static uint32_t create_default_scene_entity(const char *name,
                                            uint32_t parent_id,
                                            JceComponentType extra_type,
                                            int mesh_shape)
{
    jce_state_begin_batch_edit();

    uint32_t id = jce_state_create_entity(name, parent_id);
    if (id != 0) {
        jce_state_add_component(id, JCE_COMP_TRANSFORM);
        if (extra_type != JCE_COMP_TYPE_COUNT) {
            jce_state_add_component(id, extra_type);
            if (extra_type == JCE_COMP_MESH_RENDERER)
                set_entity_mesh_shape(id, mesh_shape);
        }
    }

    jce_state_end_batch_edit();
    return id;
}

/* ── Helper: transform 3D direction by camera view matrix → 2D ───── */

static ImVec2 project_axis(const float *view16, float dx, float dy, float dz,
                           float cx, float cy, float radius)
{
    /* Multiply direction by upper-left 3x3 of view matrix (rotation only). */
    float sx = view16[0] * dx + view16[4] * dy + view16[8]  * dz;
    float sy = view16[1] * dx + view16[5] * dy + view16[9]  * dz;
    /* Screen X → right, Screen Y → down (ImGui convention). */
    return ImVec2(cx + sx * radius, cy - sy * radius);
}

/* ── Ray-AABB intersection (slab method) ──────────────────────────── */

static bool ray_aabb_intersect(const float ro[3], const float rd[3],
                                const float bmin[3], const float bmax[3],
                                float *out_t)
{
    float tmin = 0.0f, tmax = 1e30f;
    for (int i = 0; i < 3; i++) {
        if (fabsf(rd[i]) < 1e-8f) {
            if (ro[i] < bmin[i] || ro[i] > bmax[i]) return false;
        } else {
            float inv = 1.0f / rd[i];
            float t1 = (bmin[i] - ro[i]) * inv;
            float t2 = (bmax[i] - ro[i]) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return false;
        }
    }
    *out_t = tmin;
    return true;
}

/* ── Axis Indicator (bottom-left corner) ──────────────────────────── */

static int draw_axis_indicator(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                               const float *view16)
{
    const float margin = 10.0f;
    float radius = fminf(size.x, size.y) * 0.04f;
    if (radius < 20.0f) radius = 20.0f;
    if (radius > 45.0f) radius = 45.0f;

    /* Center of the indicator widget. */
    float cx = origin.x + margin + radius + 5.0f;
    float cy = origin.y + size.y - margin - radius - 5.0f;

    /* Background circle. */
    dl->AddCircleFilled(ImVec2(cx, cy), radius + 8.0f, IM_COL32(30, 30, 40, 180), 32);
    dl->AddCircle(ImVec2(cx, cy), radius + 8.0f, IM_COL32(80, 80, 90, 120), 32, 1.0f);

    /* Project each axis direction using the view matrix rotation. */
    struct { float dx, dy, dz; ImU32 col; const char *label; } axes[3] = {
        { 1, 0, 0, IM_COL32(255, 80, 80, 255), "X" },
        { 0, 1, 0, IM_COL32(80, 255, 80, 255), "Y" },
        { 0, 0, 1, IM_COL32(80, 80, 255, 255), "Z" },
    };

    /* Draw axes sorted by depth (back to front) so closer axis draws on top. */
    float depths[3];
    int order[3] = { 0, 1, 2 };
    for (int i = 0; i < 3; i++)
        depths[i] = view16[2] * axes[i].dx + view16[6] * axes[i].dy
                   + view16[10] * axes[i].dz;

    /* Simple bubble sort of 3 elements by depth (farthest first). */
    for (int i = 0; i < 2; i++)
        for (int j = i + 1; j < 3; j++)
            if (depths[order[i]] > depths[order[j]]) {
                int tmp = order[i]; order[i] = order[j]; order[j] = tmp;
            }

    for (int k = 0; k < 3; k++) {
        int i = order[k];
        ImVec2 tip = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                  cx, cy, radius);

        /* Axis line. */
        dl->AddLine(ImVec2(cx, cy), tip, axes[i].col, 2.0f);

        /* Small circle at tip. */
        dl->AddCircleFilled(tip, 4.0f, axes[i].col, 12);

        /* Label slightly beyond the tip. */
        ImVec2 label_pos = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                        cx, cy, radius + 12.0f);
        ImVec2 text_size = ImGui::CalcTextSize(axes[i].label);
        dl->AddText(ImVec2(label_pos.x - text_size.x * 0.5f,
                           label_pos.y - text_size.y * 0.5f),
                    axes[i].col, axes[i].label);
    }

    /* Click detection: check if mouse clicked on an axis tip or its negative. */
    int clicked_axis = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        ImVec2 mouse = ImGui::GetMousePos();
        float click_r = 12.0f;
        for (int i = 0; i < 3; i++) {
            /* Positive axis tip. */
            ImVec2 tip = project_axis(view16, axes[i].dx, axes[i].dy, axes[i].dz,
                                      cx, cy, radius);
            float dx = mouse.x - tip.x, dy = mouse.y - tip.y;
            if (dx * dx + dy * dy < click_r * click_r) {
                clicked_axis = i; /* 0=+X, 1=+Y, 2=+Z */
                break;
            }
            /* Negative axis (opposite direction). */
            ImVec2 neg = project_axis(view16, -axes[i].dx, -axes[i].dy, -axes[i].dz,
                                      cx, cy, radius * 0.5f);
            dx = mouse.x - neg.x; dy = mouse.y - neg.y;
            if (dx * dx + dy * dy < click_r * click_r) {
                clicked_axis = i + 3; /* 3=-X, 4=-Y, 5=-Z */
                break;
            }
        }
    }
    return clicked_axis;
}

/* ── View Cube (top-right corner) ─────────────────────────────────── */

/* Returns -1 for no click, -2 for home button click, or 0-5 for Front/Back/Top/Bottom/Right/Left. */
static int draw_view_cube(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                           const float *view16)
{
    const float margin = 10.0f;
    float cube_size = fminf(size.x, size.y) * 0.06f;
    if (cube_size < 28.0f) cube_size = 28.0f;
    if (cube_size > 50.0f) cube_size = 50.0f;

    float cx = origin.x + size.x - margin - cube_size - 5.0f;
    float cy = origin.y + margin + cube_size + 5.0f;

    /* Background. */
    float bg_r = cube_size + 10.0f;
    float bg_x0 = cx - bg_r, bg_y0 = cy - bg_r;
    float bg_x1 = cx + bg_r, bg_y1 = cy + bg_r;
    dl->AddRectFilled(ImVec2(bg_x0, bg_y0), ImVec2(bg_x1, bg_y1),
                      IM_COL32(30, 30, 40, 160), 6.0f);
    dl->AddRect(ImVec2(bg_x0, bg_y0), ImVec2(bg_x1, bg_y1),
                IM_COL32(80, 80, 90, 100), 6.0f, 0, 1.0f);

    /* ── Home button (top-left of background) ────────────────────── */
    const float home_size = 13.0f;
    ImVec2 home_min = ImVec2(bg_x0 + 4.0f, bg_y0 + 4.0f);
    ImVec2 home_max = ImVec2(home_min.x + home_size, home_min.y + home_size);
    ImVec2 mouse = ImGui::GetMousePos();
    bool home_hovered = mouse.x >= home_min.x && mouse.x <= home_max.x
                     && mouse.y >= home_min.y && mouse.y <= home_max.y;
    ImU32 home_col = home_hovered ? IM_COL32(220, 220, 100, 255) : IM_COL32(180, 180, 180, 180);

    /* Draw a simplistic house icon: roof triangle + body rect + door rect */
    float hx = home_min.x, hy = home_min.y;
    float hw = home_size, hh = home_size;
    /* Roof (triangle) */
    dl->AddTriangleFilled(
        ImVec2(hx + hw * 0.5f, hy),
        ImVec2(hx,              hy + hh * 0.5f),
        ImVec2(hx + hw,         hy + hh * 0.5f),
        home_col);
    /* Body (rect) */
    dl->AddRectFilled(ImVec2(hx + hw * 0.15f, hy + hh * 0.47f),
                      ImVec2(hx + hw * 0.85f, hy + hh * 1.0f),
                      home_col);
    /* Door cutout (darker) */
    dl->AddRectFilled(ImVec2(hx + hw * 0.35f, hy + hh * 0.65f),
                      ImVec2(hx + hw * 0.65f, hy + hh * 1.0f),
                      IM_COL32(30, 30, 40, 200));

    /* Detect home button click. */
    if (home_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
        return -2;

    /* 8 corners of a unit cube centered at origin. */
    static const float corners[8][3] = {
        {-1,-1,-1}, { 1,-1,-1}, { 1, 1,-1}, {-1, 1,-1},
        {-1,-1, 1}, { 1,-1, 1}, { 1, 1, 1}, {-1, 1, 1},
    };

    /* Project corners. */
    ImVec2 proj[8];
    float proj_z[8];
    for (int i = 0; i < 8; i++) {
        float x = corners[i][0], y = corners[i][1], z = corners[i][2];
        float sx = view16[0]*x + view16[4]*y + view16[8]*z;
        float sy = view16[1]*x + view16[5]*y + view16[9]*z;
        float sz = view16[2]*x + view16[6]*y + view16[10]*z;
        proj[i] = ImVec2(cx + sx * cube_size * 0.55f,
                         cy - sy * cube_size * 0.55f);
        proj_z[i] = sz;
    }

    /* 6 faces: indices + colors + labels. */
    struct CubeFace {
        int idx[4];
        ImU32 col;
        ImU32 col_edge;
        const char *label;
    };

    CubeFace faces[6] = {
        {{ 4, 5, 6, 7 }, IM_COL32(80, 80, 255, 100), IM_COL32(80, 80, 255, 180), "F"},  /* +Z front */
        {{ 1, 0, 3, 2 }, IM_COL32(80, 80, 180, 100), IM_COL32(80, 80, 180, 140), "Bk"}, /* -Z back */
        {{ 2, 3, 7, 6 }, IM_COL32(80, 255, 80, 100), IM_COL32(80, 255, 80, 180), "T"},  /* +Y top */
        {{ 0, 1, 5, 4 }, IM_COL32(80, 180, 80, 100), IM_COL32(80, 180, 80, 140), "Bt"}, /* -Y bottom */
        {{ 1, 2, 6, 5 }, IM_COL32(255, 80, 80, 100), IM_COL32(255, 80, 80, 180), "R"},  /* +X right */
        {{ 3, 0, 4, 7 }, IM_COL32(180, 80, 80, 100), IM_COL32(180, 80, 80, 140), "L"},  /* -X left */
    };

    /* Face normals for depth sorting. */
    static const float face_normals[6][3] = {
        { 0, 0, 1}, { 0, 0,-1}, { 0, 1, 0}, { 0,-1, 0}, { 1, 0, 0}, {-1, 0, 0},
    };

    /* Sort faces by depth (back to front). */
    float face_depth[6];
    int face_order[6] = { 0, 1, 2, 3, 4, 5 };
    for (int i = 0; i < 6; i++) {
        float nz = view16[2]*face_normals[i][0] + view16[6]*face_normals[i][1]
                 + view16[10]*face_normals[i][2];
        face_depth[i] = nz;
    }
    for (int i = 0; i < 5; i++)
        for (int j = i + 1; j < 6; j++)
            if (face_depth[face_order[i]] > face_depth[face_order[j]]) {
                int tmp = face_order[i];
                face_order[i] = face_order[j];
                face_order[j] = tmp;
            }

    /* Draw faces. */
    for (int k = 0; k < 6; k++) {
        int fi = face_order[k];
        const CubeFace &f = faces[fi];

        /* Only draw front-facing faces (normal pointing towards camera). */
        if (face_depth[fi] < 0.0f) continue;

        /* Filled quad (two triangles). */
        ImVec2 p0 = proj[f.idx[0]], p1 = proj[f.idx[1]];
        ImVec2 p2 = proj[f.idx[2]], p3 = proj[f.idx[3]];
        dl->AddQuadFilled(p0, p1, p2, p3, f.col);
        dl->AddQuad(p0, p1, p2, p3, f.col_edge, 1.0f);

        /* Label at face center. */
        ImVec2 center = ImVec2(
            (p0.x + p1.x + p2.x + p3.x) * 0.25f,
            (p0.y + p1.y + p2.y + p3.y) * 0.25f);
        ImVec2 text_size = ImGui::CalcTextSize(f.label);
        dl->AddText(ImVec2(center.x - text_size.x * 0.5f,
                           center.y - text_size.y * 0.5f),
                    IM_COL32(255, 255, 255, 220), f.label);
    }

    /* Draw edges of the cube for all 12 edges. */
    static const int edges[12][2] = {
        {0,1},{1,2},{2,3},{3,0},  /* back face */
        {4,5},{5,6},{6,7},{7,4},  /* front face */
        {0,4},{1,5},{2,6},{3,7},  /* connecting edges */
    };
    for (int i = 0; i < 12; i++)
        dl->AddLine(proj[edges[i][0]], proj[edges[i][1]],
                    IM_COL32(120, 120, 130, 120), 1.0f);

    /* Click detection: check if mouse clicked inside a front-facing face. */
    int clicked_face = -1;
    if (ImGui::IsMouseClicked(ImGuiMouseButton_Left)) {
        /* Check faces in reverse draw order (front-most first). */
        for (int k = 5; k >= 0; k--) {
            int fi = face_order[k];
            if (face_depth[fi] < 0.0f) continue;
            const CubeFace &f = faces[fi];
            ImVec2 p0 = proj[f.idx[0]], p1 = proj[f.idx[1]];
            ImVec2 p2 = proj[f.idx[2]], p3 = proj[f.idx[3]];
            /* Point-in-quad test via two triangles. */
            auto cross2d = [](ImVec2 a, ImVec2 b, ImVec2 c) -> float {
                return (b.x - a.x) * (c.y - a.y) - (b.y - a.y) * (c.x - a.x);
            };
            auto in_tri = [&](ImVec2 a, ImVec2 b, ImVec2 c) -> bool {
                float d1 = cross2d(a, b, mouse);
                float d2 = cross2d(b, c, mouse);
                float d3 = cross2d(c, a, mouse);
                bool has_neg = (d1 < 0) || (d2 < 0) || (d3 < 0);
                bool has_pos = (d1 > 0) || (d2 > 0) || (d3 > 0);
                return !(has_neg && has_pos);
            };
            if (in_tri(p0, p1, p2) || in_tri(p0, p2, p3)) {
                clicked_face = fi;
                break;
            }
        }
    }
    return clicked_face;
}

/* ── Content (embeddable in tabs) ─────────────────────────────────── */

void jce_editor_panel_scene_view_content(void)
{
    /* Toolbar row (gizmo mode / space / 2D-3D / view menu) */
    JceGizmoMode gm = jce_state_get_gizmo_mode();
    if (ImGui::RadioButton("T", gm == JCE_GIZMO_TRANSLATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.tooltip.translate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("R", gm == JCE_GIZMO_ROTATE))
        jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.tooltip.rotate"));
    ImGui::SameLine();
    if (ImGui::RadioButton("S", gm == JCE_GIZMO_SCALE))
        jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
    if (ImGui::IsItemHovered())
        ImGui::SetTooltip("%s", jce_editor_i18n("sceneView.tooltip.scale"));

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    JceGizmoSpace gs = jce_state_get_gizmo_space();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###local", jce_editor_i18n("toolbar.local"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_LOCAL))
            jce_state_set_gizmo_space(JCE_GIZMO_LOCAL);
    }
    ImGui::SameLine();
    {
        char _lbl[64];
        snprintf(_lbl, sizeof(_lbl), "%s###world", jce_editor_i18n("toolbar.global"));
        if (ImGui::RadioButton(_lbl, gs == JCE_GIZMO_WORLD))
            jce_state_set_gizmo_space(JCE_GIZMO_WORLD);
    }

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    /* 2D / 3D toggle */
    bool is_2d = jce_state_get_2d_mode();
    if (ImGui::RadioButton(jce_editor_i18n("sceneView.mode2d"), is_2d))
        jce_state_set_2d_mode(true);
    ImGui::SameLine();
    if (ImGui::RadioButton(jce_editor_i18n("sceneView.mode3d"), !is_2d))
        jce_state_set_2d_mode(false);

    ImGui::SameLine();
    ImGui::SeparatorEx(ImGuiSeparatorFlags_Vertical);
    ImGui::SameLine();

    /* View dropdown (Render Mode, Grid, Live Preview, Camera presets) */
    if (ImGui::Button(jce_editor_i18n("menu.view"))) {
        ImGui::OpenPopup("##SceneViewMenu");
    }
    if (ImGui::BeginPopup("##SceneViewMenu")) {
        JceSceneViewMode vm = jce_state_get_view_mode();

        if (ImGui::BeginMenu(jce_editor_i18n("sceneView.renderMode"))) {
            if (ImGui::MenuItem(jce_editor_i18n("scene.wireframe"), NULL, vm == JCE_VIEW_WIREFRAME))
                jce_state_set_view_mode(JCE_VIEW_WIREFRAME);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.shaded"), NULL, vm == JCE_VIEW_SHADED))
                jce_state_set_view_mode(JCE_VIEW_SHADED);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.textured"), NULL, vm == JCE_VIEW_TEXTURED))
                jce_state_set_view_mode(JCE_VIEW_TEXTURED);
            ImGui::EndMenu();
        }

        bool grid = jce_state_get_show_grid();
        if (ImGui::MenuItem(jce_editor_i18n("scene.grid"), NULL, grid))
            jce_state_set_show_grid(!grid);

        bool lp = jce_state_get_live_preview();
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.livePreview"), NULL, lp))
            jce_state_set_live_preview(!lp);

        ImGui::Separator();

        if (ImGui::MenuItem(jce_editor_i18n("sceneView.resetCamera")))  {
            jce_editor_scene_camera_reset();
        }
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.topView")))      { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_TOP); }
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.frontView")))    { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_FRONT); }
        if (ImGui::MenuItem(jce_editor_i18n("sceneView.sideView")))     { jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_RIGHT); }

        ImGui::EndPopup();
    }

    ImGui::Separator();

    /* ── Viewport area ──────────────────────────────────────────── */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    if (avail.x <= 0 || avail.y <= 0) {
        clear_stale_gizmo_interaction_state();
        return;
    }

    ImVec2 screen_pos = ImGui::GetCursorScreenPos();
    ImDrawList *dl = ImGui::GetWindowDrawList();

    /* Background fill — rgba(30, 30, 40, 255) (Java reference). */
    dl->AddRectFilled(screen_pos,
                      ImVec2(screen_pos.x + avail.x, screen_pos.y + avail.y),
                      IM_COL32(30, 30, 40, 255));

    /* Trigger 3D scene rendering to the FBO with viewport dimensions. */
    uint32_t vp_w = (uint32_t)avail.x;
    uint32_t vp_h = (uint32_t)avail.y;
    jce_editor_scene_render_frame(vp_w, vp_h);

    /* Display the FBO texture. */
    uint16_t tex_idx = jce_editor_scene_render_get_texture();
    if (tex_idx != UINT16_MAX) {
        /* OpenGL framebuffers have bottom-left origin; flip UV Y so the
           image is not vertically inverted in the ImGui viewport. */
        const bgfx_caps_t *caps = bgfx_get_caps();
        if (caps->originBottomLeft) {
            ImGui::Image((ImTextureID)(uintptr_t)tex_idx, avail,
                         ImVec2(0.0f, 1.0f), ImVec2(1.0f, 0.0f));
        } else {
            ImGui::Image((ImTextureID)(uintptr_t)tex_idx, avail);
        }
    } else {
        /* Fallback: show status text on the dark background. */
        ImGui::SetCursorScreenPos(ImVec2(screen_pos.x + 8, screen_pos.y + 8));
        ImGui::TextColored(ImVec4(1, 1, 1, 0.6f),
            "%s  %.0f x %.0f", jce_editor_i18n("Scene"), avail.x, avail.y);
    }

    /* Invisible button overlaid for input capture. */
    ImGui::SetCursorScreenPos(screen_pos);
    ImGui::InvisibleButton("##SceneViewInput", avail);
    bool viewport_hovered = ImGui::IsItemHovered();
    (void)ImGui::IsItemActive(); /* reserved for future drag handling */

    /* ── Right-click context menu (scene viewport) ────────────── */
    /* Use manual bounds check so repeated right-clicks work even
       when an ImGui popup is covering the InvisibleButton. */
    {
        ImVec2 mpos = ImGui::GetMousePos();
        bool mouse_in_vp = (mpos.x >= screen_pos.x &&
                            mpos.x <= screen_pos.x + avail.x &&
                            mpos.y >= screen_pos.y &&
                            mpos.y <= screen_pos.y + avail.y);

        if (mouse_in_vp && ImGui::IsMouseClicked(ImGuiMouseButton_Right)
            && !ImGui::GetIO().KeyAlt)
        {
            ImGui::OpenPopup("SceneViewContextMenu");
        }
    }
    if (ImGui::BeginPopup("SceneViewContextMenu")) {
        uint32_t focused = jce_state_get_focused();
        int sel_count = 0;
        const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
        bool has_selection = sel_count > 0;

        if (ImGui::BeginMenu(jce_editor_i18n("dialog.create"))) {
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createEmpty"))) {
                uint32_t id = create_default_scene_entity("New Entity", 0,
                                                          JCE_COMP_TYPE_COUNT,
                                                          JCE_MESH_SHAPE_CUBE);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
            }

            ImGui::Separator();

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create2D"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSprite"))) {
                    uint32_t id = create_default_scene_entity("Sprite", 0,
                                                              JCE_COMP_SPRITE_RENDERER,
                                                              JCE_MESH_SHAPE_CUBE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createText"))) {
                    uint32_t id = create_default_scene_entity("UI Text", 0,
                                                              JCE_COMP_TYPE_COUNT,
                                                              JCE_MESH_SHAPE_CUBE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                }
                ImGui::EndMenu();
            }

            if (ImGui::BeginMenu(jce_editor_i18n("hierarchy.create3D"))) {
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCube"))) {
                    uint32_t id = create_default_scene_entity("Cube", 0,
                                                              JCE_COMP_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_CUBE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createSphere"))) {
                    uint32_t id = create_default_scene_entity("Sphere", 0,
                                                              JCE_COMP_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_SPHERE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createPlane"))) {
                    uint32_t id = create_default_scene_entity("Plane", 0,
                                                              JCE_COMP_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_PLANE);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                }
                if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCylinder"))) {
                    uint32_t id = create_default_scene_entity("Cylinder", 0,
                                                              JCE_COMP_MESH_RENDERER,
                                                              JCE_MESH_SHAPE_CYLINDER);
                    jce_state_select_entity(id, false);
                    jce_editor_inspector_request_sync();
                }
                ImGui::EndMenu();
            }

            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createCamera"))) {
                uint32_t id = create_default_scene_entity("Camera", 0,
                                                          JCE_COMP_CAMERA,
                                                          JCE_MESH_SHAPE_CUBE);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
            }
            if (ImGui::MenuItem(jce_editor_i18n("menu.gameObject.createLight"))) {
                uint32_t id = create_default_scene_entity("Light", 0,
                                                          JCE_COMP_LIGHT,
                                                          JCE_MESH_SHAPE_CUBE);
                jce_state_select_entity(id, false);
                jce_editor_inspector_request_sync();
            }

            ImGui::EndMenu();
        }

        if (has_selection) {
            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.duplicate"), "Ctrl+D")) {
                if (sel_count > 1) {
                    uint32_t dup_ids[JCE_MAX_SELECTED];
                    int dup_count = 0;
                    int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                    jce_state_begin_batch_edit();
                    for (int i = 0; i < n; i++) {
                        uint32_t dup = jce_state_duplicate_entity(sel_ids[i]);
                        if (dup != 0 && dup_count < JCE_MAX_SELECTED)
                            dup_ids[dup_count++] = dup;
                    }
                    jce_state_end_batch_edit();
                    if (dup_count > 0) {
                        jce_state_select_entity(dup_ids[0], false);
                        for (int i = 1; i < dup_count; i++)
                            jce_state_select_entity(dup_ids[i], true);
                    }
                } else if (focused != 0) {
                    uint32_t dup = jce_state_duplicate_entity(focused);
                    if (dup != 0)
                        jce_state_select_entity(dup, false);
                }
                jce_editor_inspector_request_sync();
            }

            if (ImGui::MenuItem(jce_editor_i18n("menu.edit.delete"), "Delete")) {
                uint32_t ids[JCE_MAX_SELECTED];
                int n = sel_count < JCE_MAX_SELECTED ? sel_count : JCE_MAX_SELECTED;
                for (int si = 0; si < n; si++)
                    ids[si] = sel_ids[si];
                if (n > 0)
                    jce_editor_inspector_request_delete_confirm_many(ids, n);
            }

            ImGui::Separator();

            if (ImGui::MenuItem(jce_editor_i18n("scene.focusSelected"), "F")) {
                if (focused != 0) {
                    int fc = 0;
                    JceComponentInfo *fcomps = jce_state_get_entity_components(focused, &fc);
                    for (int fi = 0; fi < fc; fi++) {
                        if (fcomps[fi].type == JCE_COMP_TRANSFORM) {
                            jce_editor_scene_camera_set_target(
                                fcomps[fi].data.transform.pos[0],
                                fcomps[fi].data.transform.pos[1],
                                fcomps[fi].data.transform.pos[2]);
                            break;
                        }
                    }
                }
            }

            if (ImGui::BeginMenu(jce_editor_i18n("sceneView.gizmoMode"))) {
                JceGizmoMode menu_gm = jce_state_get_gizmo_mode();
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.translate"), "W", menu_gm == JCE_GIZMO_TRANSLATE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.rotate"), "E", menu_gm == JCE_GIZMO_ROTATE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
                if (ImGui::MenuItem(jce_editor_i18n("toolbar.scale"), "R", menu_gm == JCE_GIZMO_SCALE))
                    jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);
                ImGui::EndMenu();
            }
        }

        ImGui::Separator();

        if (ImGui::BeginMenu(jce_editor_i18n("menu.view"))) {
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.resetCamera")))
                jce_editor_scene_camera_reset();
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.topView")))
                jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_TOP);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.frontView")))
                jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_FRONT);
            if (ImGui::MenuItem(jce_editor_i18n("sceneView.sideView")))
                jce_editor_scene_camera_snap_view(JCE_CAM_VIEW_RIGHT);
            ImGui::EndMenu();
        }

        ImGui::EndPopup();
    }

    /* ── Maya-style camera controls ───────────────────────────── */
    {
        ImGuiIO &io = ImGui::GetIO();
        bool alt_held = io.KeyAlt;

        /* Alt + LMB drag: orbit */
        if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Left, 1.0f)) {
            float dyaw   = io.MouseDelta.x * 0.005f;
            float dpitch = io.MouseDelta.y * 0.005f;
            jce_editor_scene_camera_orbit(dyaw, dpitch);
        }

        /* Alt + MMB drag: pan */
        if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Middle, 1.0f)) {
            jce_editor_scene_camera_pan(io.MouseDelta.x, io.MouseDelta.y);
        }

        /* Alt + RMB drag: zoom (vertical drag) */
        if (viewport_hovered && alt_held && ImGui::IsMouseDragging(ImGuiMouseButton_Right, 1.0f)) {
            float zoom_delta = -io.MouseDelta.y * 0.05f;
            jce_editor_scene_camera_zoom(zoom_delta);
        }

        /* Scroll wheel: zoom (always when hovered). */
        if (viewport_hovered && fabsf(io.MouseWheel) > 0.0f) {
            jce_editor_scene_camera_zoom(io.MouseWheel);
        }
    }

    /* ── Selection box (marquee) ──────────────────────────────── */
    if (!has_valid_gizmo_target())
        clear_stale_gizmo_interaction_state();

    if (viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
        && !ImGui::GetIO().KeyAlt
        && !jce_gizmo_is_active()
        && jce_gizmo_hovered_axis() == JCE_GIZMO_AXIS_NONE)
    {
        s_is_selecting = true;
        s_sel_start    = ImGui::GetMousePos();
        s_sel_current  = s_sel_start;

        /* Roll for Easter egg: 4.161014% probability. */
        s_sel_easter = ((float)rand() / (float)RAND_MAX) < 0.04161014f;
        if (s_sel_easter) {
            /* Unity-style: white border, light blue fill. */
            s_sel_border = IM_COL32(255, 255, 255, 200);
            s_sel_fill   = IM_COL32(100, 150, 255, 40);
            s_sel_inner  = IM_COL32(100, 150, 255, 100);

            // /* Default: orange border + orange fill (selection highlight). */
            // s_sel_border = IM_COL32(255, 165, 0, 255);
            // s_sel_fill   = IM_COL32(255, 165, 0, 30);
            // s_sel_inner  = IM_COL32(255, 165, 0, 80);
        } else {
            s_sel_border = IM_COL32(0, 255, 0, 255);
            s_sel_fill   = IM_COL32(0, 255, 0, 40);
        }
    }

    if (s_is_selecting) {
        s_sel_current = ImGui::GetMousePos();

        if (ImGui::IsMouseReleased(ImGuiMouseButton_Left)) {
            s_is_selecting = false;
            float bw = fabsf(s_sel_current.x - s_sel_start.x);
            float bh = fabsf(s_sel_current.y - s_sel_start.y);
            if (bw > 5.0f || bh > 5.0f) {
                /* Marquee: schedule hit-testing against entities in the
                   camera matrices block (below) where view/proj are known. */
                s_sel_pending  = true;
                s_sel_rect_min = ImVec2(fminf(s_sel_start.x, s_sel_current.x),
                                        fminf(s_sel_start.y, s_sel_current.y));
                s_sel_rect_max = ImVec2(fmaxf(s_sel_start.x, s_sel_current.x),
                                        fmaxf(s_sel_start.y, s_sel_current.y));
            } else {
                /* Single click: schedule ray-pick in camera matrices block. */
                s_sel_click_pending = true;
                s_sel_click_pos     = s_sel_start;
            }
        }
    }

    if (s_is_selecting) {
        float minX = fminf(s_sel_start.x, s_sel_current.x);
        float minY = fminf(s_sel_start.y, s_sel_current.y);
        float maxX = fmaxf(s_sel_start.x, s_sel_current.x);
        float maxY = fmaxf(s_sel_start.y, s_sel_current.y);

        /* Only draw if the box is larger than a few pixels (not a click). */
        if ((maxX - minX) > 3.0f || (maxY - minY) > 3.0f) {
            dl->AddRectFilled(ImVec2(minX, minY), ImVec2(maxX, maxY), s_sel_fill);
            dl->AddRect(ImVec2(minX + 1, minY + 1), ImVec2(maxX - 1, maxY - 1),
                        s_sel_inner, 0.0f, 0, 1.0f);
            dl->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY),
                        s_sel_border, 0.0f, 0, 1.5f);
        }
    }

    /* ── Gizmo overlay ────────────────────────────────────────── */
    if (jce_editor_prefs_show_gizmos()) {
        uint32_t focused = jce_state_get_focused();
        if (focused != 0) {
            int comp_count = 0;
            JceComponentInfo *comps = jce_state_get_entity_components(focused, &comp_count);
            JceComponentInfo *xform = find_transform_component(comps, comp_count);

            if (xform) {
                JceGizmoCamera gcam;
                memset(&gcam, 0, sizeof(gcam));

                if (!jce_editor_scene_get_camera_matrices(
                        gcam.view, gcam.proj, gcam.eye,
                        avail.x, avail.y))
                {
                    /* Fallback: simple default perspective camera. */
                    float eye[3] = {5.0f, 5.0f, 5.0f};
                    float center[3] = {0.0f, 0.0f, 0.0f};
                    float up[3] = {0.0f, 1.0f, 0.0f};

                    float f[3], s[3], u[3];
                    gm_v3_sub(f, center, eye);
                    gm_v3_normalize(f, f);
                    gm_v3_cross(s, f, up);
                    gm_v3_normalize(s, s);
                    gm_v3_cross(u, s, f);

                    gcam.view[0] = s[0];  gcam.view[4] = s[1];  gcam.view[8]  = s[2];  gcam.view[12] = -gm_v3_dot(s, eye);
                    gcam.view[1] = u[0];  gcam.view[5] = u[1];  gcam.view[9]  = u[2];  gcam.view[13] = -gm_v3_dot(u, eye);
                    gcam.view[2] = -f[0]; gcam.view[6] = -f[1]; gcam.view[10] = -f[2]; gcam.view[14] =  gm_v3_dot(f, eye);
                    gcam.view[3] = 0;     gcam.view[7] = 0;     gcam.view[11] = 0;     gcam.view[15] = 1;

                    float fov = 45.0f * (3.14159265f / 180.0f);
                    float aspect = (avail.y > 0) ? (avail.x / avail.y) : 1.0f;
                    float near_p = 0.1f, far_p = 1000.0f;
                    float t = tanf(fov * 0.5f);
                    memset(gcam.proj, 0, sizeof(gcam.proj));
                    gcam.proj[0]  = 1.0f / (aspect * t);
                    gcam.proj[5]  = 1.0f / t;
                    gcam.proj[10] = -(far_p + near_p) / (far_p - near_p);
                    gcam.proj[11] = -1.0f;
                    gcam.proj[14] = -(2.0f * far_p * near_p) / (far_p - near_p);

                    gm_v3_copy(gcam.eye, eye);
                }
                gcam.viewport_size[0]   = avail.x;
                gcam.viewport_size[1]   = avail.y;
                gcam.viewport_origin[0] = screen_pos.x;
                gcam.viewport_origin[1] = screen_pos.y;

                float scale_factor = jce_editor_prefs_gizmo_scale();

                int sel_count = 0;
                const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
                bool multi_select = sel_count > 1;

                float gizmo_pos[3] = {
                    xform->data.transform.pos[0],
                    xform->data.transform.pos[1],
                    xform->data.transform.pos[2]
                };
                float gizmo_rot[3] = {
                    xform->data.transform.rot[0],
                    xform->data.transform.rot[1],
                    xform->data.transform.rot[2]
                };
                float gizmo_scale[3] = {
                    xform->data.transform.scale[0],
                    xform->data.transform.scale[1],
                    xform->data.transform.scale[2]
                };

                if (multi_select) {
                    float sum_pos[3] = {0.0f, 0.0f, 0.0f};
                    int valid_xforms = 0;
                    for (int i = 0; i < sel_count; i++) {
                        int other_count = 0;
                        JceComponentInfo *other_comps = jce_state_get_entity_components(sel_ids[i], &other_count);
                        JceComponentInfo *other_xform = find_transform_component(other_comps, other_count);
                        if (!other_xform) continue;
                        sum_pos[0] += other_xform->data.transform.pos[0];
                        sum_pos[1] += other_xform->data.transform.pos[1];
                        sum_pos[2] += other_xform->data.transform.pos[2];
                        valid_xforms++;
                    }
                    if (valid_xforms > 0) {
                        gizmo_pos[0] = sum_pos[0] / (float)valid_xforms;
                        gizmo_pos[1] = sum_pos[1] / (float)valid_xforms;
                        gizmo_pos[2] = sum_pos[2] / (float)valid_xforms;
                    } else {
                        multi_select = false;
                    }
                }

                float pos_before[3] = { gizmo_pos[0], gizmo_pos[1], gizmo_pos[2] };
                float rot_before[3] = { gizmo_rot[0], gizmo_rot[1], gizmo_rot[2] };
                float scale_before[3] = { gizmo_scale[0], gizmo_scale[1], gizmo_scale[2] };

                /* Use raw drag values while dragging so snap feedback does not
                 * reset per-frame deltas and stall movement. */
                float gizmo_raw_pos[3] = { gizmo_pos[0], gizmo_pos[1], gizmo_pos[2] };
                float gizmo_raw_rot[3] = { gizmo_rot[0], gizmo_rot[1], gizmo_rot[2] };
                float gizmo_raw_scale[3] = {
                    gizmo_scale[0], gizmo_scale[1], gizmo_scale[2]
                };
                bool gizmo_dragging_before = jce_gizmo_is_active();
                if (gizmo_dragging_before && s_gizmo_raw_dragging) {
                    memcpy(gizmo_raw_pos, s_gizmo_raw_pos, sizeof(gizmo_raw_pos));
                    memcpy(gizmo_raw_rot, s_gizmo_raw_rot, sizeof(gizmo_raw_rot));
                    memcpy(gizmo_raw_scale, s_gizmo_raw_scale, sizeof(gizmo_raw_scale));
                }

                JceGizmoMode active_gm = jce_state_get_gizmo_mode();
                jce_gizmo_update(&gcam,
                                 (int)active_gm,
                                 (int)jce_state_get_gizmo_space(),
                                 scale_factor,
                                 gizmo_raw_pos,
                                 gizmo_raw_rot,
                                 gizmo_raw_scale);

                bool gizmo_dragging_after = jce_gizmo_is_active();
                if (!gizmo_dragging_before && gizmo_dragging_after && !s_gizmo_history_batch_open) {
                    jce_state_begin_batch_edit();
                    s_gizmo_history_batch_open = true;
                }
                if (gizmo_dragging_before && !gizmo_dragging_after && s_gizmo_history_batch_open) {
                    jce_state_end_batch_edit();
                    s_gizmo_history_batch_open = false;
                }
                if (gizmo_dragging_after) {
                    s_gizmo_raw_dragging = true;
                    memcpy(s_gizmo_raw_pos, gizmo_raw_pos, sizeof(s_gizmo_raw_pos));
                    memcpy(s_gizmo_raw_rot, gizmo_raw_rot, sizeof(s_gizmo_raw_rot));
                    memcpy(s_gizmo_raw_scale, gizmo_raw_scale, sizeof(s_gizmo_raw_scale));
                } else {
                    s_gizmo_raw_dragging = false;
                }

                memcpy(gizmo_pos, gizmo_raw_pos, sizeof(gizmo_pos));
                memcpy(gizmo_rot, gizmo_raw_rot, sizeof(gizmo_rot));
                memcpy(gizmo_scale, gizmo_raw_scale, sizeof(gizmo_scale));

                /* Ctrl + TRS snapping (old Java editor behavior). */
                if (ImGui::GetIO().KeyCtrl && gizmo_dragging_after) {
                    const float snap_translate = 0.5f;
                    const float snap_angle     = 15.0f;
                    const float snap_scale     = 0.25f;
                    switch (active_gm) {
                        case JCE_GIZMO_TRANSLATE:
                            gizmo_pos[0] = roundf(gizmo_pos[0] / snap_translate) * snap_translate;
                            gizmo_pos[1] = roundf(gizmo_pos[1] / snap_translate) * snap_translate;
                            gizmo_pos[2] = roundf(gizmo_pos[2] / snap_translate) * snap_translate;
                            break;
                        case JCE_GIZMO_ROTATE:
                            gizmo_rot[0] = roundf(gizmo_rot[0] / snap_angle) * snap_angle;
                            gizmo_rot[1] = roundf(gizmo_rot[1] / snap_angle) * snap_angle;
                            gizmo_rot[2] = roundf(gizmo_rot[2] / snap_angle) * snap_angle;
                            break;
                        case JCE_GIZMO_SCALE:
                            gizmo_scale[0] = roundf(gizmo_scale[0] / snap_scale) * snap_scale;
                            gizmo_scale[1] = roundf(gizmo_scale[1] / snap_scale) * snap_scale;
                            gizmo_scale[2] = roundf(gizmo_scale[2] / snap_scale) * snap_scale;
                            break;
                        default: break;
                    }
                }

                float dpos[3] = {
                    gizmo_pos[0] - pos_before[0],
                    gizmo_pos[1] - pos_before[1],
                    gizmo_pos[2] - pos_before[2]
                };
                float drot[3] = {
                    gizmo_rot[0] - rot_before[0],
                    gizmo_rot[1] - rot_before[1],
                    gizmo_rot[2] - rot_before[2]
                };
                float dscale[3] = {
                    gizmo_scale[0] - scale_before[0],
                    gizmo_scale[1] - scale_before[1],
                    gizmo_scale[2] - scale_before[2]
                };

                const float eps = 0.0001f;
                bool gizmo_changed = false;
                if (active_gm == JCE_GIZMO_TRANSLATE) {
                    gizmo_changed = (fabsf(dpos[0]) > eps || fabsf(dpos[1]) > eps || fabsf(dpos[2]) > eps);
                } else if (active_gm == JCE_GIZMO_ROTATE) {
                    gizmo_changed = (fabsf(drot[0]) > eps || fabsf(drot[1]) > eps || fabsf(drot[2]) > eps);
                } else if (active_gm == JCE_GIZMO_SCALE) {
                    gizmo_changed = (fabsf(dscale[0]) > eps || fabsf(dscale[1]) > eps || fabsf(dscale[2]) > eps);
                }

                if (gizmo_changed) {
                    if (multi_select) {
                        for (int i = 0; i < sel_count; i++) {
                            int other_count = 0;
                            JceComponentInfo *other_comps = jce_state_get_entity_components(sel_ids[i], &other_count);
                            JceComponentInfo *other_xform = find_transform_component(other_comps, other_count);
                            if (!other_xform) continue;

                            if (active_gm == JCE_GIZMO_TRANSLATE) {
                                other_xform->data.transform.pos[0] += dpos[0];
                                other_xform->data.transform.pos[1] += dpos[1];
                                other_xform->data.transform.pos[2] += dpos[2];
                            } else if (active_gm == JCE_GIZMO_ROTATE) {
                                other_xform->data.transform.rot[0] += drot[0];
                                other_xform->data.transform.rot[1] += drot[1];
                                other_xform->data.transform.rot[2] += drot[2];
                            } else if (active_gm == JCE_GIZMO_SCALE) {
                                other_xform->data.transform.scale[0] += dscale[0];
                                other_xform->data.transform.scale[1] += dscale[1];
                                other_xform->data.transform.scale[2] += dscale[2];

                                if (other_xform->data.transform.scale[0] < 0.001f) other_xform->data.transform.scale[0] = 0.001f;
                                if (other_xform->data.transform.scale[1] < 0.001f) other_xform->data.transform.scale[1] = 0.001f;
                                if (other_xform->data.transform.scale[2] < 0.001f) other_xform->data.transform.scale[2] = 0.001f;
                            }
                        }
                    } else {
                        xform->data.transform.pos[0] = gizmo_pos[0];
                        xform->data.transform.pos[1] = gizmo_pos[1];
                        xform->data.transform.pos[2] = gizmo_pos[2];
                        xform->data.transform.rot[0] = gizmo_rot[0];
                        xform->data.transform.rot[1] = gizmo_rot[1];
                        xform->data.transform.rot[2] = gizmo_rot[2];
                        xform->data.transform.scale[0] = gizmo_scale[0] < 0.001f ? 0.001f : gizmo_scale[0];
                        xform->data.transform.scale[1] = gizmo_scale[1] < 0.001f ? 0.001f : gizmo_scale[1];
                        xform->data.transform.scale[2] = gizmo_scale[2] < 0.001f ? 0.001f : gizmo_scale[2];
                    }

                    jce_editor_inspector_request_sync();
                }

                jce_gizmo_draw(dl, &gcam,
                               (int)jce_state_get_gizmo_mode(),
                               (int)jce_state_get_gizmo_space(),
                               scale_factor,
                               gizmo_pos,
                               gizmo_rot,
                               gizmo_scale);
            }
        }
    }

    if (s_gizmo_history_batch_open && !jce_gizmo_is_active()) {
        jce_state_end_batch_edit();
        s_gizmo_history_batch_open = false;
    }

    /* Keyboard shortcuts for gizmo modes */
    if (ImGui::IsWindowFocused()) {
        if (ImGui::IsKeyPressed(ImGuiKey_W)) jce_state_set_gizmo_mode(JCE_GIZMO_TRANSLATE);
        if (ImGui::IsKeyPressed(ImGuiKey_E)) jce_state_set_gizmo_mode(JCE_GIZMO_ROTATE);
        if (ImGui::IsKeyPressed(ImGuiKey_R)) jce_state_set_gizmo_mode(JCE_GIZMO_SCALE);

        /* F — Focus camera on selected entity */
        if (ImGui::IsKeyPressed(ImGuiKey_F)) {
            uint32_t f_ent = jce_state_get_focused();
            if (f_ent != 0) {
                int fc = 0;
                JceComponentInfo *fcomps = jce_state_get_entity_components(f_ent, &fc);
                for (int fi = 0; fi < fc; fi++) {
                    if (fcomps[fi].type == JCE_COMP_TRANSFORM) {
                        jce_editor_scene_camera_set_target(
                            fcomps[fi].data.transform.pos[0],
                            fcomps[fi].data.transform.pos[1],
                            fcomps[fi].data.transform.pos[2]);
                        break;
                    }
                }
            }
        }

        /* Delete — Delete selected entities */
        if (ImGui::IsKeyPressed(ImGuiKey_Delete)) {
            int dk = 0;
            const uint32_t *dids = jce_state_get_selection(&dk);
            if (dk > 0) {
                uint32_t ids[JCE_MAX_SELECTED];
                int n = dk < JCE_MAX_SELECTED ? dk : JCE_MAX_SELECTED;
                for (int di = 0; di < n; di++)
                    ids[di] = dids[di];
                jce_editor_inspector_request_delete_confirm_many(ids, n);
            }
        }

        /* Ctrl+D — Duplicate selected entities */
        if (ImGui::GetIO().KeyCtrl && ImGui::IsKeyPressed(ImGuiKey_D)) {
            int dk = 0;
            const uint32_t *dids = jce_state_get_selection(&dk);
            if (dk > 0) {
                uint32_t new_ids[JCE_MAX_SELECTED];
                int nc = 0;
                if (dk > 1)
                    jce_state_begin_batch_edit();
                for (int di = 0; di < dk && di < JCE_MAX_SELECTED; di++) {
                    uint32_t dup = jce_state_duplicate_entity(dids[di]);
                    if (dup != 0 && nc < JCE_MAX_SELECTED)
                        new_ids[nc++] = dup;
                }
                if (dk > 1)
                    jce_state_end_batch_edit();
                if (nc > 0) {
                    jce_state_select_entity(new_ids[0], false);
                    for (int di = 1; di < nc; di++)
                        jce_state_select_entity(new_ids[di], true);
                }
                jce_editor_inspector_request_sync();
            }
        }
    }

    /* ── Axis indicator (bottom-left) + View cube (top-right) ───── */
    {
        float view_mat[16], proj_mat[16], eye[3];
        if (jce_editor_scene_get_camera_matrices(view_mat, proj_mat, eye,
                                                  avail.x, avail.y))
        {
            JceGizmoCamera overlay_cam;
            memcpy(overlay_cam.view, view_mat, sizeof(float) * 16);
            memcpy(overlay_cam.proj, proj_mat, sizeof(float) * 16);
            memcpy(overlay_cam.eye,  eye,      sizeof(float) * 3);
            overlay_cam.viewport_size[0]   = avail.x;
            overlay_cam.viewport_size[1]   = avail.y;
            overlay_cam.viewport_origin[0] = screen_pos.x;
            overlay_cam.viewport_origin[1] = screen_pos.y;

            draw_scene_helper_icons(dl, &overlay_cam);

            int axis_click = draw_axis_indicator(dl, screen_pos, avail, view_mat);
            int cube_click = draw_view_cube(dl, screen_pos, avail, view_mat);

            /* Axis indicator click: +X=Right, +Y=Top, +Z=Front, -X=Left, -Y=Bottom, -Z=Back */
            if (axis_click >= 0) {
                static const JceCamPresetView axis_presets[6] = {
                    JCE_CAM_VIEW_RIGHT, JCE_CAM_VIEW_TOP, JCE_CAM_VIEW_FRONT,
                    JCE_CAM_VIEW_LEFT, JCE_CAM_VIEW_BOTTOM, JCE_CAM_VIEW_BACK,
                };
                jce_editor_scene_camera_snap_view(axis_presets[axis_click]);
            }

            /* View cube click: 0=Front, 1=Back, 2=Top, 3=Bottom, 4=Right, 5=Left, -2=Home */
            if (cube_click == -2) {
                jce_editor_scene_camera_reset();
            } else if (cube_click >= 0) {
                static const JceCamPresetView cube_presets[6] = {
                    JCE_CAM_VIEW_FRONT, JCE_CAM_VIEW_BACK,
                    JCE_CAM_VIEW_TOP, JCE_CAM_VIEW_BOTTOM,
                    JCE_CAM_VIEW_RIGHT, JCE_CAM_VIEW_LEFT,
                };
                jce_editor_scene_camera_snap_view(cube_presets[cube_click]);
            }

            /* ── Marquee selection hit-test ──────────────────────── */
            if (s_sel_pending) {
                s_sel_pending = false;
                bool add_mode = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;
                if (!add_mode) jce_state_clear_selection();

                int total = jce_state_get_entity_count();
                for (int mi = 0; mi < total; mi++) {
                    JceEntityInfo *me = jce_state_get_entity_by_index(mi);
                    if (!me || !me->enabled) continue;
                    uint32_t meid = me->id;

                    JceComponentInfo me_comps[JCE_MAX_COMPONENTS];
                    int me_cc = jce_state_get_components(meid, me_comps, JCE_MAX_COMPONENTS);
                    float mwp[3] = {0, 0, 0};
                    float mws[3] = {1, 1, 1};
                    bool has_xf = false;
                    for (int mci = 0; mci < me_cc; mci++) {
                        if (me_comps[mci].type == JCE_COMP_TRANSFORM) {
                            mwp[0] = me_comps[mci].data.transform.pos[0];
                            mwp[1] = me_comps[mci].data.transform.pos[1];
                            mwp[2] = me_comps[mci].data.transform.pos[2];
                            mws[0] = me_comps[mci].data.transform.scale[0];
                            mws[1] = me_comps[mci].data.transform.scale[1];
                            mws[2] = me_comps[mci].data.transform.scale[2];
                            has_xf = true;
                            break;
                        }
                    }
                    if (!has_xf) continue;

                    /* Screen-space AABB overlap test. Using projected bounds is
                     * much more reliable than testing only the object pivot. */
                    float hx = fabsf(mws[0]) * 0.5f;
                    float hy = fabsf(mws[1]) * 0.5f;
                    float hz = fabsf(mws[2]) * 0.5f;
                    if (hx < 0.1f) hx = 0.1f;
                    if (hy < 0.1f) hy = 0.1f;
                    if (hz < 0.1f) hz = 0.1f;

                    static const float corners[8][3] = {
                        {-1,-1,-1}, { 1,-1,-1}, {-1, 1,-1}, { 1, 1,-1},
                        {-1,-1, 1}, { 1,-1, 1}, {-1, 1, 1}, { 1, 1, 1},
                    };

                    float bb_min_x =  1e30f, bb_min_y =  1e30f;
                    float bb_max_x = -1e30f, bb_max_y = -1e30f;
                    int projected = 0;

                    for (int ci = 0; ci < 8; ci++) {
                        float wx = mwp[0] + corners[ci][0] * hx;
                        float wy = mwp[1] + corners[ci][1] * hy;
                        float wz = mwp[2] + corners[ci][2] * hz;

                        float mvx = view_mat[0]*wx + view_mat[4]*wy + view_mat[8] *wz + view_mat[12];
                        float mvy = view_mat[1]*wx + view_mat[5]*wy + view_mat[9] *wz + view_mat[13];
                        float mvz = view_mat[2]*wx + view_mat[6]*wy + view_mat[10]*wz + view_mat[14];
                        float mvw = view_mat[3]*wx + view_mat[7]*wy + view_mat[11]*wz + view_mat[15];

                        float mcx = proj_mat[0]*mvx + proj_mat[4]*mvy + proj_mat[8] *mvz + proj_mat[12]*mvw;
                        float mcy = proj_mat[1]*mvx + proj_mat[5]*mvy + proj_mat[9] *mvz + proj_mat[13]*mvw;
                        float mcw = proj_mat[3]*mvx + proj_mat[7]*mvy + proj_mat[11]*mvz + proj_mat[15]*mvw;
                        if (mcw <= 0.0f) continue;

                        float msx = screen_pos.x + (mcx / mcw + 1.0f) * 0.5f * avail.x;
                        float msy = screen_pos.y + (1.0f - mcy / mcw) * 0.5f * avail.y;
                        if (msx < bb_min_x) bb_min_x = msx;
                        if (msx > bb_max_x) bb_max_x = msx;
                        if (msy < bb_min_y) bb_min_y = msy;
                        if (msy > bb_max_y) bb_max_y = msy;
                        projected++;
                    }

                    if (projected <= 0) continue;

                    if (bb_max_x >= s_sel_rect_min.x && bb_min_x <= s_sel_rect_max.x &&
                        bb_max_y >= s_sel_rect_min.y && bb_min_y <= s_sel_rect_max.y)
                    {
                        jce_state_select_entity(meid, true /* add */);
                    }
                }
                jce_editor_inspector_request_sync();
            }

            /* ── Single-click ray pick ────────────────────────── */
            if (s_sel_click_pending) {
                s_sel_click_pending = false;
                bool add_mode = ImGui::GetIO().KeyCtrl || ImGui::GetIO().KeyShift;

                /* Build camera struct for screen-to-ray conversion. */
                JceGizmoCamera pick_cam;
                memcpy(pick_cam.view, view_mat, sizeof(float) * 16);
                memcpy(pick_cam.proj, proj_mat, sizeof(float) * 16);
                memcpy(pick_cam.eye,  eye,      sizeof(float) * 3);
                pick_cam.viewport_size[0]   = avail.x;
                pick_cam.viewport_size[1]   = avail.y;
                pick_cam.viewport_origin[0] = screen_pos.x;
                pick_cam.viewport_origin[1] = screen_pos.y;

                float ray_o[3], ray_d[3];
                gm_screen_to_ray(&pick_cam, s_sel_click_pos.x,
                                 s_sel_click_pos.y, ray_o, ray_d);

                uint32_t best_id = 0;
                float    best_t  = 1e30f;

                int total = jce_state_get_entity_count();
                for (int pi = 0; pi < total; pi++) {
                    JceEntityInfo *pe = jce_state_get_entity_by_index(pi);
                    if (!pe || !pe->enabled) continue;

                    JceComponentInfo pc[JCE_MAX_COMPONENTS];
                    int pcc = jce_state_get_components(pe->id, pc,
                                                       JCE_MAX_COMPONENTS);
                    float pos[3] = {0,0,0}, scl[3] = {1,1,1};
                    bool has_xf = false;
                    for (int ci = 0; ci < pcc; ci++) {
                        if (pc[ci].type == JCE_COMP_TRANSFORM) {
                            memcpy(pos, pc[ci].data.transform.pos,
                                   sizeof(float) * 3);
                            memcpy(scl, pc[ci].data.transform.scale,
                                   sizeof(float) * 3);
                            has_xf = true;
                            break;
                        }
                    }
                    if (!has_xf) continue;

                    /* AABB: half-extents from scale, min 0.1 each. */
                    float hx = fabsf(scl[0]) * 0.5f;
                    float hy = fabsf(scl[1]) * 0.5f;
                    float hz = fabsf(scl[2]) * 0.5f;
                    if (hx < 0.1f) hx = 0.1f;
                    if (hy < 0.1f) hy = 0.1f;
                    if (hz < 0.1f) hz = 0.1f;

                    float bmin[3] = { pos[0]-hx, pos[1]-hy, pos[2]-hz };
                    float bmax[3] = { pos[0]+hx, pos[1]+hy, pos[2]+hz };

                    float t;
                    if (ray_aabb_intersect(ray_o, ray_d, bmin, bmax, &t)) {
                        if (t < best_t) {
                            best_t  = t;
                            best_id = pe->id;
                        }
                    }
                }

                if (best_id != 0) {
                    jce_state_select_entity(best_id, add_mode);
                } else if (!add_mode) {
                    jce_state_clear_selection();
                }
                jce_editor_inspector_request_sync();
            }

            /* ── Orange selection outline for all selected entities ── */
            {
                int sel_count = 0;
                const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
                for (int si = 0; si < sel_count; si++) {
                    JceEntityInfo *se = jce_state_get_entity(sel_ids[si]);
                    if (!se || !se->enabled) continue;
                    JceComponentInfo se_comps[JCE_MAX_COMPONENTS];
                    int se_cc = jce_state_get_components(sel_ids[si], se_comps, JCE_MAX_COMPONENTS);
                    float swp[3] = {0, 0, 0};
                    bool has_sxf = false;
                    for (int sci = 0; sci < se_cc; sci++) {
                        if (se_comps[sci].type == JCE_COMP_TRANSFORM) {
                            swp[0] = se_comps[sci].data.transform.pos[0];
                            swp[1] = se_comps[sci].data.transform.pos[1];
                            swp[2] = se_comps[sci].data.transform.pos[2];
                            has_sxf = true;
                            break;
                        }
                    }
                    if (!has_sxf) continue;

                    float svx = view_mat[0]*swp[0] + view_mat[4]*swp[1] + view_mat[8] *swp[2] + view_mat[12];
                    float svy = view_mat[1]*swp[0] + view_mat[5]*swp[1] + view_mat[9] *swp[2] + view_mat[13];
                    float svz = view_mat[2]*swp[0] + view_mat[6]*swp[1] + view_mat[10]*swp[2] + view_mat[14];
                    float svw = view_mat[3]*swp[0] + view_mat[7]*swp[1] + view_mat[11]*swp[2] + view_mat[15];
                    float scx = proj_mat[0]*svx + proj_mat[4]*svy + proj_mat[8] *svz + proj_mat[12]*svw;
                    float scy = proj_mat[1]*svx + proj_mat[5]*svy + proj_mat[9] *svz + proj_mat[13]*svw;
                    float scw = proj_mat[3]*svx + proj_mat[7]*svy + proj_mat[11]*svz + proj_mat[15]*svw;
                    if (scw <= 0.0f) continue;

                    float ssx = screen_pos.x + (scx / scw + 1.0f) * 0.5f * avail.x;
                    float ssy = screen_pos.y + (1.0f - scy / scw) * 0.5f * avail.y;

                    /* Skip if outside viewport bounds. */
                    if (ssx < screen_pos.x || ssx > screen_pos.x + avail.x ||
                        ssy < screen_pos.y || ssy > screen_pos.y + avail.y)
                        continue;

                    /* Draw orange diamond marker around entity position. */
                    float mr = 14.0f;
                    dl->AddQuadFilled(
                        ImVec2(ssx,      ssy - mr),
                        ImVec2(ssx + mr, ssy),
                        ImVec2(ssx,      ssy + mr),
                        ImVec2(ssx - mr, ssy),
                        IM_COL32(255, 120, 0, 60));
                    dl->AddQuad(
                        ImVec2(ssx,      ssy - mr),
                        ImVec2(ssx + mr, ssy),
                        ImVec2(ssx,      ssy + mr),
                        ImVec2(ssx - mr, ssy),
                        IM_COL32(255, 120, 0, 230), 2.0f);
                }
            }
        }
    }
}

/* ── Standalone wrapper ───────────────────────────────────────────── */

void jce_editor_panel_scene_view(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_SCENE_VIEW);
    if (!*vis) return;

    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(0, 0));
    char title[256];
    snprintf(title, sizeof(title), "%s###SceneView", jce_editor_i18n("Scene"));
    if (ImGui::Begin(title, vis))
        jce_editor_panel_scene_view_content();
    ImGui::End();
    ImGui::PopStyleVar();
}
