/*
 * jce_scene_components_physics.c  Scene component (de)serialize module
 * for the physics domain (split from jce_scene_components_json.c).
 *
 * Pure move from the monolith: the shared JSON/Euler helpers live as
 * static inline in jce_scene_components_internal.h; the registry-referenced
 * parse_<x>/serw_<x> are external (declared in that header's shared section)
 * so the REG table can take their address; ser_<x> writers stay file-static.
 */

#include "jce_scene_components_internal.h"

void parse_rigidbody(JceScene *s, JceEntity e, const cJSON *c)
{
    JceRigidBodyComponent rb;
    memset(&rb, 0, sizeof(rb));
    rb.mass         = (float)j_num(c, "mass", 1.0);
    rb.drag         = (float)j_num(c, "drag", 0.0);
    rb.angular_drag = (float)j_num(c, "angularDrag", 0.05);
    rb.use_gravity  = j_bool(c, "useGravity", true);
    rb.is_kinematic = j_bool(c, "isKinematic", false);
    rb.freeze_rotation = j_bool(c, "freezeRotation", false);
    /* CCD (P3-C.3) — DISCRETE if absent for backwards compatibility. */
    {
        int mode = (int)j_num(c, "ccdMode", 0.0);
        if (mode < 0) mode = 0;
        if (mode > 3) mode = 3;
        rb.ccd_mode = (uint8_t)mode;
    }
    rb.ccd_threshold     = (float)j_num(c, "ccdThreshold", 0.0);
    rb.ccd_sphere_radius = (float)j_num(c, "ccdSphereRadius", 0.0);
    /* Friction / restitution (default 0.5 / 0.0 — matches the spawn-time
     * fallback so legacy scenes without these keys behave identically). */
    rb.friction      = (float)j_num(c, "friction", 0.5);
    rb.restitution   = (float)j_num(c, "restitution", 0.0);
    rb.gravity_scale = (float)j_num(c, "gravityScale", 1.0);
    rb.physics_layer = (uint32_t)j_num(c, "physicsLayer", 0.0);
    copy_str(rb.physmat_path, sizeof(rb.physmat_path),
             j_str(c, "physMaterial", ""));
    /* The shape a body with NO collider component falls back to.  The 2D
     * sibling has always round-tripped this; the 3D one did not, so the
     * inspector control wrote a field that never reached the file and the
     * choice was lost on the next load.  0 == JCE_SHAPE_BOX is the default
     * every scene written before this carries. */
    rb.shape_type = (uint8_t)(int)j_num(c, "shapeType", 0.0);
    /* JCE_RB_KIND_*, not JceBodyType.  Absent == 0 == AUTO, which is what
     * every scene written before this carries and what the engine already
     * did, so nothing changes for existing content. */
    rb.body_type  = (uint8_t)(int)j_num(c, "bodyType", 0.0);
    jce_scene_set_rigidbody(s, e, &rb);
}

void parse_rigidbody2d(JceScene *s, JceEntity e, const cJSON *c)
{
    JceRigidBody2DComponent rb;
    memset(&rb, 0, sizeof(rb));
    rb.body_type      = (uint8_t)(int)j_num(c, "bodyType", 0.0);
    rb.shape_type     = (uint8_t)(int)j_num(c, "shapeType", 0.0);
    rb.mass           = (float)j_num(c, "mass", 1.0);
    rb.friction       = (float)j_num(c, "friction", 0.4);
    rb.restitution    = (float)j_num(c, "restitution", 0.0);
    rb.fixed_rotation = j_bool(c, "fixedRotation", false);
    rb.physics_layer  = (uint32_t)j_num(c, "physicsLayer", 0.0);
    jce_scene_set_rigidbody2d(s, e, &rb);
}

void parse_box_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceBoxColliderComponent bc;
    memset(&bc, 0, sizeof(bc));
    bc.center[0] = (float)j_num(c, "centerX", 0.0);
    bc.center[1] = (float)j_num(c, "centerY", 0.0);
    bc.center[2] = (float)j_num(c, "centerZ", 0.0);
    bc.size[0]   = (float)j_num(c, "sizeX", 1.0);
    bc.size[1]   = (float)j_num(c, "sizeY", 1.0);
    bc.size[2]   = (float)j_num(c, "sizeZ", 1.0);
    bc.is_trigger = j_bool(c, "isTrigger", false);
    jce_scene_set_box_collider(s, e, &bc);
}

void parse_sphere_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSphereColliderComponent sc2;
    memset(&sc2, 0, sizeof(sc2));
    sc2.center[0] = (float)j_num(c, "centerX", 0.0);
    sc2.center[1] = (float)j_num(c, "centerY", 0.0);
    sc2.center[2] = (float)j_num(c, "centerZ", 0.0);
    sc2.radius     = (float)j_num(c, "radius", 0.5);
    sc2.is_trigger = j_bool(c, "isTrigger", false);
    jce_scene_set_sphere_collider(s, e, &sc2);
}

void parse_character_controller(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCharacterControllerComponent cc;
    memset(&cc, 0, sizeof(cc));
    cc.height      = (float)j_num(c, "height", 2.0);
    cc.radius      = (float)j_num(c, "radius", 0.5);
    cc.step_offset = (float)j_num(c, "stepOffset", 0.3);
    cc.slope_limit = (float)j_num(c, "slopeLimit", 45.0);
    cc.move_speed     = (float)j_num(c, "moveSpeed", 4.0);
    cc.sprint_mult    = (float)j_num(c, "sprintMult", 1.8);
    cc.jump_speed     = (float)j_num(c, "jumpSpeed", 5.0);
    cc.accel          = (float)j_num(c, "accel", 40.0);
    cc.air_control    = (float)j_num(c, "airControl", 0.35);
    cc.turn_speed_deg = (float)j_num(c, "turnSpeed", 720.0);
    cc.physics_layer  = (uint32_t)j_num(c, "physicsLayer", 0.0);
    jce_scene_set_character_controller(s, e, &cc);
}

void parse_buoyancy(JceScene *s, JceEntity e, const cJSON *c)
{
    JceBuoyancyComponent b;
    memset(&b, 0, sizeof(b));
    b.buoyancy_strength = (float)j_num(c, "buoyancyStrength", 20.0);
    b.drag              = (float)j_num(c, "drag", 1.0);
    b.enabled           = j_bool(c, "enabled", true);
    jce_scene_set_buoyancy(s, e, &b);
}

void parse_trigger_volume(JceScene *s, JceEntity e, const cJSON *c)
{
    JceTriggerVolumeComponent t;
    memset(&t, 0, sizeof(t));
    t.shape           = (int)j_num(c, "shape", 0);
    t.center[0]       = (float)j_num(c, "cx", 0.0);
    t.center[1]       = (float)j_num(c, "cy", 0.0);
    t.center[2]       = (float)j_num(c, "cz", 0.0);
    t.half_extents[0] = (float)j_num(c, "hx", 0.5);
    t.half_extents[1] = (float)j_num(c, "hy", 0.5);
    t.half_extents[2] = (float)j_num(c, "hz", 0.5);
    t.axis_x[0] = (float)j_num(c, "axX", 1.0); t.axis_y[1] = (float)j_num(c, "ayY", 1.0); t.axis_z[2] = (float)j_num(c, "azZ", 1.0);
    t.axis_x[1] = (float)j_num(c, "axY", 0.0); t.axis_x[2] = (float)j_num(c, "axZ", 0.0);
    t.axis_y[0] = (float)j_num(c, "ayX", 0.0); t.axis_y[2] = (float)j_num(c, "ayZ", 0.0);
    t.axis_z[0] = (float)j_num(c, "azX", 0.0); t.axis_z[1] = (float)j_num(c, "azY", 0.0);
    t.enabled   = j_bool(c, "enabled", true);
    t.fire_stay = j_bool(c, "fireStay", false);
    copy_str(t.tag, sizeof(t.tag), j_str(c, "tag", ""));
    jce_scene_set_trigger_volume(s, e, &t);
}

