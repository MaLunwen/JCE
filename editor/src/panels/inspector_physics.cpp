/*
 * inspector_physics.cpp
 * 3D physics component inspector drawers: rigidbody, box/sphere/capsule/mesh
 * colliders, character controller, constraint, wheel collider, constant force,
 * configurable joint.
 */

#include "jce_panel_inspector_common.h"
#include "ui/jce_editor_tip.h"
#include "ui/jce_editor_dnd.h"

void draw_comp_rigidbody(JceRigidBodyComponent *rb)
{
    char lbl[256];
    snprintf(lbl, sizeof(lbl), "%s###rbMass", jce_editor_i18n("rigidbody.mass"));
    ImGui::DragFloat(lbl, &rb->mass, 0.1f, 0.0f, 10000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbDrag", jce_editor_i18n("rigidbody.drag"));
    ImGui::DragFloat(lbl, &rb->drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbAngularDrag", jce_editor_i18n("rigidbody.angularDrag"));
    ImGui::DragFloat(lbl, &rb->angular_drag, 0.01f, 0.0f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbUseGravity", jce_editor_i18n("rigidbody.useGravity"));
    if (ImGui::Checkbox(lbl, &rb->use_gravity))
        insp_undo_bool(&rb->use_gravity);
    snprintf(lbl, sizeof(lbl), "%s###rbIsKinematic", jce_editor_i18n("rigidbody.isKinematic"));
    if (ImGui::Checkbox(lbl, &rb->is_kinematic))
        insp_undo_bool(&rb->is_kinematic);

    /* ── Per-body gravity scale (disabled when Use Gravity is off) ── */
    snprintf(lbl, sizeof(lbl), "%s###rbGravScale",
             jce_editor_i18n("rigidbody.gravityScale"));
    ImGui::BeginDisabled(!rb->use_gravity);
    ImGui::DragFloat(lbl, &rb->gravity_scale, 0.05f, -10.0f, 10.0f, "%.2f");
    insp_track_edit();
    ImGui::EndDisabled();

    /* ── Friction / restitution (overridden when a material is set) ── */
    ImGui::BeginDisabled(rb->physmat_path[0] != '\0');
    snprintf(lbl, sizeof(lbl), "%s###rbFriction",
             jce_editor_i18n("rigidbody.friction"));
    ImGui::DragFloat(lbl, &rb->friction, 0.01f, 0.0f, 10.0f, "%.2f");
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###rbRestitution",
             jce_editor_i18n("rigidbody.restitution"));
    ImGui::DragFloat(lbl, &rb->restitution, 0.01f, 0.0f, 1.0f, "%.2f");
    insp_track_edit();
    ImGui::EndDisabled();

    /* ── Collision layer (indexes the project Layer Collision Matrix) ── */
    {
        const JceProjectSettings *ps = jce_project_settings_current();
        const char *names[JCE_PS_LAYER_COUNT];
        static char fb[JCE_PS_LAYER_COUNT][24];
        for (int i = 0; i < JCE_PS_LAYER_COUNT; ++i) {
            const char *nm = ps ? ps->tags_layers.layers[i] : "";
            if (!nm || !nm[0]) {
                snprintf(fb[i], sizeof fb[i], "Layer %d", i);
                names[i] = fb[i];
            } else {
                names[i] = nm;
            }
        }
        int layer = (int)rb->physics_layer;
        if (layer < 0 || layer >= JCE_PS_LAYER_COUNT) layer = 0;
        snprintf(lbl, sizeof(lbl), "%s###rbLayer",
                 jce_editor_i18n("rigidbody.layer"));
        if (ImGui::Combo(lbl, &layer, names, JCE_PS_LAYER_COUNT)) {
            rb->physics_layer = (uint32_t)layer;
            insp_track_edit();
        }
    }

    /* ── Physics material override (.physmat.json) ── */
    snprintf(lbl, sizeof(lbl), "%s###rbPhysMat",
             jce_editor_i18n("rigidbody.physMaterial"));
    jce_draw_path_input_asset(lbl, rb->physmat_path, sizeof rb->physmat_path,
                              JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(rb->physmat_path, sizeof rb->physmat_path);

    /* ── Continuous Collision Detection (P3-C.3) ─────────────────── */
    const char *ccd_items[4] = {
        jce_editor_i18n("inspector.rigidbody.ccd.discrete"),
        jce_editor_i18n("inspector.rigidbody.ccd.continuous"),
        jce_editor_i18n("inspector.rigidbody.ccd.continuous_dynamic"),
        jce_editor_i18n("inspector.rigidbody.ccd.continuous_speculative"),
    };
    int ccd_mode = (int)rb->ccd_mode;
    if (ccd_mode < 0) ccd_mode = 0;
    if (ccd_mode > 3) ccd_mode = 3;
    snprintf(lbl, sizeof(lbl), "%s###rbCcdMode",
             jce_editor_i18n("inspector.rigidbody.ccd_mode"));
    if (ImGui::Combo(lbl, &ccd_mode, ccd_items, 4)) {
        rb->ccd_mode = (uint8_t)ccd_mode;
        insp_track_edit();
    }
    jce_editor::help_tip(jce_editor_i18n("inspector.rigidbody.ccd.tooltip"));

    if (rb->ccd_mode != 0) {
        snprintf(lbl, sizeof(lbl), "%s###rbCcdThr",
                 jce_editor_i18n("inspector.rigidbody.ccd_threshold"));
        ImGui::DragFloat(lbl, &rb->ccd_threshold, 0.001f, 0.0f, 1.0f, "%.4f");
        insp_track_edit();
        snprintf(lbl, sizeof(lbl), "%s###rbCcdRad",
                 jce_editor_i18n("inspector.rigidbody.ccd_sphere_radius"));
        ImGui::DragFloat(lbl, &rb->ccd_sphere_radius, 0.01f, 0.0f, 2.0f, "%.3f");
        insp_track_edit();
    }
}

void draw_comp_box_collider(JceBoxColliderComponent *bc)
{
    char lbl[256];
    ImGui::Text("%s", jce_editor_i18n("collider.center"));
    ImGui::SameLine(80);
    draw_vec3_control("BoxCenter", bc->center);
    ImGui::Text("%s", jce_editor_i18n("collider.size"));
    ImGui::SameLine(80);
    draw_vec3_control("BoxSize", bc->size, 0.01f, 1.0f);
    snprintf(lbl, sizeof(lbl), "%s###box", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &bc->is_trigger))
        insp_undo_bool(&bc->is_trigger);
}

void draw_comp_sphere_collider(JceSphereColliderComponent *sc)
{
    char lbl[256];
    ImGui::Text("%s", jce_editor_i18n("collider.center"));
    ImGui::SameLine(80);
    draw_vec3_control("SphereCenter", sc->center);
    ImGui::DragFloat(jce_editor_i18n("collider.radius"), &sc->radius, 0.01f, 0.001f, 1000.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###sphere", jce_editor_i18n("collider.isTrigger"));
    if (ImGui::Checkbox(lbl, &sc->is_trigger))
        insp_undo_bool(&sc->is_trigger);
}

void draw_comp_capsule_collider(JceCapsuleColliderComponent *cc)
{
    if (!cc) return;
    static const char *axes[] = { "X", "Y", "Z" };
    int ax = cc->axis; if (ax < 0 || ax > 2) ax = 1;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.capcol.axis", "capcol"), &ax, axes, 3)) { cc->axis = ax; insp_track_edit(); }
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.capcol.center", "capcol"), cc->center, 0.05f); insp_track_edit();
    ImGui::DragFloat (jce_editor_i18n_id("inspector.capcol.radius", "capcol"), &cc->radius, 0.01f, 0.0f, 10000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat (jce_editor_i18n_id("inspector.capcol.height", "capcol"), &cc->height, 0.01f, 0.0f, 10000.0f, "%.3f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.capcol.isTrigger", "capcol"), &cc->is_trigger))
        insp_undo_bool(&cc->is_trigger);
}

void draw_comp_mesh_collider(JceMeshColliderComponent *mc)
{
    if (!mc) return;
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.meshcol.mesh", "meshcol"), mc->mesh_path, sizeof mc->mesh_path, JCE_ASSET_KIND_MODEL);
    insp_track_edit();
    accept_asset_drop(mc->mesh_path, sizeof mc->mesh_path);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.meshcol.convex", "meshcol"), &mc->convex))
        insp_undo_bool(&mc->convex);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.meshcol.isTrigger", "meshcol"), &mc->is_trigger))
        insp_undo_bool(&mc->is_trigger);
    ImGui::DragFloat(jce_editor_i18n_id("inspector.meshcol.friction", "meshcol"),    &mc->friction,    0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.meshcol.restitution", "meshcol"), &mc->restitution, 0.01f, 0.0f, 1.0f,  "%.2f"); insp_track_edit();
    if (mc->is_trigger || !mc->convex)
        ImGui::TextDisabled(jce_editor_i18n("inspector.meshcol.convexNote"));
}

void draw_comp_compound_collider(JceCompoundColliderComponent *cc)
{
    if (!cc) return;

    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.compcol.model", "compcol"),
                              cc->model_path, sizeof cc->model_path, JCE_ASSET_KIND_MODEL);
    insp_track_edit();
    accept_asset_drop(cc->model_path, sizeof cc->model_path);

    static const char *modes[] = {
        "Auto", "Box", "Sphere", "Capsule", "Convex Hull", "Convex Decomp", "Triangle Mesh"
    };
    int mode = cc->mode; if (mode < 0 || mode > 6) mode = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.compcol.mode", "compcol"), &mode, modes, 7)) {
        cc->mode = (uint8_t)mode; insp_track_edit();
    }
    jce_editor::help_tip(jce_editor_i18n("inspector.compcol.mode.tooltip"));

    static const char *splits[] = { "By Part", "Whole" };
    int split = cc->split; if (split < 0 || split > 1) split = 0;
    if (ImGui::Combo(jce_editor_i18n_id("inspector.compcol.split", "compcol"), &split, splits, 2)) {
        cc->split = (uint8_t)split; insp_track_edit();
    }

    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.compcol.isStatic", "compcol"), &cc->is_static))
        insp_undo_bool(&cc->is_static);
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.compcol.detectNaming", "compcol"), &cc->detect_naming))
        insp_undo_bool(&cc->detect_naming);
    jce_editor::help_tip(jce_editor_i18n("inspector.compcol.detectNaming.tooltip"));
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.compcol.isTrigger", "compcol"), &cc->is_trigger))
        insp_undo_bool(&cc->is_trigger);

    ImGui::DragFloat(jce_editor_i18n_id("inspector.compcol.friction", "compcol"),
                     &cc->friction, 0.01f, 0.0f, 10.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.compcol.restitution", "compcol"),
                     &cc->restitution, 0.01f, 0.0f, 1.0f, "%.2f"); insp_track_edit();

    /* Physics material override (.physmat.json) — friction/restitution above
     * are ignored when a material is assigned. */
    jce_draw_path_input_asset(jce_editor_i18n_id("inspector.compcol.physMaterial", "compcol"),
                              cc->physmat_path, sizeof cc->physmat_path,
                              JCE_ASSET_KIND_DATA);
    insp_track_edit();
    accept_asset_drop(cc->physmat_path, sizeof cc->physmat_path);

    /* VHACD tuning only matters for the convex-decomposition mode. */
    if (cc->mode == 5) {
        ImGui::SeparatorText(jce_editor_i18n("inspector.compcol.vhacd"));
        int res = (int)cc->vhacd_resolution;
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.compcol.vhacdRes", "compcol"),
                           &res, 1000.0f, 0, 16000000)) {
            cc->vhacd_resolution = (uint32_t)(res < 0 ? 0 : res); insp_track_edit();
        }
        int hulls = (int)cc->vhacd_max_hulls;
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.compcol.vhacdHulls", "compcol"),
                           &hulls, 1.0f, 0, 1024)) {
            cc->vhacd_max_hulls = (uint32_t)(hulls < 0 ? 0 : hulls); insp_track_edit();
        }
        int verts = (int)cc->vhacd_max_verts_per_hull;
        if (ImGui::DragInt(jce_editor_i18n_id("inspector.compcol.vhacdVerts", "compcol"),
                           &verts, 1.0f, 0, 256)) {
            cc->vhacd_max_verts_per_hull = (uint32_t)(verts < 0 ? 0 : verts); insp_track_edit();
        }
        ImGui::TextDisabled("%s", jce_editor_i18n("inspector.compcol.vhacd.zeroNote"));
    }
}

