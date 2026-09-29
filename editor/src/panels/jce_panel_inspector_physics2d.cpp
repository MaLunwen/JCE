/*
 * jce_panel_inspector_physics2d.cpp
 * 2D physics component inspector drawers: rigidbody2d, collider2d, joint2d.
 */

#include "jce_panel_inspector_common.h"
#include "jce_panel_inspector_enum_maps.h"

void draw_comp_rigidbody2d(JceRigidBody2DComponent *rb)
{
    if (!rb) return;
    /* THE LABELS AND THE ENUM ARE NOT IN THE SAME ORDER, and this combo used
     * the raw index as the value.  JceBodyType is STATIC=0, DYNAMIC=1,
     * KINEMATIC=2; the list reads static / kinematic / dynamic, so picking
     * "Kinematic" stored 1 (DYNAMIC) and picking "Dynamic" stored 2
     * (KINEMATIC) -- a body that ignores gravity when you asked it to fall,
     * and falls when you asked it to be driven.  Reading back showed the
     * other one, so the panel agreed with itself and the physics did not.
     * Same value-table shape as the shapeType combo below, for the same
     * reason: index order is a UI decision, enum order is an ABI one. */
    const char *body_types[JCE_INSP_BODY_TYPE_COUNT] = {
        jce_editor_i18n("inspector.rb2d.bodyType.static"),
        jce_editor_i18n("inspector.rb2d.bodyType.kinematic"),
        jce_editor_i18n("inspector.rb2d.bodyType.dynamic"),
    };
    int bt = insp_rb2d_body_type_index(rb->body_type);
    if (ImGui::Combo(jce_editor_i18n_id("inspector.rb2d.bodyType", "rb2d"),
                     &bt, body_types, JCE_INSP_BODY_TYPE_COUNT))
        insp_undo_set(&rb->body_type, insp_rb2d_body_type_value(bt));
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rb2d.mass", "rb2d"), &rb->mass, 0.1f, 0.0f, 10000.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rb2d.friction", "rb2d"), &rb->friction, 0.01f, 0.0f, 10.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.rb2d.restitution", "rb2d"), &rb->restitution, 0.01f, 0.0f, 1.0f);
    insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.rb2d.fixedRotation", "rb2d"), &rb->fixed_rotation))
        insp_undo_bool(&rb->fixed_rotation);

    /* The 2D collision layer.  The SAME control the 3D Rigidbody uses, over
     * the same 32 named slots -- names are shared between the two matrices,
     * only the pairs differ.
     *
     * Without this the field is engine state a designer cannot reach: the
     * solver would read it, every body would sit on layer 0, and the 32x32 2D
     * grid would be "working" while nothing could be assigned to a layer.
     * That is the same broken link this unit exists to fix, pointed the other
     * way -- check_component_field_consumed named it, citing
     * JceSpawnManagerComponent.ped_prefab_path, which made every authored
     * SpawnManager spawn nothing. */
    insp_physics_layer_combo(&rb->physics_layer, "rb2dLayer", true);

    /* JceRigidBody*Component.shape_type: the shape a body gets when it has
     * NO collider component.  Only the three that can be derived from the
     * transform scale are offered -- a hull or a triangle mesh needs authored
     * geometry, so listing them here would offer a choice that silently falls
     * back to Box. */
    const char *shape_names[JCE_INSP_SHAPE_TYPE_COUNT] = {
        jce_editor_i18n("inspector.rb.shape.box"),
        jce_editor_i18n("inspector.rb.shape.sphere"),
        jce_editor_i18n("inspector.rb.shape.capsule"),
    };
    int sh = insp_rb_shape_type_index(rb->shape_type);
    if (ImGui::Combo(jce_editor_i18n_id("inspector.rb2d.shapeType", "rb2d"),
                     &sh, shape_names, 3))
        insp_undo_set(&rb->shape_type, insp_rb_shape_type_value(sh));
    ImGui::SetItemTooltip("%s", jce_editor_i18n("inspector.rb.shapeType.tip"));
}