void parse_capsule_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCapsuleColliderComponent cc;
    memset(&cc, 0, sizeof(cc));
    cc.center[0] = (float)j_num(c, "cx", 0.0);
    cc.center[1] = (float)j_num(c, "cy", 0.0);
    cc.center[2] = (float)j_num(c, "cz", 0.0);
    cc.radius    = (float)j_num(c, "radius", 0.5);
    cc.height    = (float)j_num(c, "height", 2.0);
    cc.axis      = (int)  j_num(c, "axis", 1);
    cc.is_trigger= j_bool(c, "isTrigger", false);
    jce_scene_set_capsule_collider(s, e, &cc);
}

void parse_mesh_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceMeshColliderComponent mc;
    memset(&mc, 0, sizeof(mc));
    copy_str(mc.mesh_path, sizeof(mc.mesh_path), j_str(c, "meshPath", ""));
    mc.convex      = j_bool(c, "convex", false);
    mc.is_trigger  = j_bool(c, "isTrigger", false);
    mc.friction    = (float)j_num(c, "friction", 0.5);
    mc.restitution = (float)j_num(c, "restitution", 0.0);
    jce_scene_set_mesh_collider(s, e, &mc);
}

void parse_compound_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCompoundColliderComponent cc;
    memset(&cc, 0, sizeof(cc));
    copy_str(cc.model_path, sizeof(cc.model_path), j_str(c, "modelPath", ""));
    cc.mode          = (uint8_t)j_num(c, "mode", 0);
    cc.split         = (uint8_t)j_num(c, "split", 0);
    cc.is_static     = j_bool(c, "isStatic", true);
    cc.detect_naming = j_bool(c, "detectNaming", true);
    cc.is_trigger    = j_bool(c, "isTrigger", false);
    cc.friction      = (float)j_num(c, "friction", 0.5);
    cc.restitution   = (float)j_num(c, "restitution", 0.0);
    cc.vhacd_resolution         = (uint32_t)j_num(c, "vhacdResolution", 0);
    cc.vhacd_max_hulls          = (uint32_t)j_num(c, "vhacdMaxHulls", 0);
    cc.vhacd_max_verts_per_hull = (uint32_t)j_num(c, "vhacdMaxVertsPerHull", 0);
    copy_str(cc.physmat_path, sizeof(cc.physmat_path),
             j_str(c, "physMaterial", ""));
    jce_scene_set_compound_collider(s, e, &cc);
}

void parse_collider2d(JceScene *s, JceEntity e, const cJSON *c)
{
    JceCollider2DComponent cd;
    memset(&cd, 0, sizeof(cd));
    cd.shape             = (int)j_num(c, "shape", 0);
    cd.offset[0]         = (float)j_num(c, "offX", 0.0);
    cd.offset[1]         = (float)j_num(c, "offY", 0.0);
    cd.size[0]           = (float)j_num(c, "sizeX", 1.0);
    cd.size[1]           = (float)j_num(c, "sizeY", 1.0);
    cd.radius            = (float)j_num(c, "radius", 0.5);
    cd.capsule_direction = (int)j_num(c, "capsuleDir", 0);
    cd.is_trigger        = j_bool(c, "isTrigger", false);
    cd.friction          = (float)j_num(c, "friction", 0.4);
    cd.restitution       = (float)j_num(c, "restitution", 0.0);
    int n = (int)j_num(c, "pointCount", 0);
    if (n < 0) n = 0;
    if (n > JCE_COLLIDER_2D_MAX_POINTS) n = JCE_COLLIDER_2D_MAX_POINTS;
    cd.point_count = n;
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i);
        cd.points[i][0] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof(key), "py%d", i);
        cd.points[i][1] = (float)j_num(c, key, 0.0);
    }
    jce_scene_set_collider2d(s, e, &cd);
}

void parse_wheel_collider(JceScene *s, JceEntity e, const cJSON *c)
{
    JceWheelColliderComponent w; memset(&w, 0, sizeof w);
    w.radius                = (float)j_num(c, "radius", 0.5);
    w.suspension_distance   = (float)j_num(c, "suspensionDistance", 0.3);
    w.suspension_spring     = (float)j_num(c, "suspensionSpring", 35000.0);
    w.suspension_damper     = (float)j_num(c, "suspensionDamper", 4500.0);
    w.suspension_target_pos = (float)j_num(c, "suspensionTargetPos", 0.5);
    w.mass                  = (float)j_num(c, "mass", 20.0);
    w.forward_friction      = (float)j_num(c, "forwardFriction", 1.0);
    w.sideways_friction     = (float)j_num(c, "sidewaysFriction", 1.0);
    w.center[0] = (float)j_num(c, "centerX", 0.0);
    w.center[1] = (float)j_num(c, "centerY", 0.0);
    w.center[2] = (float)j_num(c, "centerZ", 0.0);
    w.motor_torque    = (float)j_num(c, "motorTorque", 0.0);
    w.brake_torque    = (float)j_num(c, "brakeTorque", 0.0);
    w.steer_angle_deg = (float)j_num(c, "steerAngleDeg", 0.0);
    jce_scene_set_wheel_collider(s, e, &w);
}

void parse_constant_force(JceScene *s, JceEntity e, const cJSON *c)
{
    JceConstantForceComponent f; memset(&f, 0, sizeof f);
    f.force[0] = (float)j_num(c, "forceX", 0.0);
    f.force[1] = (float)j_num(c, "forceY", 0.0);
    f.force[2] = (float)j_num(c, "forceZ", 0.0);
    f.relative_force[0] = (float)j_num(c, "relForceX", 0.0);
    f.relative_force[1] = (float)j_num(c, "relForceY", 0.0);
    f.relative_force[2] = (float)j_num(c, "relForceZ", 0.0);
    f.torque[0] = (float)j_num(c, "torqueX", 0.0);
    f.torque[1] = (float)j_num(c, "torqueY", 0.0);
    f.torque[2] = (float)j_num(c, "torqueZ", 0.0);
    f.relative_torque[0] = (float)j_num(c, "relTorqueX", 0.0);
    f.relative_torque[1] = (float)j_num(c, "relTorqueY", 0.0);
    f.relative_torque[2] = (float)j_num(c, "relTorqueZ", 0.0);
    f.enabled = j_bool(c, "enabled", true);
    jce_scene_set_constant_force(s, e, &f);
}