void draw_comp_character_controller(JceCharacterControllerComponent *cc)
{
    char lbl[256];
    ImGui::DragFloat(jce_editor_i18n("collider.height"), &cc->height, 0.1f, 0.1f, 100.0f);
    insp_track_edit();
    snprintf(lbl, sizeof(lbl), "%s###cc", jce_editor_i18n("collider.radius"));
    ImGui::DragFloat(lbl, &cc->radius, 0.01f, 0.01f, 50.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.stepOffset"), &cc->step_offset, 0.01f, 0.0f, 10.0f);
    insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n("inspector.slopeLimit"), &cc->slope_limit, 1.0f, 0.0f, 90.0f);
    insp_track_edit();
}

void draw_comp_constraint(JceConstraintComponent *con)
{
    const char *constraint_types[] = { "Point2Point", "Hinge", "Slider", "6DOF" };
    int prev_type = con->constraint_type;
    if (ImGui::Combo(jce_editor_i18n("constraint.type"), &con->constraint_type, constraint_types, 4))
        insp_undo_int(&con->constraint_type, prev_type);

    /* Connected body — drag an entity from the Hierarchy here.  0 (empty)
     * anchors the constraint to the world. */
    {
        JceScene *scene = jce_state_get_scene();
        char tgt[160];
        if (con->target_entity == 0) {
            snprintf(tgt, sizeof tgt, "%s",
                     jce_editor_i18n("constraint.worldAnchor"));
        } else {
            const char *nm = scene ? jce_scene_entity_name(
                                 scene, (JceEntity)con->target_entity) : NULL;
            if (nm && nm[0]) snprintf(tgt, sizeof tgt, "%s", nm);
            else             snprintf(tgt, sizeof tgt, "Entity #%u",
                                      con->target_entity);
        }
        ImGui::TextUnformatted(jce_editor_i18n("constraint.connectedBody"));
        ImGui::SameLine();
        ImGui::Button(tgt, ImVec2(-1.0f, 0.0f));
        if (ImGui::BeginDragDropTarget()) {
            if (const ImGuiPayload *pl =
                    ImGui::AcceptDragDropPayload(JCE_DND_ENTITY)) {
                uint32_t id = *(const uint32_t *)pl->Data;
                if (id != (uint32_t)con->target_entity) {
                    con->target_entity = id;
                    insp_track_edit();
                }
            }
            ImGui::EndDragDropTarget();
        }
        if (con->target_entity != 0) {
            if (ImGui::SmallButton(jce_editor_i18n("constraint.clearTarget"))) {
                con->target_entity = 0;
                insp_track_edit();
            }
        }
    }

    ImGui::DragFloat3(jce_editor_i18n("constraint.pivotA"), con->pivot_a, 0.1f);
    insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n("constraint.pivotB"), con->pivot_b, 0.1f);
    insp_track_edit();
    if (con->constraint_type == 1 || con->constraint_type == 2) {
        ImGui::DragFloat3(jce_editor_i18n("constraint.axis"), con->axis, 0.1f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("constraint.lowerLimit"), &con->lower_limit, 0.1f);
        insp_track_edit();
        ImGui::DragFloat(jce_editor_i18n("constraint.upperLimit"), &con->upper_limit, 0.1f);
        insp_track_edit();
    }
    if (ImGui::Checkbox(jce_editor_i18n("constraint.disableCollision"), &con->disable_collision))
        insp_undo_bool(&con->disable_collision);
}

