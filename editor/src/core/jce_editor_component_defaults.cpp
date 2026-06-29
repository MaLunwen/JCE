/*
 * jce_editor_component_defaults.cpp
 *
 * Per-component default-init (Add Component), duplicate fixups and
 * pre-remove cleanup hooks, registered into the editor component
 * registry rows.  Bodies are the exact special-case blocks that used to
 * live in jce_state_add_component()'s switch, inspector_add_component's
 * synthetic-slot branches, jce_state_remove_component()'s cloth case and
 * duplicate_components()'s particle-emitter fixup — adding a future
 * component means one engine REG row, one editor descriptor row and one
 * adddef_* function here.
 */

#include "jce_editor_component_registry.h"

extern "C" {
#include <jce/middleware/physics/jce_cloth.h>
}

#include <stdio.h>
#include <string.h>

namespace {

/* Replication keys on NetworkObject (rt_spawn_net skips entities without
 * it), so adding any Net* component auto-adds the gatekeeper too —
 * otherwise the authored Net* data is silently inert at runtime. */
void ensure_network_object(JceScene *scene, JceEntity e)
{
    if (!scene || jce_scene_has_network_object(scene, e)) return;
    JceNetworkObjectComponent no; memset(&no, 0, sizeof no);
    jce_scene_set_network_object(scene, e, &no);
}

/* ── Add-Component default initializers ───────────────────────────── */

void adddef_transform(JceScene *scene, JceEntity e)
{
    JceTransform t;
    t.position = jce_v3(0.0f, 0.0f, 0.0f);
    t.rotation = jce_q_identity();
    t.scale    = jce_v3(1.0f, 1.0f, 1.0f);
    jce_scene_set_transform(scene, e, &t);
}

void adddef_pivot(JceScene *scene, JceEntity e)
{
    JcePivotComponent p;
    memset(&p, 0, sizeof(p));
    p.local_rotation = jce_q_identity();
    jce_scene_set_pivot(scene, e, &p);
}

void adddef_mesh_renderer(JceScene *scene, JceEntity e)
{
    JceMeshRenderer mr;
    memset(&mr, 0, sizeof(mr));
    mr.visible        = true;
    mr.base_color[0]  = 1.0f;
    mr.base_color[1]  = 1.0f;
    mr.base_color[2]  = 1.0f;
    mr.base_color[3]  = 1.0f;
    mr.metallic       = 0.0f;
    mr.roughness      = 0.5f;
    mr.normal_scale   = 1.0f;
    mr.ao_strength    = 1.0f;
    mr.alpha_cutoff   = 0.5f;
    jce_scene_set_mesh_renderer(scene, e, &mr);
}

void adddef_camera(JceScene *scene, JceEntity e)
{
    JceCameraComponent c;
    memset(&c, 0, sizeof(c));
    c.fov_deg    = 60.0f;
    c.near_plane = 0.1f;
    c.far_plane  = 1000.0f;
    c.is_primary = false;
    c.ortho      = false;
    jce_scene_set_camera(scene, e, &c);
}

void adddef_dir_light(JceScene *scene, JceEntity e)
{
    JceDirectionalLight l;
    memset(&l, 0, sizeof(l));
    l.direction    = jce_v3(0.0f, -1.0f, 0.0f);
    l.color        = jce_v3(1.0f, 1.0f, 1.0f);
    l.intensity    = 1.0f;
    l.casts_shadow = true;
    l.cookie_texture.idx = UINT16_MAX;
    jce_scene_set_dir_light(scene, e, &l);
}

void adddef_point_light(JceScene *scene, JceEntity e)
{
    JcePointLight l;
    memset(&l, 0, sizeof(l));
    l.position  = jce_v3(0.0f, 0.0f, 0.0f);
    l.color     = jce_v3(1.0f, 1.0f, 1.0f);
    l.intensity = 1.0f;
    l.radius    = 10.0f;
    jce_scene_set_point_light(scene, e, &l);
}

void adddef_spot_light(JceScene *scene, JceEntity e)
{
    JceSpotLight l;
    memset(&l, 0, sizeof(l));
    l.position       = jce_v3(0.0f, 0.0f, 0.0f);
    l.direction      = jce_v3(0.0f, -1.0f, 0.0f);
    l.color          = jce_v3(1.0f, 1.0f, 1.0f);
    l.intensity      = 1.0f;
    l.radius         = 10.0f;
    l.inner_cone_cos = 0.95f;
    l.outer_cone_cos = 0.85f;
    l.cookie_texture.idx = UINT16_MAX;
    l.ies_lut_texture.idx = UINT16_MAX;
    jce_scene_set_spot_light(scene, e, &l);
}

void adddef_skybox(JceScene *scene, JceEntity e)
{
    JceSkyboxComponent c;
    memset(&c, 0, sizeof(c));
    c.exposure   = 1.0f;
    c.use_as_ibl = true;
    jce_scene_set_skybox(scene, e, &c);
}

void adddef_sprite_renderer(JceScene *scene, JceEntity e)
{
    JceSpriteRendererComponent c;
    memset(&c, 0, sizeof(c));
    c.color[0] = 1.0f; c.color[1] = 1.0f;
    c.color[2] = 1.0f; c.color[3] = 1.0f;
    jce_scene_set_sprite_renderer(scene, e, &c);
}

void adddef_sprite_animator(JceScene *scene, JceEntity e)
{
    JceSpriteAnimatorComponent c;
    memset(&c, 0, sizeof(c));
    c.frame_width  = 64;
    c.frame_height = 64;
    c.speed        = 1.0f;
    c.loop         = true;
    jce_scene_set_sprite_animator(scene, e, &c);
}

void adddef_animator(JceScene *scene, JceEntity e)
{
    JceAnimatorComponent c;
    memset(&c, 0, sizeof(c));
    c.speed = 1.0f;
    c.loop  = true;
    jce_scene_set_animator(scene, e, &c);
}

void adddef_skeletal_animator(JceScene *scene, JceEntity e)
{
    JceSkeletalAnimatorComponent c;
    memset(&c, 0, sizeof(c));
    c.speed = 1.0f;
    c.loop  = true;
    jce_scene_set_skeletal_animator(scene, e, &c);
}

void adddef_constraint(JceScene *scene, JceEntity e)
{
    JceConstraintComponent c;
    memset(&c, 0, sizeof(c));
    jce_scene_set_constraint(scene, e, &c);
}

void adddef_rigidbody(JceScene *scene, JceEntity e)
{
    JceRigidBodyComponent c;
    memset(&c, 0, sizeof(c));
    c.mass          = 1.0f;
    c.friction      = 0.5f;
    c.restitution   = 0.0f;
    c.use_gravity   = true;
    c.gravity_scale = 1.0f;
    jce_scene_set_rigidbody(scene, e, &c);
}

void adddef_box_collider(JceScene *scene, JceEntity e)
{
    JceBoxColliderComponent c;
    memset(&c, 0, sizeof(c));
    c.size[0] = 1.0f; c.size[1] = 1.0f; c.size[2] = 1.0f;
    jce_scene_set_box_collider(scene, e, &c);
}

void adddef_sphere_collider(JceScene *scene, JceEntity e)
{
    JceSphereColliderComponent c;
    memset(&c, 0, sizeof(c));
    c.radius = 0.5f;
    jce_scene_set_sphere_collider(scene, e, &c);
}

void adddef_character_controller(JceScene *scene, JceEntity e)
{
    JceCharacterControllerComponent c;
    memset(&c, 0, sizeof(c));
    c.height      = 2.0f;
    c.radius      = 0.3f;
    c.step_offset = 0.35f;
    c.slope_limit = 45.0f;
    c.move_speed     = 4.0f;
    c.sprint_mult    = 1.8f;
    c.jump_speed     = 5.0f;
    c.accel          = 40.0f;
    c.air_control    = 0.35f;
    c.turn_speed_deg = 720.0f;
    jce_scene_set_character_controller(scene, e, &c);
}

void adddef_audio_source(JceScene *scene, JceEntity e)
{
    JceAudioSourceComponent c;
    memset(&c, 0, sizeof(c));
    c.volume = 1.0f;
    c.pitch  = 1.0f;
    jce_scene_set_audio_source(scene, e, &c);
}

void adddef_music_track(JceScene *scene, JceEntity e)
{
    JceMusicTrackComponent c;
    memset(&c, 0, sizeof(c));
    c.play_on_awake     = true;
    c.initial_intensity = 0.0f;
    c.bpm               = 120;
    jce_scene_set_music_track(scene, e, &c);
}

void adddef_script(JceScene *scene, JceEntity e)
{
    JceScriptComponent c;
    memset(&c, 0, sizeof(c));
    jce_scene_set_script(scene, e, &c);
}

void adddef_terrain(JceScene *scene, JceEntity e)
{
    JceTerrainComponent c;
    memset(&c, 0, sizeof(c));
    c.tint[0] = c.tint[1] = c.tint[2] = 1.0f;
    c.visible = true;
    c.tile_scale = 10.0f;
    c.splat_enabled = true;
    jce_scene_set_terrain(scene, e, &c);
}

void adddef_vegetation_scatter(JceScene *scene, JceEntity e)
{
    JceVegetationScatterComponent c;
    memset(&c, 0, sizeof(c));
    c.density       = 1.0f;
    c.seed          = 12345u;
    c.area_x        = 10.0f;
    c.area_z        = 10.0f;
    c.max_slope_deg = 45.0f;
    c.scale_min     = 0.8f;
    c.scale_max     = 1.2f;
    c.tint[0] = c.tint[1] = c.tint[2] = 1.0f;
    c.cast_shadow   = true;
    c.visible       = true;
    jce_scene_set_vegetation_scatter(scene, e, &c);
}

void adddef_water(JceScene *scene, JceEntity e)
{
    JceWaterComponent c;
    memset(&c, 0, sizeof(c));
    c.size_x      = 100.0f;
    c.size_z      = 100.0f;
    c.wave_count  = 2;
    c.base_height = 0.0f;
    /* Two crossing Gerstner waves -> a believable default chop. */
    c.waves[0].amplitude  = 0.5f;
    c.waves[0].wavelength = 12.0f;
    c.waves[0].speed      = 2.0f;
    c.waves[0].dir_x      = 1.0f;
    c.waves[0].dir_z      = 0.0f;
    c.waves[0].steepness  = 0.5f;
    c.waves[1].amplitude  = 0.25f;
    c.waves[1].wavelength = 6.0f;
    c.waves[1].speed      = 1.5f;
    c.waves[1].dir_x      = 0.6f;
    c.waves[1].dir_z      = 0.8f;
    c.waves[1].steepness  = 0.4f;
    c.color_shallow[0] = 0.1f; c.color_shallow[1] = 0.4f; c.color_shallow[2] = 0.5f;
    c.color_deep[0]    = 0.0f; c.color_deep[1]    = 0.1f; c.color_deep[2]    = 0.2f;
    c.transparency = 0.5f;
    c.sun_specular = 1.0f;
    c.visible      = true;
    jce_scene_set_water(scene, e, &c);
}

void adddef_buoyancy(JceScene *scene, JceEntity e)
{
    JceBuoyancyComponent c;
    memset(&c, 0, sizeof(c));
    /* Mass-independent defaults: enough lift to float a crate-sized dynamic
     * body and enough drag that it settles (oscillates then damps) instead of
     * pogo-ing.  Needs a dynamic RigidBody + collider on the same entity. */
    c.buoyancy_strength = 20.0f;
    c.drag              = 1.0f;
    c.enabled           = true;
    jce_scene_set_buoyancy(scene, e, &c);
}

void adddef_rigidbody2d(JceScene *scene, JceEntity e)
{
    JceRigidBody2DComponent c;
    memset(&c, 0, sizeof(c));
    c.mass = 1.0f;
    c.friction = 0.5f;
    c.restitution = 0.0f;
    c.fixed_rotation = false;
    jce_scene_set_rigidbody2d(scene, e, &c);
}

void adddef_particle_emitter(JceScene *scene, JceEntity e)
{
    JceParticleEmitterComponent c;
    memset(&c, 0, sizeof(c));
    c.emit_rate    = 10.0f;
    c.lifetime_min = 1.0f;
    c.lifetime_max = 2.0f;
    jce_scene_set_particle_emitter(scene, e, &c);
}

void adddef_behavior_tree(JceScene *scene, JceEntity e)
{
    JceBehaviorTree c;
    memset(&c, 0, sizeof(c));
    c.active = true;
    jce_scene_set_behavior_tree(scene, e, &c);
}

void adddef_lod_group(JceScene *scene, JceEntity e)
{
    JceLodGroupComponent c;
    memset(&c, 0, sizeof(c));
    c.level_count = 3;
    c.distances[0] = 15.0f;
    c.distances[1] = 50.0f;
    c.distances[2] = 150.0f;
    c.hysteresis = 0.05f;
    c.cull_when_too_far = true;
    jce_scene_set_lod_group(scene, e, &c);
}

void adddef_virtual_camera(JceScene *scene, JceEntity e)
{
    JceVirtualCameraComponent c;
    memset(&c, 0, sizeof(c));
    snprintf(c.vcam_name, sizeof(c.vcam_name), "VCam");
    c.priority   = 10;
    /* Inactive by default so an authored vcam does NOT seize the Game-View
     * camera the instant Play starts.  The designer opts in via the inspector
     * "Active" checkbox / Solo, or a Sequencer CAMERA_CUT activates it on cue. */
    c.active     = false;
    c.track_mode = 0;
    c.fov_deg    = 60.0f;
    c.damping    = 0.5f;
    jce_scene_set_virtual_camera(scene, e, &c);
}

void adddef_trigger_volume(JceScene *scene, JceEntity e)
{
    JceTriggerVolumeComponent c;
    memset(&c, 0, sizeof(c));
    c.shape = 0; /* AABB */
    c.half_extents[0] = c.half_extents[1] = c.half_extents[2] = 0.5f;
    c.axis_x[0] = 1.0f; c.axis_y[1] = 1.0f; c.axis_z[2] = 1.0f;
    c.enabled = true;
    c.fire_stay = false;
    jce_scene_set_trigger_volume(scene, e, &c);
}

void adddef_capsule_collider(JceScene *scene, JceEntity e)
{
    JceCapsuleColliderComponent c;
    memset(&c, 0, sizeof(c));
    c.radius = 0.5f;
    c.height = 2.0f;
    c.axis   = 1; /* Y */
    jce_scene_set_capsule_collider(scene, e, &c);
}

void adddef_mesh_collider(JceScene *scene, JceEntity e)
{
    JceMeshColliderComponent c;
    memset(&c, 0, sizeof(c));
    c.friction    = 0.5f;
    c.restitution = 0.0f;
    jce_scene_set_mesh_collider(scene, e, &c);
}

void adddef_compound_collider(JceScene *scene, JceEntity e)
{
    JceCompoundColliderComponent def;
    jce_editor_component_compound_default(&def);
    jce_scene_set_compound_collider(scene, e, &def);
}

void adddef_collider2d(JceScene *scene, JceEntity e)
{
    JceCollider2DComponent c;
    memset(&c, 0, sizeof(c));
    c.shape = 0; /* Box */
    c.size[0] = c.size[1] = 1.0f;
    c.radius  = 0.5f;
    c.friction    = 0.4f;
    c.restitution = 0.0f;
    jce_scene_set_collider2d(scene, e, &c);
}

void adddef_trail_renderer(JceScene *scene, JceEntity e)
{
    JceTrailRendererComponent c;
    memset(&c, 0, sizeof(c));
    c.time = 1.0f;
    c.min_vertex_distance = 0.1f;
    c.width_start = 0.1f;
    c.width_end   = 0.0f;
    c.color_start[0] = c.color_start[1] = c.color_start[2] = c.color_start[3] = 1.0f;
    c.color_end[0]   = c.color_end[1]   = c.color_end[2]   = 1.0f;
    c.color_end[3]   = 0.0f;
    c.emitting = true;
    jce_scene_set_trail_renderer(scene, e, &c);
}

void adddef_line_renderer(JceScene *scene, JceEntity e)
{
    JceLineRendererComponent c;
    memset(&c, 0, sizeof(c));
    c.position_count = 2;
    c.positions[1][0] = 1.0f; /* default 2-point line along +X */
    c.width_start = 0.1f;
    c.width_end   = 0.1f;
    c.color_start[0] = c.color_start[1] = c.color_start[2] = c.color_start[3] = 1.0f;
    c.color_end[0]   = c.color_end[1]   = c.color_end[2]   = c.color_end[3]   = 1.0f;
    c.use_world_space = true;
    jce_scene_set_line_renderer(scene, e, &c);
}

void adddef_reflection_probe(JceScene *scene, JceEntity e)
{
    JceReflectionProbeComponent c;
    memset(&c, 0, sizeof(c));
    c.mode = 0; /* Baked */
    c.resolution = 128;
    c.intensity = 1.0f;
    c.box_size[0] = c.box_size[1] = c.box_size[2] = 10.0f;
    c.near_clip = 0.3f;
    c.far_clip  = 1000.0f;
    c.box_projection = true;
    c.hdr = true;
    jce_scene_set_reflection_probe(scene, e, &c);
}

void adddef_decal(JceScene *scene, JceEntity e)
{
    JceDecalComponent c;
    memset(&c, 0, sizeof(c));
    c.size[0] = c.size[1] = c.size[2] = 1.0f;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
    c.opacity = 1.0f;
    c.draw_distance = 1000.0f;
    c.fade_factor = 1.0f;
    c.layer_mask = -1;
    jce_scene_set_decal(scene, e, &c);
}

void adddef_light_probe_group(JceScene *scene, JceEntity e)
{
    JceLightProbeGroupComponent c;
    memset(&c, 0, sizeof(c));
    /* Default: 8 corners of a unit cube. */
    c.probe_count = 8;
    for (int i = 0; i < 8; ++i) {
        c.positions[i][0] = (i & 1) ? 1.0f : -1.0f;
        c.positions[i][1] = (i & 2) ? 1.0f : -1.0f;
        c.positions[i][2] = (i & 4) ? 1.0f : -1.0f;
    }
    jce_scene_set_light_probe_group(scene, e, &c);
}

void adddef_audio_listener(JceScene *scene, JceEntity e)
{
    JceAudioListenerComponent c;
    memset(&c, 0, sizeof(c));
    c.volume = 1.0f;
    c.spatialize = true;
    c.doppler_factor = 1.0f;
    jce_scene_set_audio_listener(scene, e, &c);
}

void adddef_audio_reverb_zone(JceScene *scene, JceEntity e)
{
    JceAudioReverbZoneComponent c;
    memset(&c, 0, sizeof(c));
    c.preset = JCE_REVERB_ZONE_PRESET_GENERIC;
    c.min_distance = 10.0f;
    c.max_distance = 15.0f;
    c.room = -1000.0f;
    c.room_hf = -100.0f;
    c.decay_time = 1.49f;
    c.decay_hf_ratio = 0.83f;
    c.reflections = -2602.0f;
    c.reflections_delay = 0.007f;
    c.reverb = 200.0f;
    c.reverb_delay = 0.011f;
    c.hf_reference = 5000.0f;
    c.diffusion = 100.0f;
    c.density = 100.0f;
    jce_scene_set_audio_reverb_zone(scene, e, &c);
}

void adddef_audio_occlusion(JceScene *scene, JceEntity e)
{
    JceAudioOcclusionComponent c;
    memset(&c, 0, sizeof(c));
    c.radius = 5.0f;
    c.attenuation_db = -12.0f;
    c.lowpass_cutoff_hz = 1000.0f;
    c.layer_mask = -1;
    c.affects_reverb = true;
    jce_scene_set_audio_occlusion(scene, e, &c);
}

void adddef_spawn_manager(JceScene *scene, JceEntity e)
{
    JceSpawnManagerComponent c;
    memset(&c, 0, sizeof(c));
    c.enabled = 1;
    c.max_peds = 32;
    c.max_vehicles = 16;
    c.min_spawn_radius = 30.0f;
    c.max_spawn_radius = 120.0f;
    c.despawn_pad = 30.0f;
    c.spawn_interval = 0.5f;
    jce_scene_set_spawn_manager(scene, e, &c);
}

void adddef_weapon(JceScene *scene, JceEntity e)
{
    JceWeaponComponent c;
    memset(&c, 0, sizeof(c));
    snprintf(c.name, sizeof(c.name), "%s", "Weapon");
    c.kind = JCE_WEAPON_COMP_HITSCAN;
    c.damage = 10.0f;
    c.range = 100.0f;
    c.rpm = 600.0f;
    c.clip_size = 30;
    c.reserve_max = 120;
    c.reload_seconds = 2.0f;
    c.spread_deg = 0.5f;
    c.recoil_per_shot = 0.5f;
    c.recoil_recovery = 8.0f;
    c.pellets = 1;
    c.projectile_speed = 200.0f;
    c.full_auto = false;
    jce_scene_set_weapon(scene, e, &c);
}

void adddef_save_point(JceScene *scene, JceEntity e)
{
    JceSavePointComponent c;
    memset(&c, 0, sizeof(c));
    snprintf(c.save_id,      sizeof(c.save_id),      "%s", "save_point");
    snprintf(c.display_name, sizeof(c.display_name), "%s", "Save Point");
    c.kind = JCE_SAVE_POINT_MANUAL;
    c.radius = 1.5f;
    c.slot = -1;
    c.one_shot = false;
    c.require_interact = true;
    jce_scene_set_save_point(scene, e, &c);
}

void adddef_wheel_collider(JceScene *scene, JceEntity e)
{
    JceWheelColliderComponent c;
    memset(&c, 0, sizeof(c));
    c.radius = 0.5f;
    c.suspension_distance = 0.3f;
    c.suspension_spring = 35000.0f;
    c.suspension_damper = 4500.0f;
    c.suspension_target_pos = 0.5f;
    c.mass = 20.0f;
    c.forward_friction = 1.0f;
    c.sideways_friction = 1.0f;
    jce_scene_set_wheel_collider(scene, e, &c);
}

void adddef_constant_force(JceScene *scene, JceEntity e)
{
    JceConstantForceComponent c;
    memset(&c, 0, sizeof(c));
    c.enabled = true;
    jce_scene_set_constant_force(scene, e, &c);
}

void adddef_configurable_joint(JceScene *scene, JceEntity e)
{
    JceConfigurableJointComponent c;
    memset(&c, 0, sizeof(c));
    c.x_motion = c.y_motion = c.z_motion = JCE_CFG_JOINT_LOCKED;
    c.x_rotation = c.y_rotation = c.z_rotation = JCE_CFG_JOINT_FREE;
    c.linear_limit = 0.0f;
    c.angular_x_limit_deg = 45.0f;
    c.angular_y_limit_deg = 45.0f;
    c.angular_z_limit_deg = 45.0f;
    c.break_force = 3.4e38f;
    c.break_torque = 3.4e38f;
    c.enable_collision = false;
    jce_scene_set_configurable_joint(scene, e, &c);
}

void adddef_cloth(JceScene *scene, JceEntity e)
{
    JceClothComponent c;
    memset(&c, 0, sizeof(c));
    /* 1x1 m horizontal patch as a sensible default. */
    c.corner_00 = jce_v3(0.0f, 0.0f, 0.0f);
    c.corner_10 = jce_v3(1.0f, 0.0f, 0.0f);
    c.corner_01 = jce_v3(0.0f, 0.0f, 1.0f);
    c.corner_11 = jce_v3(1.0f, 0.0f, 1.0f);
    c.res_u = 8;
    c.res_v = 8;
    c.mass_total = 1.0f;
    c.stiffness_linear  = 0.5f;
    c.stiffness_angular = 0.5f;
    c.damping    = 0.02f;
    c.iterations = 4;
    c.self_collision = false;
    c.wind_enabled   = false;
    c.wind_velocity  = jce_v3(0.0f, 0.0f, 0.0f);
    c.pinned_count   = 0;
    c.handle = 0;
    c.dirty  = true;
    jce_scene_set_cloth(scene, e, &c);
}

void adddef_network_object(JceScene *scene, JceEntity e)
{
    JceNetworkObjectComponent c; memset(&c, 0, sizeof c);
    c.owner = 0; /* server-owned by default */
    jce_scene_set_network_object(scene, e, &c);
}

void adddef_net_transform(JceScene *scene, JceEntity e)
{
    JceNetTransformComponent c; memset(&c, 0, sizeof c);
    c.sync_rate_hz   = 20;
    c.interp_ms      = 100;
    c.tolerance      = 0.5f;
    c.authority_mode = 0;
    jce_scene_set_net_transform(scene, e, &c);
    ensure_network_object(scene, e);
}

void adddef_net_animator(JceScene *scene, JceEntity e)
{
    JceNetAnimatorComponent c; memset(&c, 0, sizeof c);
    c.sync_rate_hz   = 20;
    c.interp_ms      = 100;
    c.authority_mode = 0;
    jce_scene_set_net_animator(scene, e, &c);
    ensure_network_object(scene, e);
}

void adddef_net_rigidbody(JceScene *scene, JceEntity e)
{
    JceNetRigidbodyComponent c; memset(&c, 0, sizeof c);
    c.sync_rate_hz   = 20;
    c.interp_ms      = 100;
    c.tolerance      = 0.5f;
    c.authority_mode = 0;
    jce_scene_set_net_rigidbody(scene, e, &c);
    ensure_network_object(scene, e);
}

void adddef_tilemap(JceScene *scene, JceEntity e)
{
    JceTilemapComponent c; memset(&c, 0, sizeof c);
    c.cell_size_px = 16;
    c.sort_order   = 0;
    c.orientation  = 0;
    c.visible      = true;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
    jce_scene_set_tilemap(scene, e, &c);
}

void adddef_tilemap_collider2d(JceScene *scene, JceEntity e)
{
    JceTilemapCollider2DComponent c; memset(&c, 0, sizeof c);
    c.friction_x100   = 40;
    c.bounciness_x100 = 0;
    jce_scene_set_tilemap_collider2d(scene, e, &c);
}

void adddef_avatar(JceScene *scene, JceEntity e)
{
    JceAvatarComponent c; memset(&c, 0, sizeof c);
    c.human_rig = true;
    jce_scene_set_avatar(scene, e, &c);
}

void adddef_joint2d(JceScene *scene, JceEntity e)
{
    JceJoint2DComponent c;
    memset(&c, 0, sizeof(c));
    c.kind = JCE_JOINT_2D_DISTANCE;
    c.distance = 1.0f;
    c.frequency = 5.0f;
    c.damping_ratio = 0.7f;
    c.motor_speed_deg_s = 90.0f;
    c.motor_max_torque = 10000.0f;
    c.lower_angle_deg = -90.0f;
    c.upper_angle_deg =  90.0f;
    c.break_force = 3.4e38f;
    c.break_torque = 3.4e38f;
    c.auto_configure_distance = true;
    jce_scene_set_joint2d(scene, e, &c);
}

void adddef_billboard_renderer(JceScene *scene, JceEntity e)
{
    JceBillboardRendererComponent c;
    memset(&c, 0, sizeof(c));
    c.mode = JCE_BILLBOARD_FULL;
    c.size[0] = 1.0f; c.size[1] = 1.0f;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
    c.visible = true;
    jce_scene_set_billboard_renderer(scene, e, &c);
}

void adddef_canvas(JceScene *scene, JceEntity e)
{
    JceCanvasComponent c;
    memset(&c, 0, sizeof(c));
    c.render_mode = JCE_CANVAS_OVERLAY;
    c.sort_order = 0;
    c.reference_resolution[0] = 1920.0f;
    c.reference_resolution[1] = 1080.0f;
    c.scale_factor = 1.0f;
    c.pixel_perfect = false;
    jce_scene_set_canvas(scene, e, &c);
}

void adddef_canvas_group(JceScene *scene, JceEntity e)
{
    JceCanvasGroupComponent c;
    memset(&c, 0, sizeof(c));
    c.alpha = 1.0f;
    c.interactable = true;
    c.blocks_raycasts = true;
    c.ignore_parent_groups = false;
    jce_scene_set_canvas_group(scene, e, &c);
}

void adddef_layout_group(JceScene *scene, JceEntity e)
{
    JceLayoutGroupComponent c;
    memset(&c, 0, sizeof(c));
    c.layout_kind = JCE_LAYOUT_VERTICAL;
    c.spacing[0] = c.spacing[1] = 4.0f;
    c.cell_size[0] = c.cell_size[1] = 64.0f;
    c.child_alignment = 0;
    c.control_child_size_w = true;
    c.control_child_size_h = false;
    jce_scene_set_layout_group(scene, e, &c);
}

void adddef_ui_image(JceScene *scene, JceEntity e)
{
    JceUIImageComponent c;
    memset(&c, 0, sizeof(c));
    c.image_type = JCE_UI_IMAGE_SIMPLE;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
    c.fill_amount = 1.0f;
    c.preserve_aspect = false;
    c.raycast_target = true;
    jce_scene_set_ui_image(scene, e, &c);
}

void adddef_ui_text(JceScene *scene, JceEntity e)
{
    JceUITextComponent c;
    memset(&c, 0, sizeof(c));
    snprintf(c.text, sizeof(c.text), "%s", "New Text");
    c.font_size = 14.0f;
    c.alignment = JCE_UI_TEXT_ALIGN_LEFT;
    c.color[0] = c.color[1] = c.color[2] = c.color[3] = 1.0f;
    c.line_spacing = 1.0f;
    c.min_size = 10;
    c.max_size = 40;
    jce_scene_set_ui_text(scene, e, &c);
}

void adddef_ui_button(JceScene *scene, JceEntity e)
{
    JceUIButtonComponent c;
    memset(&c, 0, sizeof(c));
    c.interactable = true;
    c.normal_color[0] = c.normal_color[1] = c.normal_color[2] = c.normal_color[3] = 1.0f;
    c.highlighted_color[0] = 0.96f; c.highlighted_color[1] = 0.96f; c.highlighted_color[2] = 0.96f; c.highlighted_color[3] = 1.0f;
    c.pressed_color[0] = 0.78f; c.pressed_color[1] = 0.78f; c.pressed_color[2] = 0.78f; c.pressed_color[3] = 1.0f;
    c.disabled_color[0] = 0.78f; c.disabled_color[1] = 0.78f; c.disabled_color[2] = 0.78f; c.disabled_color[3] = 0.50f;
    c.fade_duration = 0.1f;
    jce_scene_set_ui_button(scene, e, &c);
}

void adddef_ui_slider(JceScene *scene, JceEntity e)
{
    JceUISliderComponent c;
    memset(&c, 0, sizeof(c));
    c.value         = 0.5f;
    c.min_value     = 0.0f;
    c.max_value     = 1.0f;
    c.direction     = 0; /* L→R */
    c.interactable  = true;
    c.whole_numbers = false;
    c.bg_color[0] = 0.20f; c.bg_color[1] = 0.20f; c.bg_color[2] = 0.20f; c.bg_color[3] = 1.0f;
    c.fill_color[0] = 0.30f; c.fill_color[1] = 0.55f; c.fill_color[2] = 0.95f; c.fill_color[3] = 1.0f;
    c.handle_color[0] = c.handle_color[1] = c.handle_color[2] = c.handle_color[3] = 1.0f;
    c.handle_size   = 20.0f;
    jce_scene_set_ui_slider(scene, e, &c);
}

void adddef_ui_toggle(JceScene *scene, JceEntity e)
{
    JceUIToggleComponent c;
    memset(&c, 0, sizeof(c));
    c.is_on        = false;
    c.interactable = true;
    c.bg_color[0] = 0.20f; c.bg_color[1] = 0.20f; c.bg_color[2] = 0.20f; c.bg_color[3] = 1.0f;
    c.checkmark_color[0] = 0.30f; c.checkmark_color[1] = 0.85f; c.checkmark_color[2] = 0.40f; c.checkmark_color[3] = 1.0f;
    jce_scene_set_ui_toggle(scene, e, &c);
}

void adddef_ui_input_field(JceScene *scene, JceEntity e)
{
    JceUIInputFieldComponent c;
    memset(&c, 0, sizeof(c));
    c.text[0]      = '\0';
    snprintf(c.placeholder, sizeof(c.placeholder), "%s", "Enter text...");
    c.content_type = 0;     /* any */
    c.char_limit   = 0;     /* buffer cap */
    c.is_password  = false;
    c.read_only    = false;
    c.interactable = true;
    c.bg_color[0] = 0.10f; c.bg_color[1] = 0.10f; c.bg_color[2] = 0.10f; c.bg_color[3] = 1.0f;
    c.text_color[0] = 1.0f; c.text_color[1] = 1.0f; c.text_color[2] = 1.0f; c.text_color[3] = 1.0f;
    c.placeholder_color[0] = 0.55f; c.placeholder_color[1] = 0.55f; c.placeholder_color[2] = 0.55f; c.placeholder_color[3] = 1.0f;
    c.caret_color[0] = 1.0f; c.caret_color[1] = 1.0f; c.caret_color[2] = 1.0f; c.caret_color[3] = 1.0f;
    c.font_size   = 16.0f;
    jce_scene_set_ui_input_field(scene, e, &c);
}

void adddef_ui_scroll_view(JceScene *scene, JceEntity e)
{
    JceUIScrollViewComponent c;
    memset(&c, 0, sizeof(c));
    c.content_size[0]    = 0.0f;   /* auto = viewport (no scroll until authored) */
    c.content_size[1]    = 0.0f;
    c.scroll_position[0] = 0.0f;
    c.scroll_position[1] = 0.0f;
    c.horizontal         = false;
    c.vertical           = true;
    c.scroll_sensitivity = 30.0f;
    c.show_scrollbar     = true;
    c.scrollbar_thickness = 8.0f;
    c.bg_color[0] = 0.12f; c.bg_color[1] = 0.12f; c.bg_color[2] = 0.12f; c.bg_color[3] = 1.0f;
    c.scrollbar_color[0] = 0.55f; c.scrollbar_color[1] = 0.55f; c.scrollbar_color[2] = 0.55f; c.scrollbar_color[3] = 1.0f;
    c.scrollbar_bg_color[0] = 0.20f; c.scrollbar_bg_color[1] = 0.20f; c.scrollbar_bg_color[2] = 0.20f; c.scrollbar_bg_color[3] = 1.0f;
    c.interactable       = true;
    jce_scene_set_ui_scroll_view(scene, e, &c);
}

void adddef_ui_progress_bar(JceScene *scene, JceEntity e)
{
    JceUIProgressBarComponent c;
    memset(&c, 0, sizeof(c));
    c.value     = 0.5f;
    c.min_value = 0.0f;
    c.max_value = 1.0f;
    c.direction = 0; /* L→R */
    c.bg_color[0] = 0.20f; c.bg_color[1] = 0.20f; c.bg_color[2] = 0.20f; c.bg_color[3] = 1.0f;
    c.fill_color[0] = 0.30f; c.fill_color[1] = 0.75f; c.fill_color[2] = 0.40f; c.fill_color[3] = 1.0f;
    jce_scene_set_ui_progress_bar(scene, e, &c);
}

void adddef_ui_dropdown(JceScene *scene, JceEntity e)
{
    JceUIDropdownComponent c;
    memset(&c, 0, sizeof(c));
    c.option_count   = 0;
    c.selected_index = 0;
    c.expanded       = false;
    c.interactable   = true;
    c.bg_color[0] = 0.16f; c.bg_color[1] = 0.16f; c.bg_color[2] = 0.16f; c.bg_color[3] = 1.0f;
    c.text_color[0] = 1.0f; c.text_color[1] = 1.0f; c.text_color[2] = 1.0f; c.text_color[3] = 1.0f;
    c.popup_color[0] = 0.10f; c.popup_color[1] = 0.10f; c.popup_color[2] = 0.10f; c.popup_color[3] = 1.0f;
    c.highlight_color[0] = 0.26f; c.highlight_color[1] = 0.45f; c.highlight_color[2] = 0.78f; c.highlight_color[3] = 1.0f;
    c.font_size = 16.0f;
    jce_scene_set_ui_dropdown(scene, e, &c);
}

/* Volume / OcclusionPortal were addable in the menu but had NO default-
 * init case in the old switch (the add silently did nothing).  The
 * registry route gives them real defaults. */
void adddef_volume(JceScene *scene, JceEntity e)
{
    JceVolumeComponent c;
    memset(&c, 0, sizeof(c));
    c.shape          = JCE_VOLUME_SHAPE_BOX;
    c.extents        = jce_v3(1.0f, 1.0f, 1.0f);
    c.blend_distance = 1.0f;
    c.weight         = 1.0f;
    c.is_global      = false;
    jce_scene_set_volume(scene, e, &c);
}

void adddef_occlusion_portal(JceScene *scene, JceEntity e)
{
    JceOcclusionPortalComponent c;
    memset(&c, 0, sizeof(c));
    c.size      = jce_v3(1.0f, 1.0f, 1.0f);
    c.open      = true;
    c.portal_id = 0;
    jce_scene_set_occlusion_portal(scene, e, &c);
}

/* Former synthetic-slot defaults (moved from jce_panel_inspector_add_component.cpp). */

void adddef_video_player(JceScene *scene, JceEntity e)
{
    JceVideoPlayerComponent def;
    memset(&def, 0, sizeof(def));
    def.autoplay   = true;
    /* JCE_TEXTURE_INVALID is a C compound literal (illegal in C++ → C4576);
     * use the C++ braced-init equivalent. */
    def.output_tex = JceTexture{ UINT16_MAX };
    jce_scene_set_video_player(scene, e, &def);
}

void adddef_nav_agent(JceScene *scene, JceEntity e)
{
    JceNavAgentComponent def;
    memset(&def, 0, sizeof(def));
    def.radius          = 0.5f;
    def.height          = 2.0f;
    def.max_speed       = 3.5f;
    def.max_accel       = 8.0f;
    def.arrive_radius   = 1.5f;
    def.waypoint_radius = 0.5f;
    def.auto_repath     = true;
    def.enabled         = true;
    jce_scene_set_nav_agent(scene, e, &def);
}

void adddef_sim_lod(JceScene *scene, JceEntity e)
{
    /* Open-world sensible defaults: full rate within 25 m, ~10 Hz out to 80 m,
     * 1 Hz beyond — gating script + nav + behavior tree.  ENABLED by default
     * (the component is opt-in by its mere presence). */
    JceSimLodComponent def;
    memset(&def, 0, sizeof(def));
    def.enabled       = true;
    def.near_radius   = 25.0f;
    def.mid_radius    = 80.0f;
    def.near_hz       = 0.0f;   /* every frame */
    def.mid_hz        = 10.0f;
    def.far_hz        = 1.0f;
    def.gate_mask     = (uint32_t)JCE_SIMLOD_GATE_ALL;
    def.gate_anim_far = true;
    jce_scene_set_sim_lod(scene, e, &def);
}

void adddef_ik_constraints(JceScene *scene, JceEntity e)
{
    JceIkConstraintComponent def;
    memset(&def, 0, sizeof(def));
    def.count = 0;   /* empty stack; author in the Rigging panel */
    jce_scene_set_ik_constraints(scene, e, &def);
}

void adddef_foot_ik(JceScene *scene, JceEntity e)
{
    /* Sensible biped defaults.  Bone-name guesses follow the UE-Mannequin /
     * Mixamo convention the engine's example rigs use ("Hips" pelvis,
     * thigh/calf/foot L/R); empty fields leave a leg disabled until the
     * designer names its ankle.  ENABLED with blend 1, but the renderer pass
     * is a NO-OP until a runtime installs a ground-query hook, so this never
     * disturbs the pose in tools. */
    JceFootIkComponent def;
    memset(&def, 0, sizeof(def));
    def.enabled = true;
    snprintf(def.pelvis_bone,   sizeof(def.pelvis_bone),   "%s", "Hips");
    snprintf(def.hip_bone[0],   sizeof(def.hip_bone[0]),   "%s", "LeftUpLeg");
    snprintf(def.hip_bone[1],   sizeof(def.hip_bone[1]),   "%s", "RightUpLeg");
    snprintf(def.knee_bone[0],  sizeof(def.knee_bone[0]),  "%s", "LeftLeg");
    snprintf(def.knee_bone[1],  sizeof(def.knee_bone[1]),  "%s", "RightLeg");
    snprintf(def.ankle_bone[0], sizeof(def.ankle_bone[0]), "%s", "LeftFoot");
    snprintf(def.ankle_bone[1], sizeof(def.ankle_bone[1]), "%s", "RightFoot");
    def.max_step_height  = 0.5f;
    def.foot_offset      = 0.02f;
    def.cast_up          = 0.5f;
    def.cast_down        = 0.6f;
    def.rotate_to_normal = true;
    def.blend            = 1.0f;
    jce_scene_set_foot_ik(scene, e, &def);
}

void adddef_full_body_ik(JceScene *scene, JceEntity e)
{
    /* One disabled effector slot to start (designer names the bone + a script /
     * gameplay sets the world target).  ENABLED with blend 1, but the renderer
     * pass is a no-op until at least one effector names a resolvable bone, so
     * this never disturbs the pose on its own. */
    JceFullBodyIkComponent def;
    memset(&def, 0, sizeof(def));
    def.enabled        = true;
    def.iterations     = 10;
    def.blend          = 1.0f;
    def.effector_count = 1;
    snprintf(def.effectors[0].bone, sizeof(def.effectors[0].bone), "%s", "");
    def.effectors[0].weight = 1.0f;
    jce_scene_set_full_body_ik(scene, e, &def);
}

void adddef_sequence_player(JceScene *scene, JceEntity e)
{
    JceSequencePlayerComponent def;
    memset(&def, 0, sizeof(def));
    def.speed         = 1.0f;
    def.play_on_awake = true;
    jce_scene_set_sequence_player(scene, e, &def);
}

void adddef_morph_weights(JceScene *scene, JceEntity e)
{
    /* Empty authored set; the inspector populates sliders from the model's
     * morph-target count, and authoring a weight sets its override bit. */
    JceMorphWeightsComponent def;
    memset(&def, 0, sizeof(def));
    jce_scene_set_morph_weights(scene, e, &def);
}

void adddef_network_variable(JceScene *scene, JceEntity e)
{
    /* Default: an F32 server-authoritative variable named "value" with an
     * initial of 0.  The inspector lets the designer rename / retype / set
     * the authority and initial value; the runtime bridge registers the
     * matching typed NetVar at Play.  Auto-add the NetworkObject gatekeeper
     * (rt_spawn_net skips entities without it), same as the Net* overrides. */
    JceNetworkVariableComponent def;
    memset(&def, 0, sizeof(def));
    snprintf(def.var_name, sizeof(def.var_name), "%s", "value");
    def.var_type      = JCE_NETVAR_AUTHOR_TYPE_F32;
    def.authority     = JCE_NETVAR_AUTHOR_AUTH_SERVER;
    def.initial_value = 0.0f;
    jce_scene_set_network_variable(scene, e, &def);
    ensure_network_object(scene, e);
}

void adddef_gas(JceScene *scene, JceEntity e)
{
    /* A ready-to-play starter loadout: Health[100,0..100] + Mana[50,0..50] and
     * one "Fireball" ability costing 20 Mana on a 2 s cooldown.  The inspector
     * lets the designer add/edit attribute + ability rows; the runtime inits a
     * live JceGameplayAbilitySystem from these tables at Play and exposes it to
     * scripts (jce.gas_activate / jce.gas_get / jce.gas_apply). */
    JceGameplayAbilitySystemComponent def;
    memset(&def, 0, sizeof(def));

    snprintf(def.attributes[0].name, sizeof(def.attributes[0].name), "%s", "Health");
    def.attributes[0].base = 100.0f;
    def.attributes[0].min  = 0.0f;
    def.attributes[0].max  = 100.0f;
    snprintf(def.attributes[1].name, sizeof(def.attributes[1].name), "%s", "Mana");
    def.attributes[1].base = 50.0f;
    def.attributes[1].min  = 0.0f;
    def.attributes[1].max  = 50.0f;
    def.attribute_count = 2;

    snprintf(def.abilities[0].name, sizeof(def.abilities[0].name), "%s", "Fireball");
    def.abilities[0].id               = 1;
    def.abilities[0].cost_attr_idx    = 1;   /* Mana */
    def.abilities[0].cost_magnitude   = 20.0f;
    def.abilities[0].cooldown_seconds = 2.0f;
    def.ability_count = 1;

    jce_scene_set_gas(scene, e, &def);
}

void adddef_vehicle(JceScene *scene, JceEntity e)
{
    /* Sedan-ish defaults; ENABLED so adding the component is immediately
     * drivable at Play.  Half-extents left at 0 -> the runtime derives the
     * chassis box from the entity's BoxCollider (or its own default).  With no
     * child WheelCollider entities the runtime synthesizes four corner wheels,
     * so a bare Vehicle entity drives out of the box. */
    JceVehicleComponent c;
    memset(&c, 0, sizeof(c));
    c.enabled          = true;
    c.chassis_mass     = 1500.0f;
    c.max_engine_force = 4000.0f;
    c.max_brake_force  = 100.0f;
    c.max_steering_deg = 30.0f;
    c.drive_mode       = JCE_VEHICLE_DRIVE_RWD;
    c.input_mode       = JCE_VEHICLE_INPUT_PLAYER;
    jce_scene_set_vehicle(scene, e, &c);
}

void adddef_soft_body(JceScene *scene, JceEntity e)
{
    /* Squishy 0.5m-radius pressure ball; ENABLED so adding it is immediately
     * simulated at Play (the runtime mirrors the scene's static boxes in so it
     * rests on the ground). */
    JceSoftBodyComponent c;
    memset(&c, 0, sizeof(c));
    c.enabled          = true;
    c.radius[0]        = 0.5f;
    c.radius[1]        = 0.5f;
    c.radius[2]        = 0.5f;
    c.mass             = 2.0f;
    c.pressure         = 100.0f;
    c.stiffness_linear = 0.4f;
    c.stiffness_volume = 0.4f;
    c.damping          = 0.02f;
    c.friction         = 0.5f;
    c.resolution       = 96;
    c.self_collision   = false;
    jce_scene_set_soft_body(scene, e, &c);
}

void adddef_ragdoll(JceScene *scene, JceEntity e)
{
    /* Presence-gated (no flag bit), like Vehicle/SoftBody.  ENABLED with a
     * full-animation blend (blend_weight 1) so adding it does not collapse the
     * pose the moment Play starts; the designer dials blend toward 0 for a
     * death/physics blend.  Per-bone capsule defaults match a humanoid limb. */
    JceRagdollComponent c;
    memset(&c, 0, sizeof(c));
    c.enable       = true;
    c.blend_weight = 1.0f;
    c.radius       = 0.08f;
    c.height_scale = 1.0f;
    jce_scene_set_ragdoll(scene, e, &c);
}

void adddef_fracture(JceScene *scene, JceEntity e)
{
    /* Presence-gated (no flag bit), like Vehicle/SoftBody.  Defaults mirror the
     * engine doc comment: 8 Voronoi fragments, deterministic seed 12345, a
     * plausible break impulse and a typical solid density.  enabled defaults to
     * OFF so the runtime fracture path stays inert until the designer arms it. */
    JceFractureComponent c;
    memset(&c, 0, sizeof(c));
    c.enabled        = false;
    c.fragment_count = 8;
    c.break_impulse  = 10.0f;
    c.density        = 1000.0f;
    c.seed           = 12345u;
    jce_scene_set_fracture(scene, e, &c);
}

/* ── Duplicate fixups ─────────────────────────────────────────────── */

/* ParticleEmitter has NO flecs copy hook (the scene uses mark-and-sweep),
 * so a duplicate aliases the source's live emitter (jce_scene_particles.c
 * skips the rebuild while loaded && asset_epoch matches).  Reset the
 * duplicate's runtime bookkeeping so it builds its OWN emitter on first
 * tick. */
void dupfix_particle_emitter(JceScene *scene, JceEntity dst)
{
    if (JceParticleEmitterComponent *pe =
            jce_scene_get_particle_emitter(scene, dst)) {
        pe->loaded             = false;
        pe->emitter_handle_idx = UINT32_MAX;
        pe->asset_epoch        = 0;
    }
}

/* Cloth carries a runtime JceClothHandle; a duplicate must not alias the
 * source's live cloth — clear the handle and mark dirty so the duplicate
 * builds its own on first tick. */
void dupfix_cloth(JceScene *scene, JceEntity dst)
{
    if (JceClothComponent *cl = jce_scene_get_cloth(scene, dst)) {
        cl->handle = 0;
        cl->dirty  = true;
    }
}

/* ── Pre-remove cleanup ───────────────────────────────────────────── */

/* Destroy the runtime cloth handle (if any) before dropping the
 * component so the cloth runtime doesn't leak. */
void preremove_cloth(JceScene *scene, JceEntity e)
{
    JceClothComponent *cc = jce_scene_get_cloth(scene, e);
    if (cc && cc->handle != 0) {
        jce_cloth_destroy((JceClothHandle)cc->handle);
        cc->handle = 0;
    }
}

} /* namespace */