void parse_ragdoll(JceScene *s, JceEntity e, const cJSON *c)
{
    JceRagdollComponent rc;
    memset(&rc, 0, sizeof rc);
    /* Sane defaults so an entity that authored a bare component (or a partial
     * one) still gets a valid ragdoll: animation-driven, thin capsules. */
    rc.enable       = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "enable"));
    rc.blend_weight = (float)j_num(c, "blendWeight", 1.0);
    rc.radius       = (float)j_num(c, "radius", 0.08);
    rc.height_scale = (float)j_num(c, "heightScale", 1.0);
    if (rc.blend_weight < 0.0f) rc.blend_weight = 0.0f;
    if (rc.blend_weight > 1.0f) rc.blend_weight = 1.0f;
    if (rc.radius <= 0.0f)       rc.radius = 0.08f;
    if (rc.height_scale <= 0.0f) rc.height_scale = 1.0f;
    /* ABSENT => 0 => the engine default of 1.0, resolved at ragdoll build
     * time rather than here, because a NEGATIVE value is a meaningful
     * authored state (no limits at all) that must survive the load. */
    rc.joint_limit_scale = (float)j_num(c, "jointLimitScale", 0.0);
    jce_scene_set_ragdoll(s, e, &rc);
}

void parse_fracture(JceScene *s, JceEntity e, const cJSON *c)
{
    JceFractureComponent fc;
    memset(&fc, 0, sizeof fc);
    /* Defaults match the header: disabled, 8 fragments, impulse 50, density
     * 1000 (water), seed 12345.  An absent key keeps the default so old scenes
     * (which never wrote a Fracture component) round-trip to "disabled". */
    fc.enabled        = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "enabled"));
    fc.fragment_count = (int)j_num(c, "fragmentCount", 8.0);
    fc.break_impulse  = (float)j_num(c, "breakImpulse", 50.0);
    fc.density        = (float)j_num(c, "density", 1000.0);
    fc.seed           = (uint32_t)j_num(c, "seed", 12345.0);
    if (fc.fragment_count < 1)   fc.fragment_count = 1;
    if (fc.fragment_count > 256) fc.fragment_count = 256;
    if (fc.break_impulse < 0.0f) fc.break_impulse = 0.0f;
    if (fc.density <= 0.0f)       fc.density = 1000.0f;
    jce_scene_set_fracture(s, e, &fc);
}

void parse_vehicle(JceScene *s, JceEntity e, const cJSON *c)
{
    JceVehicleComponent vc;
    memset(&vc, 0, sizeof vc);
    /* Defaults match the header.  An absent "enabled" key keeps it OFF so old
     * scenes (which never wrote a Vehicle component) round-trip to "inert".
     * Half-extents default to (0,0,0) -> runtime derives from BoxCollider. */
    vc.enabled                 = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "enabled"));
    vc.chassis_half_extents[0] = (float)j_num(c, "chassisHalfX", 0.0);
    vc.chassis_half_extents[1] = (float)j_num(c, "chassisHalfY", 0.0);
    vc.chassis_half_extents[2] = (float)j_num(c, "chassisHalfZ", 0.0);
    vc.chassis_mass            = (float)j_num(c, "chassisMass", 1500.0);
    vc.max_engine_force        = (float)j_num(c, "maxEngineForce", 4000.0);
    vc.max_brake_force         = (float)j_num(c, "maxBrakeForce", 100.0);
    vc.max_steering_deg        = (float)j_num(c, "maxSteeringDeg", 30.0);
    vc.drive_mode              = (int)j_num(c, "driveMode", 0.0);
    vc.input_mode              = (int)j_num(c, "inputMode", 1.0);
    if (vc.drive_mode < 0 || vc.drive_mode > 2) vc.drive_mode = 0;
    if (vc.input_mode < 0 || vc.input_mode > 1) vc.input_mode = 1;
    jce_scene_set_vehicle(s, e, &vc);
}

void parse_soft_body(JceScene *s, JceEntity e, const cJSON *c)
{
    JceSoftBodyComponent sc;
    memset(&sc, 0, sizeof sc);
    /* Defaults match the header.  An absent "enabled" key keeps it OFF so old
     * scenes (which never wrote a SoftBody component) round-trip to "inert". */
    sc.enabled          = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "enabled"));
    sc.radius[0]        = (float)j_num(c, "radiusX", 0.5);
    sc.radius[1]        = (float)j_num(c, "radiusY", 0.5);
    sc.radius[2]        = (float)j_num(c, "radiusZ", 0.5);
    sc.mass             = (float)j_num(c, "mass", 2.0);
    sc.pressure         = (float)j_num(c, "pressure", 100.0);
    sc.stiffness_linear = (float)j_num(c, "stiffnessLinear", 0.4);
    sc.stiffness_volume = (float)j_num(c, "stiffnessVolume", 0.4);
    sc.damping          = (float)j_num(c, "damping", 0.02);
    sc.friction         = (float)j_num(c, "friction", 0.5);
    sc.resolution       = (int)j_num(c, "resolution", 96.0);
    sc.self_collision   = cJSON_IsTrue(cJSON_GetObjectItemCaseSensitive(c, "selfCollision"));
    if (sc.mass <= 0.0f)        sc.mass = 2.0f;
    if (sc.resolution < 4)      sc.resolution = 64;
    if (sc.resolution > 256)    sc.resolution = 256;
    jce_scene_set_soft_body(s, e, &sc);
}

void parse_configurable_joint(JceScene *s, JceEntity e, const cJSON *c)
{
    JceConfigurableJointComponent j; memset(&j, 0, sizeof j);
    j.connected_body = (uint64_t)j_num(c, "connectedBody", 0.0);
    j.anchor[0] = (float)j_num(c, "anchorX", 0.0);
    j.anchor[1] = (float)j_num(c, "anchorY", 0.0);
    j.anchor[2] = (float)j_num(c, "anchorZ", 0.0);
    j.connected_anchor[0] = (float)j_num(c, "connAnchorX", 0.0);
    j.connected_anchor[1] = (float)j_num(c, "connAnchorY", 0.0);
    j.connected_anchor[2] = (float)j_num(c, "connAnchorZ", 0.0);
    j.x_motion = (int)j_num(c, "xMotion", 0);
    j.y_motion = (int)j_num(c, "yMotion", 0);
    j.z_motion = (int)j_num(c, "zMotion", 0);
    j.x_rotation = (int)j_num(c, "xRotation", 0);
    j.y_rotation = (int)j_num(c, "yRotation", 0);
    j.z_rotation = (int)j_num(c, "zRotation", 0);
    j.linear_limit         = (float)j_num(c, "linearLimit", 0.0);
    j.angular_x_limit_deg  = (float)j_num(c, "angXLimit", 0.0);
    j.angular_y_limit_deg  = (float)j_num(c, "angYLimit", 0.0);
    j.angular_z_limit_deg  = (float)j_num(c, "angZLimit", 0.0);
    j.break_force  = (float)j_num(c, "breakForce", 1e30);
    j.break_torque = (float)j_num(c, "breakTorque", 1e30);
    j.enable_collision = j_bool(c, "enableCollision", false);
    /* Per-axis drives, indexed keys.  A scene written before drives existed
     * has none of them and parses to JCE_JOINT_DRIVE_OFF on every axis, which
     * is what the memset above already holds. */
    for (int d = 0; d < 6; ++d) {
        char key[24];
        snprintf(key, sizeof key, "driveMode%d", d);
        j.drive_mode[d] = (int)j_num(c, key, 0.0);
        snprintf(key, sizeof key, "driveTarget%d", d);
        j.drive_target[d] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof key, "driveSpring%d", d);
        j.drive_spring[d] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof key, "driveDamper%d", d);
        j.drive_damper[d] = (float)j_num(c, key, 0.0);
        snprintf(key, sizeof key, "driveMaxForce%d", d);
        j.drive_max_force[d] = (float)j_num(c, key, 0.0);
    }
    jce_scene_set_configurable_joint(s, e, &j);
}