void draw_comp_wheel_collider(JceWheelColliderComponent *w)
{
    if (!w) return;
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.radius", "wc"),               &w->radius,                0.01f, 0.01f, 100.0f,   "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionDistance", "wc"),  &w->suspension_distance,   0.01f, 0.0f, 10.0f,     "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionSpring", "wc"),    &w->suspension_spring,     50.0f, 0.0f, 1.0e7f,    "%.0f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionDamper", "wc"),    &w->suspension_damper,     10.0f, 0.0f, 1.0e6f,    "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.suspensionTargetPos", "wc"),&w->suspension_target_pos, 0.01f, 0.0f, 1.0f,      "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.mass", "wc"),                 &w->mass,                  0.1f, 0.001f, 100000.0f,"%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.forwardFriction", "wc"),     &w->forward_friction,      0.01f, 0.0f, 10.0f,     "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.sidewaysFriction", "wc"),    &w->sideways_friction,     0.01f, 0.0f, 10.0f,     "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.wc.center", "wc"),              w->center,                 0.01f, -100.0f, 100.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.motorTorque", "wc"),         &w->motor_torque,          1.0f, -100000.0f, 100000.0f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.brakeTorque", "wc"),         &w->brake_torque,          1.0f, 0.0f, 100000.0f,  "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.wc.steerAngle", "wc"),    &w->steer_angle_deg,       0.5f, -90.0f, 90.0f,    "%.2f"); insp_track_edit();
}

