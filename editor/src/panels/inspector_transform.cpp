/*
 * inspector_transform.cpp
 * Transform component inspector drawer.
 */

#include "jce_panel_inspector_common.h"

void draw_comp_transform(uint32_t entity_id, JceTransform *t)
{
    float pos[3] = {t->position.x, t->position.y, t->position.z};
    float scl[3] = {t->scale.x,    t->scale.y,    t->scale.z};
    float rot[3];
    if (!jce_editor_get_cached_euler_deg(entity_id, t->rotation, rot))
        editor_q_to_euler_deg(t->rotation, rot);

    ImGui::Text("%s", jce_editor_i18n("transform.position"));
    ImGui::SameLine(80);
    draw_vec3_control("Position", pos);
    if (pos[0] != t->position.x || pos[1] != t->position.y || pos[2] != t->position.z) {
        t->position.x = pos[0]; t->position.y = pos[1]; t->position.z = pos[2];
    }

    ImGui::Text("%s", jce_editor_i18n("transform.rotation"));
    ImGui::SameLine(80);
    float rot_in[3] = {rot[0], rot[1], rot[2]};
    draw_vec3_control("Rotation", rot, 1.0f);
    for (int a = 0; a < 3; a++) {
        rot[a] = fmodf(rot[a], 360.0f);
        if (rot[a] < 0.0f) rot[a] += 360.0f;
    }
    if (rot[0] != rot_in[0] || rot[1] != rot_in[1] || rot[2] != rot_in[2]) {
        t->rotation = editor_q_from_euler_deg(rot);
        jce_editor_set_cached_euler_deg(entity_id, t->rotation, rot);
    }

    ImGui::Text("%s", jce_editor_i18n("transform.scale"));
    ImGui::SameLine(80);
    draw_vec3_control("Scale", scl, 0.01f, 1.0f);
    if (scl[0] != t->scale.x || scl[1] != t->scale.y || scl[2] != t->scale.z) {
        t->scale.x = scl[0]; t->scale.y = scl[1]; t->scale.z = scl[2];
    }
}