void parse_cloth(JceScene *s, JceEntity e, const cJSON *c)
{
    JceClothComponent cl; memset(&cl, 0, sizeof cl);
    cl.corner_00 = jce_v3((float)j_num(c, "c00X", 0.0), (float)j_num(c, "c00Y", 0.0), (float)j_num(c, "c00Z", 0.0));
    cl.corner_10 = jce_v3((float)j_num(c, "c10X", 1.0), (float)j_num(c, "c10Y", 0.0), (float)j_num(c, "c10Z", 0.0));
    cl.corner_01 = jce_v3((float)j_num(c, "c01X", 0.0), (float)j_num(c, "c01Y", 0.0), (float)j_num(c, "c01Z", 1.0));
    cl.corner_11 = jce_v3((float)j_num(c, "c11X", 1.0), (float)j_num(c, "c11Y", 0.0), (float)j_num(c, "c11Z", 1.0));
    cl.res_u = (uint32_t)j_num(c, "resU", 8);
    cl.res_v = (uint32_t)j_num(c, "resV", 8);
    if (cl.res_u < 2) cl.res_u = 2;
    if (cl.res_v < 2) cl.res_v = 2;
    cl.mass_total        = (float)j_num(c, "mass", 1.0);
    cl.stiffness_linear  = (float)j_num(c, "stiffLin", 0.5);
    cl.stiffness_angular = (float)j_num(c, "stiffAng", 0.5);
    cl.damping           = (float)j_num(c, "damping", 0.02);
    cl.iterations        = (uint32_t)j_num(c, "iters", 4);
    cl.self_collision    = j_bool(c, "selfColl", false);
    cl.wind_enabled      = j_bool(c, "windOn", false);
    cl.wind_velocity     = jce_v3((float)j_num(c, "windX", 0.0),
                                  (float)j_num(c, "windY", 0.0),
                                  (float)j_num(c, "windZ", 0.0));
    const cJSON *pin = cJSON_GetObjectItemCaseSensitive(c, "pinned");
    if (cJSON_IsArray(pin)) {
        int n = cJSON_GetArraySize(pin);
        if (n > (int)JCE_CLOTH_MAX_PINNED) n = (int)JCE_CLOTH_MAX_PINNED;
        for (int i = 0; i < n; ++i) {
            const cJSON *it = cJSON_GetArrayItem(pin, i);
            if (cJSON_IsNumber(it))
                cl.pinned_indices[cl.pinned_count++] = (uint32_t)it->valuedouble;
        }
    }
    cl.handle = 0;
    cl.dirty  = true;
    jce_scene_set_cloth(s, e, &cl);
}

void parse_joint2d(JceScene *s, JceEntity e, const cJSON *c)
{
    JceJoint2DComponent j; memset(&j, 0, sizeof j);
    j.kind = (int)j_num(c, "kind", 0);
    j.connected_body = (uint64_t)j_num(c, "connectedBody", 0.0);
    j.anchor[0] = (float)j_num(c, "anchorX", 0.0);
    j.anchor[1] = (float)j_num(c, "anchorY", 0.0);
    j.connected_anchor[0] = (float)j_num(c, "connAnchorX", 0.0);
    j.connected_anchor[1] = (float)j_num(c, "connAnchorY", 0.0);
    j.distance      = (float)j_num(c, "distance", 1.0);
    j.frequency     = (float)j_num(c, "frequency", 1.0);
    j.damping_ratio = (float)j_num(c, "dampingRatio", 0.5);
    j.use_motor          = j_bool(c, "useMotor", false);
    j.motor_speed_deg_s  = (float)j_num(c, "motorSpeed", 0.0);
    j.motor_max_torque   = (float)j_num(c, "motorMaxTorque", 10000.0);
    j.use_limits         = j_bool(c, "useLimits", false);
    j.lower_angle_deg    = (float)j_num(c, "lowerAngle", -90.0);
    j.upper_angle_deg    = (float)j_num(c, "upperAngle",  90.0);
    j.break_force  = (float)j_num(c, "breakForce", 1e30);
    j.break_torque = (float)j_num(c, "breakTorque", 1e30);
    j.enable_collision = j_bool(c, "enableCollision", false);
    j.auto_configure_distance = j_bool(c, "autoConfigureDistance", true);
    jce_scene_set_joint2d(s, e, &j);
}

void parse_constraint(JceScene *s, JceEntity e, const cJSON *c)
{
    JceConstraintComponent cn;
    memset(&cn, 0, sizeof(cn));
    cn.constraint_type  = (int)j_num(c, "constraintType", 0);
    cn.target_entity    = (uint32_t)j_num(c, "targetEntity", 0);
    cn.pivot_a[0] = (float)j_num(c, "pivotAx", 0.0);
    cn.pivot_a[1] = (float)j_num(c, "pivotAy", 0.0);
    cn.pivot_a[2] = (float)j_num(c, "pivotAz", 0.0);
    cn.pivot_b[0] = (float)j_num(c, "pivotBx", 0.0);
    cn.pivot_b[1] = (float)j_num(c, "pivotBy", 0.0);
    cn.pivot_b[2] = (float)j_num(c, "pivotBz", 0.0);
    cn.axis[0]    = (float)j_num(c, "axisX", 0.0);
    cn.axis[1]    = (float)j_num(c, "axisY", 1.0);
    cn.axis[2]    = (float)j_num(c, "axisZ", 0.0);
    cn.lower_limit = (float)j_num(c, "lowerLimit", 0.0);
    cn.upper_limit = (float)j_num(c, "upperLimit", 0.0);
    cn.disable_collision = j_bool(c, "disableCollision", false);
    cn.use_motor             = j_bool(c, "useMotor", false);
    cn.motor_target_velocity = (float)j_num(c, "motorTargetVelocity", 0.0);
    cn.motor_max_force       = (float)j_num(c, "motorMaxForce", 0.0);
    jce_scene_set_constraint(s, e, &cn);
}

