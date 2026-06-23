/*
 * jce_scene_view_helpers.cpp  Icons, projections, helper line drawing, entity creation.
 *
 * Reads/writes scene data via the engine ECS API
 * (jce_state_get_scene + jce_scene_get_*).
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
bool  s_gizmo_transaction_open = false;

/* ── Per-entity persistent euler cache (see internal header) ─────── */

static uint32_t  s_euler_cache_entity = 0;
static jce_quat  s_euler_cache_quat   = {0.0f, 0.0f, 0.0f, 1.0f};
static float     s_euler_cache_deg[3] = {0.0f, 0.0f, 0.0f};
static bool      s_euler_cache_valid  = false;

static bool euler_cache_quat_almost_equal(jce_quat a, jce_quat b)
{
    /* Quaternion double-cover: q and -q are the same rotation. */
    float d = a.w*b.w + a.x*b.x + a.y*b.y + a.z*b.z;
    return fabsf(d) > 0.99999f;
}

bool jce_editor_get_cached_euler_deg(uint32_t entity_id, jce_quat current_q,
                                      float out_deg[3])
{
    if (!s_euler_cache_valid) return false;
    if (s_euler_cache_entity != entity_id) return false;
    if (!euler_cache_quat_almost_equal(s_euler_cache_quat, current_q)) return false;
    out_deg[0] = s_euler_cache_deg[0];
    out_deg[1] = s_euler_cache_deg[1];
    out_deg[2] = s_euler_cache_deg[2];
    return true;
}

void jce_editor_set_cached_euler_deg(uint32_t entity_id, jce_quat q,
                                      const float deg[3])
{
    s_euler_cache_entity = entity_id;
    s_euler_cache_quat   = q;
    s_euler_cache_deg[0] = deg[0];
    s_euler_cache_deg[1] = deg[1];
    s_euler_cache_deg[2] = deg[2];
    s_euler_cache_valid  = true;
}

/* ── Gizmo state helpers ─────────────────────────────────────────── */

void clear_stale_gizmo_interaction_state(void)
{
    if (jce_gizmo_is_active() || jce_gizmo_hovered_axis() != JCE_GIZMO_AXIS_NONE)
        jce_gizmo_cancel_interaction();

    if (s_gizmo_transaction_open) {
        jce_state_cancel_transaction();
        s_gizmo_transaction_open = false;
    }

    s_gizmo_raw_dragging = false;
}

