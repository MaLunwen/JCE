/*
 * jce_scene_view_gizmo.cpp  Gizmo interaction + selection box (marquee).
 */

#include "jce_scene_view_internal.h"

/* ── Selection box (marquee) ─────────────────────────────────────── */

void handle_scene_selection_box(const SceneViewCtx *ctx)
{
    if (!has_valid_gizmo_target())
        clear_stale_gizmo_interaction_state();

    if (ctx->viewport_hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
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

/* ── Gizmo overlay (translate/rotate/scale) ──────────────────────── */

void update_and_draw_scene_gizmo(const SceneViewCtx *ctx)
{
    if (jce_editor_prefs_show_gizmos() && jce_state_get_play_state() == JCE_PLAY_STOPPED) {
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
                        ctx->avail.x, ctx->avail.y))
                {
                    float eye[3] = {5.0f, 5.0f, 5.0f};
                    float center[3] = {0.0f, 0.0f, 0.0f};
                    float up[3] = {0.0f, 1.0f, 0.0f};

                    glm_lookat(eye, center, up, (vec4 *)gcam.view);

                    float fov = 45.0f * JCE_DEG2RAD;
                    float aspect = (ctx->avail.y > 0) ? (ctx->avail.x / ctx->avail.y) : 1.0f;
                    float near_p = 0.1f, far_p = 1000.0f;
                    glm_perspective_rh_no(fov, aspect, near_p, far_p, (vec4 *)gcam.proj);

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

                /* Ctrl + TRS snapping.
                 * Use gizmo_dragging_before || gizmo_dragging_after so that
                 * snapping also applies on the release frame (before=true,
                 * after=false), preventing the final value from drifting to
                 * the un-snapped raw position. */
                if (ImGui::GetIO().KeyCtrl && (gizmo_dragging_before || gizmo_dragging_after)) {
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
                                for (int a = 0; a < 3; a++) {
                                    other_xform->data.transform.rot[a] = fmodf(other_xform->data.transform.rot[a], 360.0f);
                                    if (other_xform->data.transform.rot[a] < 0.0f) other_xform->data.transform.rot[a] += 360.0f;
                                }
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
                        for (int a = 0; a < 3; a++) {
                            xform->data.transform.rot[a] = fmodf(xform->data.transform.rot[a], 360.0f);
                            if (xform->data.transform.rot[a] < 0.0f) xform->data.transform.rot[a] += 360.0f;
                        }
                        xform->data.transform.scale[0] = gizmo_scale[0] < 0.001f ? 0.001f : gizmo_scale[0];
                        xform->data.transform.scale[1] = gizmo_scale[1] < 0.001f ? 0.001f : gizmo_scale[1];
                        xform->data.transform.scale[2] = gizmo_scale[2] < 0.001f ? 0.001f : gizmo_scale[2];
                    }

                    jce_editor_inspector_request_sync();
                }

                jce_gizmo_draw(ctx->dl, &gcam,
                               (int)jce_state_get_gizmo_mode(),
                               (int)jce_state_get_gizmo_space(),
                               scale_factor,
                               gizmo_pos,
                               gizmo_rot,
                               gizmo_scale);
            }
        }
    }
}
