/*
 * jce_scene_view_helpers.cpp  Icons, projections, helper line drawing, entity creation.
 */

#include "jce_scene_view_internal.h"

/* ── State instances (shared via extern in internal header) ──────── */

bool   s_is_selecting   = false;
ImVec2 s_sel_start      = ImVec2(0, 0);
ImVec2 s_sel_current    = ImVec2(0, 0);
bool   s_sel_easter     = false;
ImU32  s_sel_border     = 0;
ImU32  s_sel_fill       = 0;
ImU32  s_sel_inner      = 0;

bool   s_sel_pending    = false;
ImVec2 s_sel_rect_min   = ImVec2(0, 0);
ImVec2 s_sel_rect_max   = ImVec2(0, 0);

bool   s_sel_click_pending = false;
ImVec2 s_sel_click_pos     = ImVec2(0, 0);

bool  s_gizmo_raw_dragging = false;
float s_gizmo_raw_pos[3]   = {0.0f, 0.0f, 0.0f};
float s_gizmo_raw_rot[3]   = {0.0f, 0.0f, 0.0f};
float s_gizmo_raw_scale[3] = {1.0f, 1.0f, 1.0f};
bool  s_gizmo_history_batch_open = false;

/* ── Gizmo state helpers ─────────────────────────────────────────── */

void clear_stale_gizmo_interaction_state(void)
{
    if (jce_gizmo_is_active() || jce_gizmo_hovered_axis() != JCE_GIZMO_AXIS_NONE)
        jce_gizmo_cancel_interaction();

    if (s_gizmo_history_batch_open) {
        jce_state_end_batch_edit();
        s_gizmo_history_batch_open = false;
    }

    s_gizmo_raw_dragging = false;
}

bool has_valid_gizmo_target(void)
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

/* ── Component lookup ────────────────────────────────────────────── */

JceComponentInfo *find_transform_component(JceComponentInfo *comps, int comp_count)
{
    if (!comps || comp_count <= 0) return NULL;
    for (int i = 0; i < comp_count; i++) {
        if (comps[i].type == JCE_COMP_TRANSFORM)
            return &comps[i];
    }
    return NULL;
}

JceComponentInfo *find_component_by_type(JceComponentInfo *comps,
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

/* ── Camera scene icon ───────────────────────────────────────────── */

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

/* ── Light scene icon ────────────────────────────────────────────── */

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

/* ── Orientation helper basis from euler angles ──────────────────── */

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
    float base_right[3];
    gm_v3_cross(base_right, world_up, forward);
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

    float base_up[3];
    gm_v3_cross(base_up, forward, base_right);

    right[0] = base_right[0] * cr + base_up[0] * sr;
    right[1] = base_right[1] * cr + base_up[1] * sr;
    right[2] = base_right[2] * cr + base_up[2] * sr;

    gm_v3_cross(up, right, forward);
}

/* ── Project world point to screen ───────────────────────────────── */

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

/* ── Camera helper lines (frustum visualization) ─────────────────── */

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

/* ── Light helper lines ──────────────────────────────────────────── */

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

/* ── Scene helper icons (cameras & lights) ───────────────────────── */

void draw_scene_helper_icons(ImDrawList *dl, const JceGizmoCamera *cam)
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

/* ── Entity creation helpers ─────────────────────────────────────── */

void set_entity_mesh_shape(uint32_t entity_id, int mesh_shape)
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

uint32_t create_default_scene_entity(const char *name,
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