static void ser_rigidbody(const JceRigidBodyComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Rigidbody");
    cJSON_AddNumberToObject(o, "mass", c->mass);
    cJSON_AddNumberToObject(o, "drag", c->drag);
    cJSON_AddNumberToObject(o, "angularDrag", c->angular_drag);
    cJSON_AddBoolToObject(o, "useGravity", c->use_gravity);
    cJSON_AddBoolToObject(o, "isKinematic", c->is_kinematic);
    if (c->freeze_rotation) cJSON_AddBoolToObject(o, "freezeRotation", c->freeze_rotation);
    cJSON_AddNumberToObject(o, "friction", c->friction);
    cJSON_AddNumberToObject(o, "restitution", c->restitution);
    /* CCD (P3-C.3) — only emit when non-default to keep diffs small. */
    if (c->ccd_mode != 0)
        cJSON_AddNumberToObject(o, "ccdMode", (double)c->ccd_mode);
    if (c->ccd_threshold > 0.0f)
        cJSON_AddNumberToObject(o, "ccdThreshold", c->ccd_threshold);
    if (c->ccd_sphere_radius > 0.0f)
        cJSON_AddNumberToObject(o, "ccdSphereRadius", c->ccd_sphere_radius);
    /* Per-body gravity / layer / material — emit only when non-default. */
    if (c->gravity_scale != 1.0f)
        cJSON_AddNumberToObject(o, "gravityScale", c->gravity_scale);
    if (c->physics_layer != 0)
        cJSON_AddNumberToObject(o, "physicsLayer", (double)c->physics_layer);
    if (c->physmat_path[0])
        cJSON_AddStringToObject(o, "physMaterial", c->physmat_path);
    /* Emitted only when non-default, like the four above, so every scene
     * written before this re-saves byte-identically.  0 == JCE_SHAPE_BOX. */
    if (c->shape_type)
        cJSON_AddNumberToObject(o, "shapeType", (double)c->shape_type);
    /* Emitted only when the author picked one, so every scene written before
     * this re-saves byte-identically.  0 is AUTO. */
    if (c->body_type)
        cJSON_AddNumberToObject(o, "bodyType", (double)c->body_type);
    cJSON_AddItemToArray(arr, o);
}

static void ser_rigidbody2d(const JceRigidBody2DComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Rigidbody2D");
    cJSON_AddNumberToObject(o, "bodyType", (double)c->body_type);
    cJSON_AddNumberToObject(o, "shapeType", (double)c->shape_type);
    cJSON_AddNumberToObject(o, "mass", c->mass);
    cJSON_AddNumberToObject(o, "friction", c->friction);
    cJSON_AddNumberToObject(o, "restitution", c->restitution);
    cJSON_AddBoolToObject(o, "fixedRotation", c->fixed_rotation);
    cJSON_AddNumberToObject(o, "physicsLayer", (double)c->physics_layer);
    cJSON_AddItemToArray(arr, o);
}

static void ser_box_collider(const JceBoxColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "BoxCollider");
    cJSON_AddNumberToObject(o, "centerX", c->center[0]);
    cJSON_AddNumberToObject(o, "centerY", c->center[1]);
    cJSON_AddNumberToObject(o, "centerZ", c->center[2]);
    cJSON_AddNumberToObject(o, "sizeX", c->size[0]);
    cJSON_AddNumberToObject(o, "sizeY", c->size[1]);
    cJSON_AddNumberToObject(o, "sizeZ", c->size[2]);
    cJSON_AddBoolToObject(o, "isTrigger", c->is_trigger);
    cJSON_AddItemToArray(arr, o);
}

static void ser_sphere_collider(const JceSphereColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SphereCollider");
    cJSON_AddNumberToObject(o, "centerX", c->center[0]);
    cJSON_AddNumberToObject(o, "centerY", c->center[1]);
    cJSON_AddNumberToObject(o, "centerZ", c->center[2]);
    cJSON_AddNumberToObject(o, "radius", c->radius);
    cJSON_AddBoolToObject(o, "isTrigger", c->is_trigger);
    cJSON_AddItemToArray(arr, o);
}

static void ser_character_controller(const JceCharacterControllerComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "CharacterController");
    cJSON_AddNumberToObject(o, "height", c->height);
    cJSON_AddNumberToObject(o, "radius", c->radius);
    cJSON_AddNumberToObject(o, "stepOffset", c->step_offset);
    cJSON_AddNumberToObject(o, "slopeLimit", c->slope_limit);
    cJSON_AddNumberToObject(o, "moveSpeed", c->move_speed);
    cJSON_AddNumberToObject(o, "sprintMult", c->sprint_mult);
    cJSON_AddNumberToObject(o, "jumpSpeed", c->jump_speed);
    cJSON_AddNumberToObject(o, "accel", c->accel);
    cJSON_AddNumberToObject(o, "airControl", c->air_control);
    cJSON_AddNumberToObject(o, "turnSpeed", c->turn_speed_deg);
    /* Written only when non-default, like RigidBody's: keeps existing
     * scene files byte-identical when nobody moved the character off
     * layer 0.  parse_character_controller defaults it to the same 0, so
     * the round trip is closed in BOTH directions -- a key written by one
     * side and unread by the other is how MeshRenderer.visible silently
     * stopped persisting. */
    if (c->physics_layer != 0)
        cJSON_AddNumberToObject(o, "physicsLayer", (double)c->physics_layer);
    cJSON_AddItemToArray(arr, o);
}

static void ser_constraint(const JceConstraintComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Constraint");
    cJSON_AddNumberToObject(o, "constraintType", c->constraint_type);
    cJSON_AddNumberToObject(o, "targetEntity", (double)c->target_entity);
    cJSON_AddNumberToObject(o, "pivotAx", c->pivot_a[0]);
    cJSON_AddNumberToObject(o, "pivotAy", c->pivot_a[1]);
    cJSON_AddNumberToObject(o, "pivotAz", c->pivot_a[2]);
    cJSON_AddNumberToObject(o, "pivotBx", c->pivot_b[0]);
    cJSON_AddNumberToObject(o, "pivotBy", c->pivot_b[1]);
    cJSON_AddNumberToObject(o, "pivotBz", c->pivot_b[2]);
    cJSON_AddNumberToObject(o, "axisX", c->axis[0]);
    cJSON_AddNumberToObject(o, "axisY", c->axis[1]);
    cJSON_AddNumberToObject(o, "axisZ", c->axis[2]);
    cJSON_AddNumberToObject(o, "lowerLimit", c->lower_limit);
    cJSON_AddNumberToObject(o, "upperLimit", c->upper_limit);
    cJSON_AddBoolToObject(o, "disableCollision", c->disable_collision);
    cJSON_AddBoolToObject(o, "useMotor", c->use_motor);
    cJSON_AddNumberToObject(o, "motorTargetVelocity", c->motor_target_velocity);
    cJSON_AddNumberToObject(o, "motorMaxForce", c->motor_max_force);
    cJSON_AddItemToArray(arr, o);
}

static void ser_buoyancy(const JceBuoyancyComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Buoyancy");
    cJSON_AddNumberToObject(o, "buoyancyStrength", c->buoyancy_strength);
    cJSON_AddNumberToObject(o, "drag", c->drag);
    cJSON_AddBoolToObject  (o, "enabled", c->enabled);
    cJSON_AddItemToArray(arr, o);
}

static void ser_trigger_volume(const JceTriggerVolumeComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "TriggerVolume");
    cJSON_AddNumberToObject(o, "shape", c->shape);
    cJSON_AddNumberToObject(o, "cx", c->center[0]);
    cJSON_AddNumberToObject(o, "cy", c->center[1]);
    cJSON_AddNumberToObject(o, "cz", c->center[2]);
    cJSON_AddNumberToObject(o, "hx", c->half_extents[0]);
    cJSON_AddNumberToObject(o, "hy", c->half_extents[1]);
    cJSON_AddNumberToObject(o, "hz", c->half_extents[2]);
    cJSON_AddNumberToObject(o, "axX", c->axis_x[0]); cJSON_AddNumberToObject(o, "axY", c->axis_x[1]); cJSON_AddNumberToObject(o, "axZ", c->axis_x[2]);
    cJSON_AddNumberToObject(o, "ayX", c->axis_y[0]); cJSON_AddNumberToObject(o, "ayY", c->axis_y[1]); cJSON_AddNumberToObject(o, "ayZ", c->axis_y[2]);
    cJSON_AddNumberToObject(o, "azX", c->axis_z[0]); cJSON_AddNumberToObject(o, "azY", c->axis_z[1]); cJSON_AddNumberToObject(o, "azZ", c->axis_z[2]);
    cJSON_AddBoolToObject  (o, "enabled",   c->enabled);
    cJSON_AddBoolToObject  (o, "fireStay",  c->fire_stay);
    cJSON_AddStringToObject(o, "tag",       c->tag);
    cJSON_AddItemToArray(arr, o);
}