void draw_comp_constant_force(JceConstantForceComponent *cf)
{
    if (!cf) return;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cf.enabled", "cf"), &cf->enabled)) insp_undo_bool(&cf->enabled);
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.force", "cf"),          cf->force,           0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.relativeForce", "cf"), cf->relative_force,  0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.torque", "cf"),         cf->torque,          0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cf.relativeTorque", "cf"),cf->relative_torque, 0.1f, -1.0e6f, 1.0e6f, "%.3f"); insp_track_edit();
}

void draw_comp_configurable_joint(JceConfigurableJointComponent *cj)
{
    if (!cj) return;
    int connected = (int)cj->connected_body;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cjj.connectedBody", "cjj"), &connected, 1.0f, 0, 1<<30)) {
        cj->connected_body = (uint64_t)(connected < 0 ? 0 : connected);
        insp_track_edit();
    }
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cjj.anchor", "cjj"),            cj->anchor,           0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    ImGui::DragFloat3(jce_editor_i18n_id("inspector.cjj.connectedAnchor", "cjj"),  cj->connected_anchor, 0.01f, -1000.0f, 1000.0f, "%.3f"); insp_track_edit();
    static const char *motion_names[] = { "Locked", "Limited", "Free" };
    ImGui::TextUnformatted(jce_editor_i18n("inspector.cjj.linearMotion"));
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_lx.x", "cjj_lx"), &cj->x_motion, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_ly.y", "cjj_ly"), &cj->y_motion, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_lz.z", "cjj_lz"), &cj->z_motion, motion_names, 3)) insp_track_edit();
    ImGui::TextUnformatted(jce_editor_i18n("inspector.cjj.angularMotion"));
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_ax.x", "cjj_ax"), &cj->x_rotation, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_ay.y", "cjj_ay"), &cj->y_rotation, motion_names, 3)) insp_track_edit();
    if (ImGui::Combo(jce_editor_i18n_id("inspector.cjj_az.z", "cjj_az"), &cj->z_rotation, motion_names, 3)) insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.linearLimit", "cjj"),   &cj->linear_limit,        0.01f, 0.0f, 1.0e6f, "%.3f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.angularXLimit", "cjj"),&cj->angular_x_limit_deg, 0.5f, 0.0f, 180.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.angularYLimit", "cjj"),&cj->angular_y_limit_deg, 0.5f, 0.0f, 180.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.angularZLimit", "cjj"),&cj->angular_z_limit_deg, 0.5f, 0.0f, 180.0f, "%.2f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.breakForce", "cjj"),    &cj->break_force,  10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    ImGui::DragFloat(jce_editor_i18n_id("inspector.cjj.breakTorque", "cjj"),   &cj->break_torque, 10.0f, 0.0f, 3.4e38f, "%.1f"); insp_track_edit();
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cjj.enableCollision", "cjj"), &cj->enable_collision)) insp_undo_bool(&cj->enable_collision);
}