bool has_valid_gizmo_target(void)
{
    /* Gizmo is allowed during PLAYING/PAUSED (Unity-parity); the only
       global gate is the Show-Gizmos preference. Keeping the play_state
       gate here used to wipe gizmo interaction state every frame during
       Play (see clear_stale_gizmo_interaction_state callers), which made
       TRS dragging silently fail even though the bar drew normally. */
    if (!jce_editor_prefs_show_gizmos())
        return false;

    uint32_t focused = jce_state_get_focused();
    if (focused == 0 || !jce_state_entity_exists(focused))
        return false;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return false;
    return jce_scene_has_transform(scene, (JceEntity)focused);
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

/* ── Orientation helper basis from a transform's quaternion ──────── */

static void build_helper_basis(const JceTransform *xform,
                               float forward[3],
                               float right[3],
                               float up[3])
{
    /* Convert the stored quaternion back to euler degrees and rebuild
     * the basis with the same YXZ convention used historically by the
     * scene-view helper icons. This keeps icon orientation consistent
     * with what the user sees in the inspector. */
    float pitch = 0.0f, yaw = 0.0f, roll = 0.0f;
    if (xform) {
        jce_vec3 e_rad = jce_q_to_euler(xform->rotation);
        pitch = e_rad.x;
        yaw   = e_rad.y;
        roll  = e_rad.z;
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
                                     const JceTransform *xform,
                                     const JceCameraComponent *camera,
                                     bool selected)
{
    if (!dl || !cam || !xform || !camera) return;

    float pos[3] = { xform->position.x, xform->position.y, xform->position.z };
    float forward[3], right[3], up[3];
    build_helper_basis(xform, forward, right, up);

    float near_d = 0.9f;
    float far_d = 2.6f;
    float fov = camera->fov_deg > 1.0f ? camera->fov_deg : 60.0f;
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
                                    const JceTransform *xform,
                                    JceScene *scene, JceEntity e,
                                    int light_type,
                                    bool selected)
{
    if (!dl || !cam || !xform) return;

    float pos[3] = { xform->position.x, xform->position.y, xform->position.z };
    ImU32 line_col = selected ? IM_COL32(255, 255, 255, 180)
                              : IM_COL32(255, 214, 92, 150);
    ImVec2 pos_s;
    if (!project_helper_point(cam, pos, &pos_s))
        return;

    /* WORLD shine direction = the light component's own direction rotated by the
     * entity (matches the renderer sr_light_world_shine_direction).  The old code
     * used the entity forward axis, which ignored the light's direction field =
     * static/fake.  Icon-sized lengths keep the always-on indicator uncluttered;
     * the detailed, data-accurate gizmo is drawn on selection elsewhere. */
    jce_quat q = jce_q_normalize(xform->rotation);

    if (light_type == 0 || light_type == 2) {
        jce_vec3 cdir, fb;
        float length;
        if (light_type == 0) {
            JceDirectionalLight *L = scene ? jce_scene_get_dir_light(scene, e) : NULL;
            cdir = L ? L->direction : jce_v3(0.0f, -1.0f, 0.0f);
            fb = jce_v3(0.0f, -1.0f, 0.0f); length = 2.2f;
        } else {
            JceSpotLight *L = scene ? jce_scene_get_spot_light(scene, e) : NULL;
            cdir = L ? L->direction : jce_v3(0.0f, 0.0f, -1.0f);
            fb = jce_v3(0.0f, 0.0f, -1.0f); length = 2.4f;
        }
        float l2 = cdir.x * cdir.x + cdir.y * cdir.y + cdir.z * cdir.z;
        jce_vec3 d  = jce_v3_normalize(jce_q_rotate(q, (l2 < 1e-8f) ? fb : cdir));
        jce_vec3 u0 = (fabsf(d.y) > 0.95f) ? jce_v3(1, 0, 0) : jce_v3(0, 1, 0);
        jce_vec3 rt = jce_v3_normalize(jce_v3_cross(d, u0));
        jce_vec3 uv = jce_v3_normalize(jce_v3_cross(rt, d));

        if (light_type == 0) {
            /* Directional: 3 parallel arrows along the SHINE direction. */
            for (int i = -1; i <= 1; i++) {
                float o = (float)i * 0.45f;
                float start[3] = { pos[0] + rt.x * o, pos[1] + rt.y * o, pos[2] + rt.z * o };
                float end[3]   = { start[0] + d.x * length, start[1] + d.y * length,
                                   start[2] + d.z * length };
                ImVec2 a, b;
                if (project_helper_point(cam, start, &a) && project_helper_point(cam, end, &b)) {
                    dl->AddLine(a, b, line_col, 1.7f);
                    ImVec2 di(b.x - a.x, b.y - a.y);
                    float ln = sqrtf(di.x * di.x + di.y * di.y);
                    if (ln > 1.0f) {
                        di.x /= ln; di.y /= ln;
                        ImVec2 n(-di.y, di.x);
                        dl->AddLine(b, ImVec2(b.x - di.x * 7.0f + n.x * 3.0f,
                                              b.y - di.y * 7.0f + n.y * 3.0f), line_col, 1.7f);
                        dl->AddLine(b, ImVec2(b.x - di.x * 7.0f - n.x * 3.0f,
                                              b.y - di.y * 7.0f - n.y * 3.0f), line_col, 1.7f);
                    }
                }
            }
        } else {
            /* Spot: cone along the SHINE direction; half-angle from the outer
             * cone cosine (real data), at an icon-sized length. */
            float cosA = 0.7071f;
            JceSpotLight *L = scene ? jce_scene_get_spot_light(scene, e) : NULL;
            if (L && L->outer_cone_cos > 0.0f && L->outer_cone_cos < 0.9999f)
                cosA = L->outer_cone_cos;
            float r = tanf(acosf(cosA)) * length;
            if (r < 0.05f) r = 0.05f;
            float bc[3] = { pos[0] + d.x * length, pos[1] + d.y * length, pos[2] + d.z * length };
            float corners[4][3];
            for (int i = 0; i < 4; i++) {
                float sx = (i == 0 || i == 3) ? -1.0f : 1.0f;
                float sy = (i < 2) ? -1.0f : 1.0f;
                corners[i][0] = bc[0] + rt.x * r * sx + uv.x * r * sy;
                corners[i][1] = bc[1] + rt.y * r * sx + uv.y * r * sy;
                corners[i][2] = bc[2] + rt.z * r * sx + uv.z * r * sy;
            }
            ImVec2 cs[4]; int pj = 0;
            for (int i = 0; i < 4; i++) pj += project_helper_point(cam, corners[i], &cs[i]) ? 1 : 0;
            if (pj == 4)
                for (int i = 0; i < 4; i++) {
                    dl->AddLine(pos_s, cs[i], line_col, 1.5f);
                    dl->AddLine(cs[i], cs[(i + 1) % 4], line_col, 1.5f);
                }
        }
    } else {
        /* Point: 6-axis spider icon (omnidirectional; range sphere shows on
         * selection in the 3D gizmo). */
        float axes[6][3] = { {1,0,0},{-1,0,0},{0,1,0},{0,-1,0},{0,0,1},{0,0,-1} };
        for (int i = 0; i < 6; i++) {
            float end[3] = { pos[0] + axes[i][0] * 0.9f,
                             pos[1] + axes[i][1] * 0.9f,
                             pos[2] + axes[i][2] * 0.9f };
            ImVec2 ep;
            if (project_helper_point(cam, end, &ep))
                dl->AddLine(pos_s, ep, line_col, 1.4f);
        }
    }
}

/* ── Scene helper icons (cameras & lights) ───────────────────────── */

void draw_scene_helper_icons(ImDrawList *dl, const JceGizmoCamera *cam)
{
    if (!dl || !cam) return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    int total = jce_state_get_entity_count();
    for (int i = 0; i < total; i++) {
        uint32_t id = jce_state_get_entity_id_by_index(i);
        if (id == 0 || !jce_state_entity_exists(id)) continue;
        if (!jce_state_entity_enabled(id)) continue;

        JceEntity e = (JceEntity)id;
        JceTransform *xform = jce_scene_get_transform(scene, e);
        if (!xform) continue;

        bool has_camera = jce_scene_has_camera(scene, e);
        bool has_dir    = jce_scene_has_dir_light(scene, e);
        bool has_point  = jce_scene_has_point_light(scene, e);
        bool has_spot   = jce_scene_has_spot_light(scene, e);
        bool has_light  = has_dir || has_point || has_spot;
        if (!has_camera && !has_light) continue;

        float world[3] = {
            xform->position.x, xform->position.y, xform->position.z
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

        bool selected = jce_state_is_selected(id);
        bool skip_icon = selected && id == jce_state_get_focused()
                      && jce_editor_prefs_show_gizmos();
        ImVec2 center(screen[0], screen[1]);

        int light_type = -1;
        if (has_dir)        light_type = 0;
        else if (has_point) light_type = 1;
        else if (has_spot)  light_type = 2;

        /* Always-on helper LINES (direction / frustum) only when NOT selected:
         * a selected light/camera gets the detailed, data-accurate 3D gizmo from
         * draw_selection_outlines, so drawing both would double up (bloat). The
         * 2D icons below stay always-on. */
        if (has_camera && !selected) {
            JceCameraComponent *camera = jce_scene_get_camera(scene, e);
            draw_camera_helper_lines(dl, cam, xform, camera, selected);
        }
        if (light_type >= 0 && !selected)
            draw_light_helper_lines(dl, cam, xform, scene, e, light_type, selected);

        if (skip_icon) {
            continue;
        }

        if (has_camera && light_type >= 0) {
            draw_camera_scene_icon(dl, ImVec2(center.x - 14.0f, center.y),
                                   16.0f, selected);
            draw_light_scene_icon(dl, ImVec2(center.x + 14.0f, center.y),
                                  16.0f, light_type, selected);
        } else if (has_camera) {
            draw_camera_scene_icon(dl, center, 18.0f, selected);
        } else if (light_type >= 0) {
            draw_light_scene_icon(dl, center, 18.0f, light_type, selected);
        }
    }
}

/* ── Entity creation helpers ─────────────────────────────────────── */

void set_entity_mesh_shape(uint32_t entity_id, int mesh_shape)
{
    if (entity_id == 0) return;
    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    JceMeshRenderer *mr = jce_scene_get_mesh_renderer(scene, (JceEntity)entity_id);
    if (!mr) return;

    mr->mesh_shape = mesh_shape;
}

uint32_t create_default_scene_entity(const char *name,
                                     uint32_t parent_id,
                                     uint64_t extra_comp_flag,
                                     int mesh_shape)
{
    jce_state_begin_batch_edit();

    /* jce_state_create_entity already seeds Transform + EditorMeta. */
    uint32_t id = jce_state_create_entity(name, parent_id);
    if (id != 0 && extra_comp_flag != 0) {
        jce_state_add_component(id, extra_comp_flag);
        if (extra_comp_flag == JCE_COMP_FLAG_MESH_RENDERER)
            set_entity_mesh_shape(id, mesh_shape);
    }

    jce_state_end_batch_edit();
    return id;
}