static void ser_capsule_collider(const JceCapsuleColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "CapsuleCollider");
    cJSON_AddNumberToObject(o, "cx", c->center[0]);
    cJSON_AddNumberToObject(o, "cy", c->center[1]);
    cJSON_AddNumberToObject(o, "cz", c->center[2]);
    cJSON_AddNumberToObject(o, "radius", c->radius);
    cJSON_AddNumberToObject(o, "height", c->height);
    cJSON_AddNumberToObject(o, "axis",   c->axis);
    cJSON_AddBoolToObject  (o, "isTrigger", c->is_trigger);
    cJSON_AddItemToArray(arr, o);
}

static void ser_mesh_collider(const JceMeshColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "MeshCollider");
    cJSON_AddStringToObject(o, "meshPath", c->mesh_path);
    cJSON_AddBoolToObject  (o, "convex",    c->convex);
    cJSON_AddBoolToObject  (o, "isTrigger", c->is_trigger);
    cJSON_AddNumberToObject(o, "friction",    c->friction);
    cJSON_AddNumberToObject(o, "restitution", c->restitution);
    cJSON_AddItemToArray(arr, o);
}

static void ser_compound_collider(const JceCompoundColliderComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "CompoundCollider");
    cJSON_AddStringToObject(o, "modelPath", c->model_path);
    cJSON_AddNumberToObject(o, "mode",  c->mode);
    cJSON_AddNumberToObject(o, "split", c->split);
    cJSON_AddBoolToObject  (o, "isStatic",     c->is_static);
    cJSON_AddBoolToObject  (o, "detectNaming", c->detect_naming);
    cJSON_AddBoolToObject  (o, "isTrigger",    c->is_trigger);
    cJSON_AddNumberToObject(o, "friction",    c->friction);
    cJSON_AddNumberToObject(o, "restitution", c->restitution);
    cJSON_AddNumberToObject(o, "vhacdResolution",      c->vhacd_resolution);
    cJSON_AddNumberToObject(o, "vhacdMaxHulls",        c->vhacd_max_hulls);
    cJSON_AddNumberToObject(o, "vhacdMaxVertsPerHull", c->vhacd_max_verts_per_hull);
    if (c->physmat_path[0])
        cJSON_AddStringToObject(o, "physMaterial", c->physmat_path);
    cJSON_AddItemToArray(arr, o);
}

static void ser_collider2d(const JceCollider2DComponent *c, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Collider2D");
    cJSON_AddNumberToObject(o, "shape",      c->shape);
    cJSON_AddNumberToObject(o, "offX",       c->offset[0]);
    cJSON_AddNumberToObject(o, "offY",       c->offset[1]);
    cJSON_AddNumberToObject(o, "sizeX",      c->size[0]);
    cJSON_AddNumberToObject(o, "sizeY",      c->size[1]);
    cJSON_AddNumberToObject(o, "radius",     c->radius);
    cJSON_AddNumberToObject(o, "capsuleDir", c->capsule_direction);
    cJSON_AddBoolToObject  (o, "isTrigger",  c->is_trigger);
    cJSON_AddNumberToObject(o, "friction",   c->friction);
    cJSON_AddNumberToObject(o, "restitution",c->restitution);
    int n = c->point_count;
    if (n < 0) n = 0; if (n > JCE_COLLIDER_2D_MAX_POINTS) n = JCE_COLLIDER_2D_MAX_POINTS;
    cJSON_AddNumberToObject(o, "pointCount", n);
    char key[24];
    for (int i = 0; i < n; ++i) {
        snprintf(key, sizeof(key), "px%d", i);
        cJSON_AddNumberToObject(o, key, c->points[i][0]);
        snprintf(key, sizeof(key), "py%d", i);
        cJSON_AddNumberToObject(o, key, c->points[i][1]);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_wheel_collider(const JceWheelColliderComponent *w, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "WheelCollider");
    cJSON_AddNumberToObject(o, "radius", w->radius);
    cJSON_AddNumberToObject(o, "suspensionDistance",  w->suspension_distance);
    cJSON_AddNumberToObject(o, "suspensionSpring",    w->suspension_spring);
    cJSON_AddNumberToObject(o, "suspensionDamper",    w->suspension_damper);
    cJSON_AddNumberToObject(o, "suspensionTargetPos", w->suspension_target_pos);
    cJSON_AddNumberToObject(o, "mass",                w->mass);
    cJSON_AddNumberToObject(o, "forwardFriction",     w->forward_friction);
    cJSON_AddNumberToObject(o, "sidewaysFriction",    w->sideways_friction);
    cJSON_AddNumberToObject(o, "centerX", w->center[0]);
    cJSON_AddNumberToObject(o, "centerY", w->center[1]);
    cJSON_AddNumberToObject(o, "centerZ", w->center[2]);
    cJSON_AddNumberToObject(o, "motorTorque",   w->motor_torque);
    cJSON_AddNumberToObject(o, "brakeTorque",   w->brake_torque);
    cJSON_AddNumberToObject(o, "steerAngleDeg", w->steer_angle_deg);
    cJSON_AddItemToArray(arr, o);
}

static void ser_constant_force(const JceConstantForceComponent *f, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ConstantForce");
    cJSON_AddNumberToObject(o, "forceX", f->force[0]);
    cJSON_AddNumberToObject(o, "forceY", f->force[1]);
    cJSON_AddNumberToObject(o, "forceZ", f->force[2]);
    cJSON_AddNumberToObject(o, "relForceX", f->relative_force[0]);
    cJSON_AddNumberToObject(o, "relForceY", f->relative_force[1]);
    cJSON_AddNumberToObject(o, "relForceZ", f->relative_force[2]);
    cJSON_AddNumberToObject(o, "torqueX", f->torque[0]);
    cJSON_AddNumberToObject(o, "torqueY", f->torque[1]);
    cJSON_AddNumberToObject(o, "torqueZ", f->torque[2]);
    cJSON_AddNumberToObject(o, "relTorqueX", f->relative_torque[0]);
    cJSON_AddNumberToObject(o, "relTorqueY", f->relative_torque[1]);
    cJSON_AddNumberToObject(o, "relTorqueZ", f->relative_torque[2]);
    cJSON_AddBoolToObject  (o, "enabled", f->enabled);
    cJSON_AddItemToArray(arr, o);
}

static void ser_ragdoll(const JceRagdollComponent *rc, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Ragdoll");
    cJSON_AddBoolToObject(o, "enable", rc->enable);
    cJSON_AddNumberToObject(o, "blendWeight", (double)rc->blend_weight);
    cJSON_AddNumberToObject(o, "radius", (double)rc->radius);
    cJSON_AddNumberToObject(o, "heightScale", (double)rc->height_scale);
    cJSON_AddNumberToObject(o, "jointLimitScale", (double)rc->joint_limit_scale);
    cJSON_AddItemToArray(arr, o);
}

static void ser_fracture(const JceFractureComponent *fc, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Fracture");
    cJSON_AddBoolToObject(o, "enabled", fc->enabled);
    cJSON_AddNumberToObject(o, "fragmentCount", (double)fc->fragment_count);
    cJSON_AddNumberToObject(o, "breakImpulse", (double)fc->break_impulse);
    cJSON_AddNumberToObject(o, "density", (double)fc->density);
    cJSON_AddNumberToObject(o, "seed", (double)fc->seed);
    cJSON_AddItemToArray(arr, o);
}

static void ser_vehicle(const JceVehicleComponent *vc, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Vehicle");
    cJSON_AddBoolToObject(o, "enabled", vc->enabled);
    cJSON_AddNumberToObject(o, "chassisHalfX", (double)vc->chassis_half_extents[0]);
    cJSON_AddNumberToObject(o, "chassisHalfY", (double)vc->chassis_half_extents[1]);
    cJSON_AddNumberToObject(o, "chassisHalfZ", (double)vc->chassis_half_extents[2]);
    cJSON_AddNumberToObject(o, "chassisMass", (double)vc->chassis_mass);
    cJSON_AddNumberToObject(o, "maxEngineForce", (double)vc->max_engine_force);
    cJSON_AddNumberToObject(o, "maxBrakeForce", (double)vc->max_brake_force);
    cJSON_AddNumberToObject(o, "maxSteeringDeg", (double)vc->max_steering_deg);
    cJSON_AddNumberToObject(o, "driveMode", (double)vc->drive_mode);
    cJSON_AddNumberToObject(o, "inputMode", (double)vc->input_mode);
    cJSON_AddItemToArray(arr, o);
}

static void ser_soft_body(const JceSoftBodyComponent *sc, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "SoftBody");
    cJSON_AddBoolToObject(o, "enabled", sc->enabled);
    cJSON_AddNumberToObject(o, "radiusX", (double)sc->radius[0]);
    cJSON_AddNumberToObject(o, "radiusY", (double)sc->radius[1]);
    cJSON_AddNumberToObject(o, "radiusZ", (double)sc->radius[2]);
    cJSON_AddNumberToObject(o, "mass", (double)sc->mass);
    cJSON_AddNumberToObject(o, "pressure", (double)sc->pressure);
    cJSON_AddNumberToObject(o, "stiffnessLinear", (double)sc->stiffness_linear);
    cJSON_AddNumberToObject(o, "stiffnessVolume", (double)sc->stiffness_volume);
    cJSON_AddNumberToObject(o, "damping", (double)sc->damping);
    cJSON_AddNumberToObject(o, "friction", (double)sc->friction);
    cJSON_AddNumberToObject(o, "resolution", (double)sc->resolution);
    cJSON_AddBoolToObject(o, "selfCollision", sc->self_collision);
    cJSON_AddItemToArray(arr, o);
}

