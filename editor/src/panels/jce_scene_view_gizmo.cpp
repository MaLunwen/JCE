/*
 * jce_scene_view_gizmo.cpp  Gizmo interaction + selection box (marquee).
 *
 * Reads/writes JceTransform directly from the engine ECS scene.
 */

#include "jce_scene_view_internal.h"

/* ── Selection box (marquee) ─────────────────────────────────────── */

void handle_scene_selection_box(const SceneViewCtx *ctx)
{
    if (!has_valid_gizmo_target())
        clear_stale_gizmo_interaction_state();

    if (jce_scene_view_should_start_selection(
            ctx->viewport_left_clicked,
            ImGui::GetIO().KeyAlt,
            jce_gizmo_is_active(),
            jce_gizmo_hovered_axis() != JCE_GIZMO_AXIS_NONE))
    {
        s_is_selecting = true;
        s_sel_start    = ImGui::GetMousePos();
        s_sel_current  = s_sel_start;

        /* Roll for Easter egg: 4.161014% probability. */
        s_sel_easter = ((float)rand() / (float)RAND_MAX) < 0.04161014f;
        if (s_sel_easter) {
            s_sel_border = IM_COL32(255, 255, 255, 200);
            s_sel_fill   = IM_COL32(100, 150, 255, 40);
            s_sel_inner  = IM_COL32(100, 150, 255, 100);
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
                s_sel_pending  = true;
                s_sel_rect_min = ImVec2(fminf(s_sel_start.x, s_sel_current.x),
                                        fminf(s_sel_start.y, s_sel_current.y));
                s_sel_rect_max = ImVec2(fmaxf(s_sel_start.x, s_sel_current.x),
                                        fmaxf(s_sel_start.y, s_sel_current.y));
            } else {
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

        if ((maxX - minX) > 3.0f || (maxY - minY) > 3.0f) {
            ctx->dl->AddRectFilled(ImVec2(minX, minY), ImVec2(maxX, maxY), s_sel_fill);
            ctx->dl->AddRect(ImVec2(minX + 1, minY + 1), ImVec2(maxX - 1, maxY - 1),
                        s_sel_inner, 0.0f, 0, 1.0f);
            ctx->dl->AddRect(ImVec2(minX, minY), ImVec2(maxX, maxY),
                        s_sel_border, 0.0f, 0, 1.5f);
        }
    }
}

/* ── Helpers for transform read/write via ECS ────────────────────── */

static JceTransform *get_transform_for_id(JceScene *scene, uint32_t id)
{
    if (!scene || id == 0 || !jce_state_entity_exists(id)) return NULL;
    return jce_scene_get_transform(scene, (JceEntity)id);
}

static void set_transform_for_id(JceScene *scene, uint32_t id,
                                 const JceTransform *value)
{
    if (!scene || id == 0 || !value || !jce_state_entity_exists(id))
        return;
    jce_scene_set_transform(scene, (JceEntity)id, value);
}

static void normalize_euler_deg(float rot[3])
{
    for (int a = 0; a < 3; a++) {
        rot[a] = fmodf(rot[a], 360.0f);
        if (rot[a] < 0.0f) rot[a] += 360.0f;
    }
}

static float clamp_transform_scale(float v)
{
    return v < 0.001f ? 0.001f : v;
}

static void apply_2d_constraints(float pos[3], float rot[3], float scale[3],
                                 float lock_pos_z, float lock_rot_x,
                                 float lock_rot_y, float lock_scale_z)
{
    pos[2]   = lock_pos_z;
    rot[0]   = lock_rot_x;
    rot[1]   = lock_rot_y;
    scale[2] = lock_scale_z;
}

static void rotate_relative(float out[3], const float rel[3], jce_quat q)
{
    jce_vec3 r = jce_q_rotate(q, jce_v3(rel[0], rel[1], rel[2]));
    out[0] = r.x;
    out[1] = r.y;
    out[2] = r.z;
}

static void scale_relative_on_axes(float out[3],
                                   const float rel[3],
                                   const float factor[3],
                                   const float ax_x[3],
                                   const float ax_y[3],
                                   const float ax_z[3])
{
    float dx = rel[0]*ax_x[0] + rel[1]*ax_x[1] + rel[2]*ax_x[2];
    float dy = rel[0]*ax_y[0] + rel[1]*ax_y[1] + rel[2]*ax_y[2];
    float dz = rel[0]*ax_z[0] + rel[1]*ax_z[1] + rel[2]*ax_z[2];

    out[0] = ax_x[0] * dx * factor[0]
           + ax_y[0] * dy * factor[1]
           + ax_z[0] * dz * factor[2];
    out[1] = ax_x[1] * dx * factor[0]
           + ax_y[1] * dy * factor[1]
           + ax_z[1] * dz * factor[2];
    out[2] = ax_x[2] * dx * factor[0]
           + ax_y[2] * dy * factor[1]
           + ax_z[2] * dz * factor[2];
}

/* Per-entity persistent euler cache. JceTransform stores rotation as a
 * quaternion, but the gizmo operates in euler degrees. Round-tripping
 * quat→euler→quat every frame collapses rotations whenever pitch crosses
 * the YXZ gimbal-lock branch at ±90° — X drag past 90° would suddenly
 * push 180° into Y/Z values. To avoid this we keep the editor's own
 * authoritative euler for the focused entity, only re-decomposing from
 * the quaternion when the transform was modified externally (inspector,
 * undo, scene reload, focus change). */

/* ── Gizmo overlay (translate/rotate/scale) ──────────────────────── */

void update_and_draw_scene_gizmo(const SceneViewCtx *ctx)
{
    /* Gizmo is allowed in any play state (Unity-parity). Transform
       edits during PLAYING/PAUSED apply to the live scene; a future
       snapshot/rollback pass should snapshot transforms on Play and
       restore on Stop so design-time state isn't permanently polluted.
       For now the only gate is the global Show-Gizmos preference. */
    if (!jce_editor_prefs_show_gizmos())
        return;

    uint32_t focused = jce_state_get_focused();
    if (focused == 0) return;

    JceScene *scene = jce_state_get_scene();
    if (!scene) return;

    if (jce_gizmo_is_active() && ImGui::IsKeyPressed(ImGuiKey_Escape, false)) {
        jce_gizmo_cancel_interaction();
        if (s_gizmo_transaction_open) {
            jce_state_cancel_transaction();
            s_gizmo_transaction_open = false;
        }
        s_gizmo_raw_dragging = false;
        jce_editor_inspector_request_sync();
        return;
    }

    JceTransform *xform = get_transform_for_id(scene, focused);
    if (!xform) return;

    JceGizmoCamera gcam;
    memset(&gcam, 0, sizeof(gcam));

    if (!jce_editor_scene_get_camera_matrices(
            gcam.view, gcam.proj, gcam.eye,
            ctx->avail.x, ctx->avail.y))
    {
        float eye[3] = {5.0f, 5.0f, 5.0f};
        float center[3] = {0.0f, 0.0f, 0.0f};
        float up[3] = {0.0f, 1.0f, 0.0f};

        jce_mat4 view_m = jce_m4_look_at(jce_v3(eye[0], eye[1], eye[2]),
                                         jce_v3(center[0], center[1], center[2]),
                                         jce_v3(up[0], up[1], up[2]));
        memcpy(gcam.view, &view_m, sizeof(gcam.view));

        float fov = 45.0f * JCE_DEG2RAD;
        float aspect = (ctx->avail.y > 0) ? (ctx->avail.x / ctx->avail.y) : 1.0f;
        float near_p = 0.1f, far_p = 1000.0f;
        jce_mat4 proj_m = jce_m4_perspective(fov, aspect, near_p, far_p, true);
        memcpy(gcam.proj, &proj_m, sizeof(gcam.proj));

        gm_v3_copy(gcam.eye, eye);
    }
    gcam.viewport_size[0]   = ctx->avail.x;
    gcam.viewport_size[1]   = ctx->avail.y;
    gcam.viewport_origin[0] = ctx->screen_pos.x;
    gcam.viewport_origin[1] = ctx->screen_pos.y;

    float scale_factor = jce_editor_prefs_gizmo_scale();

    int sel_count = 0;
    const uint32_t *sel_ids = jce_state_get_selection(&sel_count);
    bool multi_select = sel_count > 1;

    /* Read focused transform into euler-degree working copies. */
    float gizmo_pos[3] = {
        xform->position.x, xform->position.y, xform->position.z
    };
    float gizmo_rot[3];
    if (!jce_editor_get_cached_euler_deg(focused, xform->rotation, gizmo_rot))
        editor_q_to_euler_deg(xform->rotation, gizmo_rot);
    float gizmo_scale[3] = {
        xform->scale.x, xform->scale.y, xform->scale.z
    };

    if (multi_select) {
        float min_pos[3] = {0.0f, 0.0f, 0.0f};
        float max_pos[3] = {0.0f, 0.0f, 0.0f};
        int valid_xforms = 0;
        for (int i = 0; i < sel_count; i++) {
            JceTransform *other = get_transform_for_id(scene, sel_ids[i]);
            if (!other) continue;
            float p[3] = { other->position.x, other->position.y, other->position.z };
            if (valid_xforms == 0) {
                memcpy(min_pos, p, sizeof(min_pos));
                memcpy(max_pos, p, sizeof(max_pos));
            } else {
                for (int a = 0; a < 3; a++) {
                    if (p[a] < min_pos[a]) min_pos[a] = p[a];
                    if (p[a] > max_pos[a]) max_pos[a] = p[a];
                }
            }
            valid_xforms++;
        }
        if (valid_xforms <= 0) {
            multi_select = false;
        } else if (jce_state_get_gizmo_pivot() == JCE_GIZMO_CENTER) {
            gizmo_pos[0] = (min_pos[0] + max_pos[0]) * 0.5f;
            gizmo_pos[1] = (min_pos[1] + max_pos[1]) * 0.5f;
            gizmo_pos[2] = (min_pos[2] + max_pos[2]) * 0.5f;
        }
    }

    float pos_before[3]   = { gizmo_pos[0], gizmo_pos[1], gizmo_pos[2] };
    float rot_before[3]   = { gizmo_rot[0], gizmo_rot[1], gizmo_rot[2] };
    float scale_before[3] = { gizmo_scale[0], gizmo_scale[1], gizmo_scale[2] };

    float gizmo_raw_pos[3]   = { gizmo_pos[0],   gizmo_pos[1],   gizmo_pos[2]   };
    float gizmo_raw_rot[3]   = { gizmo_rot[0],   gizmo_rot[1],   gizmo_rot[2]   };
    float gizmo_raw_scale[3] = { gizmo_scale[0], gizmo_scale[1], gizmo_scale[2] };

    bool gizmo_dragging_before = jce_gizmo_is_active();
    if (gizmo_dragging_before && s_gizmo_raw_dragging) {
        memcpy(gizmo_raw_pos,   s_gizmo_raw_pos,   sizeof(gizmo_raw_pos));
        memcpy(gizmo_raw_rot,   s_gizmo_raw_rot,   sizeof(gizmo_raw_rot));
        memcpy(gizmo_raw_scale, s_gizmo_raw_scale, sizeof(gizmo_raw_scale));
    }

    JceGizmoMode active_gm = jce_state_get_gizmo_mode();
    bool s_view_2d = jce_state_get_2d_mode();
    jce_gizmo_set_dimension(s_view_2d
        ? JCE_GIZMO_DIMENSION_2D
        : JCE_GIZMO_DIMENSION_3D);

    /* Snapshot pre-drag values so we can lock Z in 2D mode.  In 2D the
     * gizmo only operates in the XY plane: Z translation is locked, and
     * rotation is restricted to the Z axis. */
    float pre_pos_z   = gizmo_raw_pos[2];
    float pre_rot_x   = gizmo_raw_rot[0];
    float pre_rot_y   = gizmo_raw_rot[1];
    float pre_scale_z = gizmo_raw_scale[2];
    jce_gizmo_update(&gcam,
                     (int)active_gm,
                     (int)jce_state_get_gizmo_space(),
                     scale_factor,
                     gizmo_raw_pos,
                     gizmo_raw_rot,
                     gizmo_raw_scale);
    if (s_view_2d) {
        apply_2d_constraints(gizmo_raw_pos, gizmo_raw_rot, gizmo_raw_scale,
                             pre_pos_z, pre_rot_x, pre_rot_y, pre_scale_z);
    }

    bool gizmo_dragging_after = jce_gizmo_is_active();
    bool gizmo_drag_started = !gizmo_dragging_before && gizmo_dragging_after;
    bool gizmo_drag_ended   = gizmo_dragging_before && !gizmo_dragging_after;
    if (gizmo_drag_started && !s_gizmo_transaction_open)
        s_gizmo_transaction_open = jce_state_begin_transaction("gizmo-transform");

    if (gizmo_dragging_after) {
        s_gizmo_raw_dragging = true;
        memcpy(s_gizmo_raw_pos,   gizmo_raw_pos,   sizeof(s_gizmo_raw_pos));
        memcpy(s_gizmo_raw_rot,   gizmo_raw_rot,   sizeof(s_gizmo_raw_rot));
        memcpy(s_gizmo_raw_scale, gizmo_raw_scale, sizeof(s_gizmo_raw_scale));
    } else {
        s_gizmo_raw_dragging = false;
    }

    memcpy(gizmo_pos,   gizmo_raw_pos,   sizeof(gizmo_pos));
    memcpy(gizmo_rot,   gizmo_raw_rot,   sizeof(gizmo_rot));
    memcpy(gizmo_scale, gizmo_raw_scale, sizeof(gizmo_scale));

    /* Ctrl + TRS snapping.
     * Use gizmo_dragging_before || gizmo_dragging_after so that snapping
     * also applies on the release frame (before=true, after=false),
     * preventing the final value from drifting to the un-snapped raw
     * position. */
    if (ImGui::GetIO().KeyCtrl && (gizmo_dragging_before || gizmo_dragging_after)) {
        const float snap_translate = jce_state_get_gizmo_snap_translate();
        const float snap_angle     = jce_state_get_gizmo_snap_rotate();
        const float snap_scale     = jce_state_get_gizmo_snap_scale();
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
    if (s_view_2d) {
        apply_2d_constraints(gizmo_pos, gizmo_rot, gizmo_scale,
                             pre_pos_z, pre_rot_x, pre_rot_y, pre_scale_z);
    }

    float dpos[3]   = { gizmo_pos[0]   - pos_before[0],
                        gizmo_pos[1]   - pos_before[1],
                        gizmo_pos[2]   - pos_before[2] };
    float drot[3]   = { gizmo_rot[0]   - rot_before[0],
                        gizmo_rot[1]   - rot_before[1],
                        gizmo_rot[2]   - rot_before[2] };
    float dscale[3] = { gizmo_scale[0] - scale_before[0],
                        gizmo_scale[1] - scale_before[1],
                        gizmo_scale[2] - scale_before[2] };

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
            float ax_x[3], ax_y[3], ax_z[3];
            jce_gizmo_get_axes(ax_x, ax_y, ax_z);

            float scale_ratio[3] = {1.0f, 1.0f, 1.0f};
            if (active_gm == JCE_GIZMO_SCALE) {
                for (int a = 0; a < 3; a++) {
                    if (fabsf(scale_before[a]) > eps)
                        scale_ratio[a] = gizmo_scale[a] / scale_before[a];
                    else
                        scale_ratio[a] = 1.0f + dscale[a];
                    if (scale_ratio[a] < 0.001f)
                        scale_ratio[a] = 0.001f;
                }
                if (s_view_2d)
                    scale_ratio[2] = 1.0f;
            }

            jce_quat delta_rot = editor_q_from_euler_deg(drot);

            for (int i = 0; i < sel_count; i++) {
                JceTransform *other = get_transform_for_id(scene, sel_ids[i]);
                if (!other) continue;
                JceTransform next = *other;

                if (active_gm == JCE_GIZMO_TRANSLATE) {
                    next.position.x += dpos[0];
                    next.position.y += dpos[1];
                    next.position.z += dpos[2];
                    if (s_view_2d)
                        next.position.z = other->position.z;
                } else if (active_gm == JCE_GIZMO_ROTATE) {
                    float rel[3] = {
                        other->position.x - pos_before[0],
                        other->position.y - pos_before[1],
                        other->position.z - pos_before[2],
                    };
                    float rel_rot[3];
                    rotate_relative(rel_rot, rel, delta_rot);
                    next.position.x = pos_before[0] + rel_rot[0];
                    next.position.y = pos_before[1] + rel_rot[1];
                    next.position.z = pos_before[2] + rel_rot[2];
                    if (s_view_2d)
                        next.position.z = other->position.z;

                    /* Decompose-add-recompose to apply euler delta consistently
                     * with the focused entity's gizmo handle. */
                    float other_rot[3];
                    editor_q_to_euler_deg(other->rotation, other_rot);
                    other_rot[0] += drot[0];
                    other_rot[1] += drot[1];
                    other_rot[2] += drot[2];
                    normalize_euler_deg(other_rot);
                    next.rotation = editor_q_from_euler_deg(other_rot);
                    if (sel_ids[i] == focused)
                        jce_editor_set_cached_euler_deg(focused, next.rotation, other_rot);
                } else if (active_gm == JCE_GIZMO_SCALE) {
                    float rel[3] = {
                        other->position.x - pos_before[0],
                        other->position.y - pos_before[1],
                        other->position.z - pos_before[2],
                    };
                    float rel_scaled[3];
                    scale_relative_on_axes(rel_scaled, rel, scale_ratio, ax_x, ax_y, ax_z);
                    next.position.x = pos_before[0] + rel_scaled[0];
                    next.position.y = pos_before[1] + rel_scaled[1];
                    next.position.z = pos_before[2] + rel_scaled[2];
                    if (s_view_2d)
                        next.position.z = other->position.z;

                    next.scale.x = clamp_transform_scale(other->scale.x * scale_ratio[0]);
                    next.scale.y = clamp_transform_scale(other->scale.y * scale_ratio[1]);
                    next.scale.z = s_view_2d
                                 ? other->scale.z
                                 : clamp_transform_scale(other->scale.z * scale_ratio[2]);
                }
                set_transform_for_id(scene, sel_ids[i], &next);
            }
        } else {
            JceTransform next = *xform;
            next.position.x = gizmo_pos[0];
            next.position.y = gizmo_pos[1];
            next.position.z = gizmo_pos[2];

            normalize_euler_deg(gizmo_rot);
            next.rotation = editor_q_from_euler_deg(gizmo_rot);
            jce_editor_set_cached_euler_deg(focused, next.rotation, gizmo_rot);

            next.scale.x = clamp_transform_scale(gizmo_scale[0]);
            next.scale.y = clamp_transform_scale(gizmo_scale[1]);
            next.scale.z = clamp_transform_scale(gizmo_scale[2]);
            set_transform_for_id(scene, focused, &next);
        }

        jce_editor_inspector_request_sync();
    }

    if (gizmo_drag_ended && s_gizmo_transaction_open) {
        jce_state_commit_transaction();
        s_gizmo_transaction_open = false;
    }

    jce_gizmo_draw(ctx->dl, &gcam,
                   (int)jce_state_get_gizmo_mode(),
                   (int)jce_state_get_gizmo_space(),
                   scale_factor,
                   gizmo_pos,
                   gizmo_rot,
                   gizmo_scale);
}