void draw_comp_collider2d(JceCollider2DComponent *cd)
{
    if (!cd) return;
    const char *shapes[] = { jce_editor_i18n("inspector.c2d.shape.box"), jce_editor_i18n("inspector.c2d.shape.circle"), jce_editor_i18n("inspector.c2d.shape.capsule"), jce_editor_i18n("inspector.c2d.shape.edge"), jce_editor_i18n("inspector.c2d.shape.polygon") };
    int sh = cd->shape; if (sh < 0 || sh > 4) sh = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.c2d.shape", "c2d"), &sh, shapes, 5))
        insp_undo_set(&cd->shape, sh);
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.c2d.offset", "c2d"), cd->offset, 0.05f); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.c2d.isTrigger", "c2d"), &cd->is_trigger))
        insp_undo_bool(&cd->is_trigger);
    /* The rest of this component reaches physics; this one field does not.
     * rt_build_body2d reads shape, offset, friction, restitution, size and
     * radius and never is_trigger, so a 2D collider marked as a trigger is
     * built solid.  It is a MISSING SUBSYSTEM rather than a missing line:
     * JceBody2DDesc carries no sensor field, and while the wrapper does set
     * enableSensorEvents on a sensor shape, nothing anywhere calls
     * b2World_GetSensorEvents and there is no public 2D overlap query -- so
     * wiring the flag alone would stop the collider colliding AND still
     * report no overlaps, which is worse than leaving it solid. */
    insp_unwired_field_badge();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.c2d.friction", "c2d"),    &cd->friction,    0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.c2d.restitution", "c2d"), &cd->restitution, 0.01f, 0.0f, 1.0f,  "%.2f"); insp_track_edit();
    ImGui::Separator();
    switch (cd->shape) {
    case JCE_COLLIDER_2D_BOX:
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.c2d.size", "c2d"), cd->size, 0.05f, 0.0f, 10000.0f, "%.3f");
        insp_track_edit();
        break;
    case JCE_COLLIDER_2D_CIRCLE:
        ImGui::DragFloat(jce_editor_i18n_id("inspector.c2d.radius", "c2d"), &cd->radius, 0.01f, 0.0f, 10000.0f, "%.3f");
        insp_track_edit();
        break;
    case JCE_COLLIDER_2D_CAPSULE: {
        ImGui::DragFloat2(jce_editor_i18n_id("inspector.c2d.size", "c2d"), cd->size, 0.05f, 0.0f, 10000.0f, "%.3f");
        insp_track_edit();
        const char *dirs[] = { jce_editor_i18n("inspector.c2d.direction.vertical"), jce_editor_i18n("inspector.c2d.direction.horizontal") };
        int dir = cd->capsule_direction; if (dir < 0 || dir > 1) dir = 0;
        if (ImGui::Combo(jce_editor_i18n_id("inspector.c2d.direction", "c2d"), &dir, dirs, 2))
            insp_undo_set(&cd->capsule_direction, dir);
        break;
    }
    case JCE_COLLIDER_2D_EDGE:
    case JCE_COLLIDER_2D_POLYGON: {
        int n = cd->point_count;
        if (ImGui::SliderInt(jce_editor_i18n_id("inspector.c2d.pointCount", "c2d"), &n, 0, JCE_COLLIDER_2D_MAX_POINTS)) {
            cd->point_count = n;
        }
        insp_track_edit();
        for (int i = 0; i < cd->point_count; ++i) {
            ImGui::PushID(i);
            char lbl[16]; snprintf(lbl, sizeof lbl, jce_editor_i18n_id("inspector.c2d.pD", "c2d"), i);
            ImGui::DragFloat2(lbl, cd->points[i], 0.05f);
            insp_track_edit();
            ImGui::PopID();
        }
        if (cd->shape == JCE_COLLIDER_2D_POLYGON)
            ImGui::TextDisabled(jce_editor_i18n("inspector.c2d.polygonNote"));
        else
            ImGui::TextDisabled(jce_editor_i18n("inspector.c2d.edgeNote"));
        break;
    }
    }
}

void draw_comp_joint2d(JceJoint2DComponent *j)
{
    insp_unwired_badge();
    if (!j) return;
    const char *kinds[] = { jce_editor_i18n("inspector.j2d.kind.distance"), jce_editor_i18n("inspector.j2d.kind.hinge"), jce_editor_i18n("inspector.j2d.kind.spring") };
    int k = j->kind; if (k < 0 || k > 2) k = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.j2d.kind", "j2d"), &k, kinds, 3))
        insp_undo_set(&j->kind, k);
    int connected = (int)j->connected_body;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.j2d.connectedBody", "j2d"), &connected, 1.0f, 0, 1<<30)) {
        j->connected_body = (uint64_t)(connected < 0 ? 0 : connected);
    }
    insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.j2d.anchor", "j2d"),            j->anchor,           0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat2(jce_editor_i18n_id("inspector.j2d.connectedAnchor", "j2d"),  j->connected_anchor, 0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    if (j->kind == JCE_JOINT_2D_DISTANCE || j->kind == JCE_JOINT_2D_SPRING) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.distance", "j2d"), &j->distance, 0.01f, 0.0f, 1.0e6f, "%.3f"); insp_track_edit();
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.autoConfigureDistance", "j2d"), &j->auto_configure_distance)) insp_undo_bool(&j->auto_configure_distance);
    }
    if (j->kind == JCE_JOINT_2D_SPRING) {
        ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.frequency", "j2d"),     &j->frequency,     0.05f, 0.0f, 10000.0f, "%.3f"); insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.dampingRatio", "j2d"), &j->damping_ratio, 0.01f, 0.0f, 1.0f,     "%.3f"); insp_track_edit();
    }
    if (j->kind == JCE_JOINT_2D_HINGE) {
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.useMotor", "j2d"), &j->use_motor)) insp_undo_bool(&j->use_motor);
        if (j->use_motor) {
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.motorSpeed", "j2d"), &j->motor_speed_deg_s, 1.0f, -3600.0f, 3600.0f, "%.1f"); insp_track_edit();
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.motorMaxTorque", "j2d"),    &j->motor_max_torque,  10.0f, 0.0f, 1.0e7f,     "%.1f"); insp_track_edit();
        }
        if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.useLimits", "j2d"), &j->use_limits)) insp_undo_bool(&j->use_limits);
        if (j->use_limits) {
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.lowerAngle", "j2d"), &j->lower_angle_deg, 0.5f, -360.0f, 360.0f, "%.2f"); insp_track_edit();
            ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.upperAngle", "j2d"), &j->upper_angle_deg, 0.5f, -360.0f, 360.0f, "%.2f"); insp_track_edit();
        }
    }
    ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.breakForce", "j2d"),  &j->break_force,  10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.j2d.breakTorque", "j2d"), &j->break_torque, 10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.j2d.enableCollision", "j2d"), &j->enable_collision)) insp_undo_bool(&j->enable_collision);
}