static void ser_configurable_joint(const JceConfigurableJointComponent *j, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "ConfigurableJoint");
    cJSON_AddNumberToObject(o, "connectedBody", (double)j->connected_body);
    cJSON_AddNumberToObject(o, "anchorX", j->anchor[0]);
    cJSON_AddNumberToObject(o, "anchorY", j->anchor[1]);
    cJSON_AddNumberToObject(o, "anchorZ", j->anchor[2]);
    cJSON_AddNumberToObject(o, "connAnchorX", j->connected_anchor[0]);
    cJSON_AddNumberToObject(o, "connAnchorY", j->connected_anchor[1]);
    cJSON_AddNumberToObject(o, "connAnchorZ", j->connected_anchor[2]);
    cJSON_AddNumberToObject(o, "xMotion", j->x_motion);
    cJSON_AddNumberToObject(o, "yMotion", j->y_motion);
    cJSON_AddNumberToObject(o, "zMotion", j->z_motion);
    cJSON_AddNumberToObject(o, "xRotation", j->x_rotation);
    cJSON_AddNumberToObject(o, "yRotation", j->y_rotation);
    cJSON_AddNumberToObject(o, "zRotation", j->z_rotation);
    cJSON_AddNumberToObject(o, "linearLimit", j->linear_limit);
    cJSON_AddNumberToObject(o, "angXLimit",   j->angular_x_limit_deg);
    cJSON_AddNumberToObject(o, "angYLimit",   j->angular_y_limit_deg);
    cJSON_AddNumberToObject(o, "angZLimit",   j->angular_z_limit_deg);
    cJSON_AddNumberToObject(o, "breakForce",  j->break_force);
    cJSON_AddNumberToObject(o, "breakTorque", j->break_torque);
    cJSON_AddBoolToObject  (o, "enableCollision", j->enable_collision);
    /* EVERY axis, every time, including the OFF ones.  Writing only the
     * driven axes would make a drive that is switched off in the Inspector
     * indistinguishable in the file from one that was never authored -- and
     * the author who switched it off would find their spring stiffness gone
     * when they switched it back on. */
    for (int d = 0; d < 6; ++d) {
        char key[24];
        snprintf(key, sizeof key, "driveMode%d", d);
        cJSON_AddNumberToObject(o, key, j->drive_mode[d]);
        snprintf(key, sizeof key, "driveTarget%d", d);
        cJSON_AddNumberToObject(o, key, j->drive_target[d]);
        snprintf(key, sizeof key, "driveSpring%d", d);
        cJSON_AddNumberToObject(o, key, j->drive_spring[d]);
        snprintf(key, sizeof key, "driveDamper%d", d);
        cJSON_AddNumberToObject(o, key, j->drive_damper[d]);
        snprintf(key, sizeof key, "driveMaxForce%d", d);
        cJSON_AddNumberToObject(o, key, j->drive_max_force[d]);
    }
    cJSON_AddItemToArray(arr, o);
}

static void ser_cloth(const JceClothComponent *cl, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Cloth");
    cJSON_AddNumberToObject(o, "c00X", cl->corner_00.x);
    cJSON_AddNumberToObject(o, "c00Y", cl->corner_00.y);
    cJSON_AddNumberToObject(o, "c00Z", cl->corner_00.z);
    cJSON_AddNumberToObject(o, "c10X", cl->corner_10.x);
    cJSON_AddNumberToObject(o, "c10Y", cl->corner_10.y);
    cJSON_AddNumberToObject(o, "c10Z", cl->corner_10.z);
    cJSON_AddNumberToObject(o, "c01X", cl->corner_01.x);
    cJSON_AddNumberToObject(o, "c01Y", cl->corner_01.y);
    cJSON_AddNumberToObject(o, "c01Z", cl->corner_01.z);
    cJSON_AddNumberToObject(o, "c11X", cl->corner_11.x);
    cJSON_AddNumberToObject(o, "c11Y", cl->corner_11.y);
    cJSON_AddNumberToObject(o, "c11Z", cl->corner_11.z);
    cJSON_AddNumberToObject(o, "resU",     cl->res_u);
    cJSON_AddNumberToObject(o, "resV",     cl->res_v);
    cJSON_AddNumberToObject(o, "mass",     cl->mass_total);
    cJSON_AddNumberToObject(o, "stiffLin", cl->stiffness_linear);
    cJSON_AddNumberToObject(o, "stiffAng", cl->stiffness_angular);
    cJSON_AddNumberToObject(o, "damping",  cl->damping);
    cJSON_AddNumberToObject(o, "iters",    cl->iterations);
    cJSON_AddBoolToObject  (o, "selfColl", cl->self_collision);
    cJSON_AddBoolToObject  (o, "windOn",   cl->wind_enabled);
    cJSON_AddNumberToObject(o, "windX",    cl->wind_velocity.x);
    cJSON_AddNumberToObject(o, "windY",    cl->wind_velocity.y);
    cJSON_AddNumberToObject(o, "windZ",    cl->wind_velocity.z);
    cJSON *pin = cJSON_CreateArray();
    uint32_t pc = cl->pinned_count;
    if (pc > JCE_CLOTH_MAX_PINNED) pc = JCE_CLOTH_MAX_PINNED;
    for (uint32_t i = 0; i < pc; ++i)
        cJSON_AddItemToArray(pin, cJSON_CreateNumber((double)cl->pinned_indices[i]));
    cJSON_AddItemToObject(o, "pinned", pin);
    cJSON_AddItemToArray(arr, o);
}

