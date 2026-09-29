/*
 * jce_panel_inspector_transform.cpp
 * Transform component inspector drawer.
 */

#include "jce_panel_inspector_common.h"

static bool vec3_changed(const float a[3], const float b[3])
{
    return a[0] != b[0] || a[1] != b[1] || a[2] != b[2];
}

static void begin_transform_write_if_needed(bool *opened)
{
    *opened = !s_insp_batch_open;
    if (*opened)
        jce_state_begin_batch_edit();
}

static void end_transform_write_if_needed(bool opened)
{
    if (opened)
        jce_state_end_batch_edit();
}

static void invalidate_scene_world_cache(void)
{
    JceScene *scene = jce_state_get_scene();
    if (scene)
        jce_scene_invalidate_world_cache(scene);
}

void draw_comp_transform(uint32_t entity_id, JceTransform *t)
{
    float pos[3] = {t->position.x, t->position.y, t->position.z};
    float scl[3] = {t->scale.x,    t->scale.y,    t->scale.z};
    float rot[3];
    if (!jce_editor_get_cached_euler_deg(entity_id, t->rotation, rot))
        editor_q_to_euler_deg(t->rotation, rot);

    ImGui::Text("%s", jce_editor_i18n("transform.position"));
    ImGui::SameLine(80);
    float pos_in[3] = {pos[0], pos[1], pos[2]};
    draw_vec3_control("Position", pos);
    if (vec3_changed(pos, pos_in)) {
        bool opened = false;
        begin_transform_write_if_needed(&opened);
        t->position.x = pos[0]; t->position.y = pos[1]; t->position.z = pos[2];
        invalidate_scene_world_cache();
        end_transform_write_if_needed(opened);
    }

    /* WORLD POSITION -- shown only for a PARENTED entity, because that is the
     * only case where it differs from the row above and the only case where
     * the row above is misleading on its own.  A trigger volume parented to a
     * door at x=4 and authored at local x=0 reads "0, 0, 0" here, and the
     * author has no way to tell that from an object genuinely at the origin.
     *
     * EDITABLE, not a readout.  UE shows both and lets you type into either;
     * "put it where the door is" was not expressible in this editor at all,
     * and a number you can only look at does not make it so.  The rotation is
     * carried through unchanged so typing a position cannot turn the object.
     *
     * A root entity deliberately shows nothing: the row above IS its world
     * position, and a second identical row would teach an author that the two
     * are different things when they are not. */
    JceScene *wscene = jce_state_get_scene();
    const JceEntity we = jce_state_to_ecs_entity(entity_id);
    if (wscene && jce_scene_get_parent(wscene, we) != JCE_ENTITY_INVALID) {
        jce_vec3 wp;
        jce_quat wr;
        if (jce_scene_get_world_pose(wscene, we, &wp, &wr, nullptr)) {
            float wpos[3] = {wp.x, wp.y, wp.z};
            const float wpos_in[3] = {wpos[0], wpos[1], wpos[2]};
            ImGui::Text("%s", jce_editor_i18n("transform.worldPosition"));
            ImGui::SameLine(80);
            draw_vec3_control("WorldPosition", wpos);
            if (vec3_changed(wpos, wpos_in)) {
                bool opened = false;
                begin_transform_write_if_needed(&opened);
                /* Solves the local TRS under whatever parent this entity has;
                 * the same call the physics write-back uses, so the Inspector
                 * and the simulation cannot disagree about what world means. */
                jce_scene_set_world_pose(wscene, we,
                                         jce_v3(wpos[0], wpos[1], wpos[2]), wr);
                invalidate_scene_world_cache();
                end_transform_write_if_needed(opened);
            }
        }
    }

    ImGui::Text("%s", jce_editor_i18n("transform.rotation"));
    ImGui::SameLine(80);
    float rot_in[3] = {rot[0], rot[1], rot[2]};
    draw_vec3_control("Rotation", rot, 1.0f);
    for (int a = 0; a < 3; a++) {
        rot[a] = fmodf(rot[a], 360.0f);
        if (rot[a] < 0.0f) rot[a] += 360.0f;
    }
    if (vec3_changed(rot, rot_in)) {
        bool opened = false;
        begin_transform_write_if_needed(&opened);
        t->rotation = editor_q_from_euler_deg(rot);
        jce_editor_set_cached_euler_deg(entity_id, t->rotation, rot);
        invalidate_scene_world_cache();
        end_transform_write_if_needed(opened);
    }

    ImGui::Text("%s", jce_editor_i18n("transform.scale"));
    ImGui::SameLine(80);
    float scl_in[3] = {scl[0], scl[1], scl[2]};
    draw_vec3_control("Scale", scl, 0.01f, 1.0f);
    if (vec3_changed(scl, scl_in)) {
        bool opened = false;
        begin_transform_write_if_needed(&opened);
        t->scale.x = scl[0] < 0.001f ? 0.001f : scl[0];
        t->scale.y = scl[1] < 0.001f ? 0.001f : scl[1];
        t->scale.z = scl[2] < 0.001f ? 0.001f : scl[2];
        invalidate_scene_world_cache();
        end_transform_write_if_needed(opened);
    }
}

