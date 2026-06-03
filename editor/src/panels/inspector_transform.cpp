/*
 * inspector_transform.cpp
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
        end_transform_write_if_needed(opened);
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
        end_transform_write_if_needed(opened);
    }
}