static void ser_joint2d(const JceJoint2DComponent *j, cJSON *arr)
{
    cJSON *o = cJSON_CreateObject();
    cJSON_AddStringToObject(o, "type", "Joint2D");
    cJSON_AddNumberToObject(o, "kind", j->kind);
    cJSON_AddNumberToObject(o, "connectedBody", (double)j->connected_body);
    cJSON_AddNumberToObject(o, "anchorX", j->anchor[0]);
    cJSON_AddNumberToObject(o, "anchorY", j->anchor[1]);
    cJSON_AddNumberToObject(o, "connAnchorX", j->connected_anchor[0]);
    cJSON_AddNumberToObject(o, "connAnchorY", j->connected_anchor[1]);
    cJSON_AddNumberToObject(o, "distance",     j->distance);
    cJSON_AddNumberToObject(o, "frequency",    j->frequency);
    cJSON_AddNumberToObject(o, "dampingRatio", j->damping_ratio);
    cJSON_AddBoolToObject  (o, "useMotor",        j->use_motor);
    cJSON_AddNumberToObject(o, "motorSpeed",      j->motor_speed_deg_s);
    cJSON_AddNumberToObject(o, "motorMaxTorque",  j->motor_max_torque);
    cJSON_AddBoolToObject  (o, "useLimits",       j->use_limits);
    cJSON_AddNumberToObject(o, "lowerAngle",      j->lower_angle_deg);
    cJSON_AddNumberToObject(o, "upperAngle",      j->upper_angle_deg);
    cJSON_AddNumberToObject(o, "breakForce",  j->break_force);
    cJSON_AddNumberToObject(o, "breakTorque", j->break_torque);
    cJSON_AddBoolToObject  (o, "enableCollision",       j->enable_collision);
    cJSON_AddBoolToObject  (o, "autoConfigureDistance", j->auto_configure_distance);
    cJSON_AddItemToArray(arr, o);
}

void serw_rigidbody(JceScene *s, JceEntity e, cJSON *arr)
{
    JceRigidBodyComponent *c = jce_scene_get_rigidbody(s, e);
    if (c) ser_rigidbody(c, arr);
}

void serw_rigidbody2d(JceScene *s, JceEntity e, cJSON *arr)
{
    JceRigidBody2DComponent *c = jce_scene_get_rigidbody2d(s, e);
    if (c) ser_rigidbody2d(c, arr);
}

void serw_box_collider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceBoxColliderComponent *c = jce_scene_get_box_collider(s, e);
    if (c) ser_box_collider(c, arr);
}

void serw_sphere_collider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSphereColliderComponent *c = jce_scene_get_sphere_collider(s, e);
    if (c) ser_sphere_collider(c, arr);
}

void serw_character_controller(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCharacterControllerComponent *c = jce_scene_get_character_controller(s, e);
    if (c) ser_character_controller(c, arr);
}

void serw_ragdoll(JceScene *s, JceEntity e, cJSON *arr)
{
    JceRagdollComponent *c = jce_scene_get_ragdoll(s, e);
    if (c) ser_ragdoll(c, arr);
}

void serw_fracture(JceScene *s, JceEntity e, cJSON *arr)
{
    JceFractureComponent *c = jce_scene_get_fracture(s, e);
    if (c) ser_fracture(c, arr);
}

void serw_vehicle(JceScene *s, JceEntity e, cJSON *arr)
{
    JceVehicleComponent *c = jce_scene_get_vehicle(s, e);
    if (c) ser_vehicle(c, arr);
}

void serw_soft_body(JceScene *s, JceEntity e, cJSON *arr)
{
    JceSoftBodyComponent *c = jce_scene_get_soft_body(s, e);
    if (c) ser_soft_body(c, arr);
}

void serw_constraint(JceScene *s, JceEntity e, cJSON *arr)
{
    JceConstraintComponent *c = jce_scene_get_constraint(s, e);
    if (c) ser_constraint(c, arr);
}

void serw_buoyancy(JceScene *s, JceEntity e, cJSON *arr)
{
    JceBuoyancyComponent *c = jce_scene_get_buoyancy(s, e);
    if (c) ser_buoyancy(c, arr);
}

void serw_trigger_volume(JceScene *s, JceEntity e, cJSON *arr)
{
    JceTriggerVolumeComponent *c = jce_scene_get_trigger_volume(s, e);
    if (c) ser_trigger_volume(c, arr);
}

void serw_capsule_collider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCapsuleColliderComponent *c = jce_scene_get_capsule_collider(s, e);
    if (c) ser_capsule_collider(c, arr);
}

void serw_mesh_collider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceMeshColliderComponent *c = jce_scene_get_mesh_collider(s, e);
    if (c) ser_mesh_collider(c, arr);
}

void serw_compound_collider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCompoundColliderComponent *c = jce_scene_get_compound_collider(s, e);
    if (c) ser_compound_collider(c, arr);
}

void serw_collider2d(JceScene *s, JceEntity e, cJSON *arr)
{
    JceCollider2DComponent *c = jce_scene_get_collider2d(s, e);
    if (c) ser_collider2d(c, arr);
}

void serw_wheel_collider(JceScene *s, JceEntity e, cJSON *arr)
{
    JceWheelColliderComponent *c = jce_scene_get_wheel_collider(s, e);
    if (c) ser_wheel_collider(c, arr);
}

void serw_constant_force(JceScene *s, JceEntity e, cJSON *arr)
{
    JceConstantForceComponent *c = jce_scene_get_constant_force(s, e);
    if (c) ser_constant_force(c, arr);
}

void serw_configurable_joint(JceScene *s, JceEntity e, cJSON *arr)
{
    JceConfigurableJointComponent *c = jce_scene_get_configurable_joint(s, e);
    if (c) ser_configurable_joint(c, arr);
}

void serw_cloth(JceScene *s, JceEntity e, cJSON *arr)
{
    JceClothComponent *c = jce_scene_get_cloth(s, e);
    if (c) ser_cloth(c, arr);
}

void serw_joint2d(JceScene *s, JceEntity e, cJSON *arr)
{
    JceJoint2DComponent *c = jce_scene_get_joint2d(s, e);
    if (c) ser_joint2d(c, arr);
}