/* ── Cloth (P3-C.4 follow-up) ────────────────────────────────────── */
void draw_comp_cloth(JceClothComponent *cl)
{
    if (!cl) return;

    /* Any change to authoring fields marks the component dirty so the
     * scene's reconciliation pass rebuilds the runtime handle. */
#define CLOTH_F3(KEY, ID, FIELD)                                              \
    do {                                                                      \
        float v[3] = { (FIELD).x, (FIELD).y, (FIELD).z };                     \
        if (ImGui::DragFloat3(jce_editor_i18n_id(KEY, ID), v, 0.01f,          \
                              -1000.0f, 1000.0f, "%.3f")) {                   \
            (FIELD).x = v[0]; (FIELD).y = v[1]; (FIELD).z = v[2];             \
            cl->dirty = true; insp_track_edit();                              \
        }                                                                     \
    } while (0)

    CLOTH_F3("inspector.cloth.c00", "cloth_c00", cl->corner_00);
    CLOTH_F3("inspector.cloth.c10", "cloth_c10", cl->corner_10);
    CLOTH_F3("inspector.cloth.c01", "cloth_c01", cl->corner_01);
    CLOTH_F3("inspector.cloth.c11", "cloth_c11", cl->corner_11);
#undef CLOTH_F3

    int ru = (int)cl->res_u, rv = (int)cl->res_v;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cloth.resU", "cloth_ru"),
                       &ru, 1.0f, 2, 256)) {
        if (ru < 2) ru = 2;
        cl->res_u = (uint32_t)ru; cl->dirty = true; insp_track_edit();
    }
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cloth.resV", "cloth_rv"),
                       &rv, 1.0f, 2, 256)) {
        if (rv < 2) rv = 2;
        cl->res_v = (uint32_t)rv; cl->dirty = true; insp_track_edit();
    }

    if (ImGui::DragFloat(jce_editor_i18n_id("inspector.cloth.mass", "cloth_m"),
                         &cl->mass_total, 0.05f, 0.001f, 1.0e4f, "%.3f")) {
        cl->dirty = true; insp_track_edit();
    }
    if (ImGui::SliderFloat(jce_editor_i18n_id("inspector.cloth.stiffLin", "cloth_sl"),
                           &cl->stiffness_linear, 0.0f, 1.0f, "%.3f")) {
        cl->dirty = true; insp_track_edit();
    }
    if (ImGui::SliderFloat(jce_editor_i18n_id("inspector.cloth.stiffAng", "cloth_sa"),
                           &cl->stiffness_angular, 0.0f, 1.0f, "%.3f")) {
        cl->dirty = true; insp_track_edit();
    }
    if (ImGui::SliderFloat(jce_editor_i18n_id("inspector.cloth.damping", "cloth_dp"),
                           &cl->damping, 0.0f, 1.0f, "%.3f")) {
        cl->dirty = true; insp_track_edit();
    }
    int iters = (int)cl->iterations;
    if (ImGui::DragInt(jce_editor_i18n_id("inspector.cloth.iters", "cloth_it"),
                       &iters, 1.0f, 1, 32)) {
        if (iters < 1) iters = 1;
        cl->iterations = (uint32_t)iters; cl->dirty = true; insp_track_edit();
    }

    bool self_coll = cl->self_collision;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cloth.selfColl", "cloth_sc"),
                        &self_coll)) {
        cl->self_collision = self_coll; cl->dirty = true; insp_track_edit();
    }

    /* Wind: tweaked in-place (no rebuild) — see reconciliation pass. */
    bool wind_on = cl->wind_enabled;
    if (ImGui::Checkbox(jce_editor_i18n_id("inspector.cloth.windOn", "cloth_won"),
                        &wind_on)) {
        cl->wind_enabled = wind_on; insp_track_edit();
    }
    float wv[3] = { cl->wind_velocity.x, cl->wind_velocity.y, cl->wind_velocity.z };
    if (ImGui::DragFloat3(jce_editor_i18n_id("inspector.cloth.windVel", "cloth_wv"),
                          wv, 0.05f, -100.0f, 100.0f, "%.3f")) {
        cl->wind_velocity.x = wv[0];
        cl->wind_velocity.y = wv[1];
        cl->wind_velocity.z = wv[2];
        insp_track_edit();
    }

    /* Pinned indices: comma-separated list, capped at JCE_CLOTH_MAX_PINNED. */
    static char buf[512];
    {
        size_t off = 0;
        buf[0] = '\0';
        for (uint32_t i = 0; i < cl->pinned_count && off + 8 < sizeof buf; ++i) {
            int n = snprintf(buf + off, sizeof(buf) - off,
                             "%s%u", (i == 0 ? "" : ","), cl->pinned_indices[i]);
            if (n < 0) break;
            off += (size_t)n;
        }
    }
    char edit_buf[512];
    memcpy(edit_buf, buf, sizeof buf);
    if (ImGui::InputText(jce_editor_i18n_id("inspector.cloth.pinned", "cloth_pin"),
                         edit_buf, sizeof edit_buf)) {
        uint32_t newp[JCE_CLOTH_MAX_PINNED];
        uint32_t cnt = 0;
        bool overflow = false;
        const char *p = edit_buf;
        while (*p) {
            while (*p == ' ' || *p == ',' || *p == '\t') ++p;
            if (!*p) break;
            char *end = NULL;
            unsigned long v = strtoul(p, &end, 10);
            if (end == p) break;
            if (cnt >= JCE_CLOTH_MAX_PINNED) { overflow = true; break; }
            newp[cnt++] = (uint32_t)v;
            p = end;
        }
        memset(cl->pinned_indices, 0, sizeof cl->pinned_indices);
        memcpy(cl->pinned_indices, newp, cnt * sizeof(uint32_t));
        cl->pinned_count = cnt;
        cl->dirty = true;
        insp_track_edit();
        if (overflow) {
            ImGui::TextDisabled("%s",
                jce_editor_i18n("inspector.cloth.pinnedOverflow"));
        }
    }

    ImGui::Separator();
    if (ImGui::Button(jce_editor_i18n("inspector.cloth.recreate"))) {
        cl->dirty = true; insp_track_edit();
    }
    ImGui::SameLine();
    ImGui::TextDisabled("handle=%u", cl->handle);
}