void draw_comp_pivot(JceScene *scene, JceEntity e, JcePivotComponent *p)
{
    if (!scene || e == JCE_ENTITY_INVALID || !p)
        return;

    float local_pos[3] = {
        p->local_position.x, p->local_position.y, p->local_position.z
    };
    float local_in[3] = { local_pos[0], local_pos[1], local_pos[2] };

    ImGui::Text("%s", jce_editor_i18n("pivot.localPosition"));
    ImGui::SameLine(110);
    draw_vec3_control("PivotLocal", local_pos);
    if (vec3_changed(local_pos, local_in)) {
        bool opened = false;
        begin_transform_write_if_needed(&opened);
        jce_scene_set_pivot_local_position_preserve_model(
            scene, e, jce_v3(local_pos[0], local_pos[1], local_pos[2]));
        end_transform_write_if_needed(opened);
    }

    float pivot_rot[3];
    if (p->local_rotation.x == 0.0f && p->local_rotation.y == 0.0f &&
        p->local_rotation.z == 0.0f && p->local_rotation.w == 0.0f) {
        p->local_rotation = jce_q_identity();
    }
    editor_q_to_euler_deg(p->local_rotation, pivot_rot);
    float rot_in[3] = { pivot_rot[0], pivot_rot[1], pivot_rot[2] };

    ImGui::Text("%s", jce_editor_i18n("pivot.orientation"));
    ImGui::SameLine(110);
    draw_vec3_control("PivotOrientation", pivot_rot, 1.0f);
    for (int a = 0; a < 3; a++) {
        pivot_rot[a] = fmodf(pivot_rot[a], 360.0f);
        if (pivot_rot[a] < 0.0f) pivot_rot[a] += 360.0f;
    }
    if (vec3_changed(pivot_rot, rot_in)) {
        bool opened = false;
        begin_transform_write_if_needed(&opened);
        JcePivotComponent next = *p;
        next.local_rotation = editor_q_from_euler_deg(pivot_rot);
        jce_scene_set_pivot(scene, e, &next);
        end_transform_write_if_needed(opened);
    }

    /* Read-only world-space readout: with the engine's pivot model the
     * entity transform position IS the pivot's world location. */
    jce_vec3 wp = jce_scene_get_pivot_world_position(scene, e);
    ImGui::Text("%s", jce_editor_i18n("pivot.worldPosition"));
    ImGui::SameLine(110);
    ImGui::TextDisabled("%.3f  %.3f  %.3f", wp.x, wp.y, wp.z);

    if (ImGui::Button(jce_editor_i18n("pivot.reset"))) {
        bool opened = false;
        begin_transform_write_if_needed(&opened);
        JcePivotComponent next = *p;
        jce_scene_set_pivot_local_position_preserve_model(
            scene, e, jce_v3(0.0f, 0.0f, 0.0f));
        next.local_position = jce_v3(0.0f, 0.0f, 0.0f);
        next.local_rotation = jce_q_identity();
        jce_scene_set_pivot(scene, e, &next);
        end_transform_write_if_needed(opened);
    }
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("pivot.center"))) {
        uint32_t id = jce_state_from_ecs_entity(e);
        float bmin[3];
        float bmax[3];
        if (id != 0 &&
            jce_editor_scene_camera_get_entity_focus_bounds(id, bmin, bmax)) {
            bool opened = false;
            begin_transform_write_if_needed(&opened);
            jce_scene_set_pivot_world_position_preserve_model(
                scene, e,
                jce_v3((bmin[0] + bmax[0]) * 0.5f,
                       (bmin[1] + bmax[1]) * 0.5f,
                       (bmin[2] + bmax[2]) * 0.5f));
            end_transform_write_if_needed(opened);
        }
    }
}