void jce_editor_component_defaults_ensure_registered(void)
{
    static bool done = false;
    if (done)
        return;
    done = true;

    struct Row {
        const char               *engine_name;
        JceEditorCompAddDefaultFn add_default;
    };
    static const Row kRows[] = {
        { "Transform",           adddef_transform },
        { "Pivot",               adddef_pivot },
        { "MeshRenderer",        adddef_mesh_renderer },
        { "Camera",              adddef_camera },
        { "DirectionalLight",    adddef_dir_light },
        { "PointLight",          adddef_point_light },
        { "SpotLight",           adddef_spot_light },
        { "Skybox",              adddef_skybox },
        { "SpriteRenderer",      adddef_sprite_renderer },
        { "SpriteAnimator",      adddef_sprite_animator },
        { "Animator",            adddef_animator },
        { "SkeletalAnimator",    adddef_skeletal_animator },
        { "Constraint",          adddef_constraint },
        { "Rigidbody",           adddef_rigidbody },
        { "BoxCollider",         adddef_box_collider },
        { "SphereCollider",      adddef_sphere_collider },
        { "CharacterController", adddef_character_controller },
        { "AudioSource",         adddef_audio_source },
        { "MusicTrack",          adddef_music_track },
        { "Script",              adddef_script },
        { "Terrain",             adddef_terrain },
        { "VegetationScatter",   adddef_vegetation_scatter },
        { "Water",               adddef_water },
        { "Buoyancy",            adddef_buoyancy },
        { "Rigidbody2D",         adddef_rigidbody2d },
        { "ParticleEmitter",     adddef_particle_emitter },
        { "BehaviorTree",        adddef_behavior_tree },
        { "LODGroup",            adddef_lod_group },
        { "VirtualCamera",       adddef_virtual_camera },
        { "TriggerVolume",       adddef_trigger_volume },
        { "CapsuleCollider",     adddef_capsule_collider },
        { "MeshCollider",        adddef_mesh_collider },
        { "CompoundCollider",    adddef_compound_collider },
        { "Collider2D",          adddef_collider2d },
        { "TrailRenderer",       adddef_trail_renderer },
        { "LineRenderer",        adddef_line_renderer },
        { "ReflectionProbe",     adddef_reflection_probe },
        { "Decal",               adddef_decal },
        { "LightProbeGroup",     adddef_light_probe_group },
        { "AudioListener",       adddef_audio_listener },
        { "AudioReverbZone",     adddef_audio_reverb_zone },
        { "AudioOcclusion",      adddef_audio_occlusion },
        { "SpawnManager",        adddef_spawn_manager },
        { "Weapon",              adddef_weapon },
        { "SavePoint",           adddef_save_point },
        { "WheelCollider",       adddef_wheel_collider },
        { "ConstantForce",       adddef_constant_force },
        { "ConfigurableJoint",   adddef_configurable_joint },
        { "Cloth",               adddef_cloth },
        { "NetworkObject",       adddef_network_object },
        { "NetworkTransform",    adddef_net_transform },
        { "NetworkAnimator",     adddef_net_animator },
        { "NetworkRigidbody",    adddef_net_rigidbody },
        { "Tilemap",             adddef_tilemap },
        { "TilemapCollider2D",   adddef_tilemap_collider2d },
        { "Avatar",              adddef_avatar },
        { "Joint2D",             adddef_joint2d },
        { "BillboardRenderer",   adddef_billboard_renderer },
        { "Canvas",              adddef_canvas },
        { "CanvasGroup",         adddef_canvas_group },
        { "LayoutGroup",         adddef_layout_group },
        { "UIImage",             adddef_ui_image },
        { "UIText",              adddef_ui_text },
        { "UIButton",            adddef_ui_button },
        { "UISlider",            adddef_ui_slider },
        { "UIToggle",            adddef_ui_toggle },
        { "UIInputField",        adddef_ui_input_field },
        { "UIScrollView",        adddef_ui_scroll_view },
        { "UIProgressBar",       adddef_ui_progress_bar },
        { "UIDropdown",          adddef_ui_dropdown },
        { "Volume",              adddef_volume },
        { "OcclusionPortal",     adddef_occlusion_portal },
        { "VideoPlayer",         adddef_video_player },
        { "NavAgent",            adddef_nav_agent },
        { "SimLod",              adddef_sim_lod },
        { "IkConstraints",       adddef_ik_constraints },
        { "FootIk",              adddef_foot_ik },
        { "FullBodyIk",          adddef_full_body_ik },
        { "SequencePlayer",      adddef_sequence_player },
        { "MorphWeights",        adddef_morph_weights },
        { "NetworkVariable",     adddef_network_variable },
        { "GameplayAbilitySystem", adddef_gas },
        { "Vehicle",             adddef_vehicle },
        { "SoftBody",            adddef_soft_body },
        { "Ragdoll",             adddef_ragdoll },
        { "Fracture",            adddef_fracture },
    };
    for (const Row &r : kRows)
        jce_editor_component_set_add_default_fn(r.engine_name, r.add_default);

    jce_editor_component_set_dup_fixup_fn("ParticleEmitter",
                                          dupfix_particle_emitter);
    jce_editor_component_set_dup_fixup_fn("Cloth", dupfix_cloth);

    jce_editor_component_set_pre_remove_fn("Cloth", preremove_cloth);
}
