/*
 * jce_scene.h  ECS-based scene management (powered by flecs).
 *
 * Provides entity creation, component registration, and
 * scene lifecycle management.  All components use the engine's
 * own math types (jce_math.h).
 */

#ifndef JCE_SCENE_H
#define JCE_SCENE_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_gfx_types.h>
#include <jce/renderer/jce_material_property_block.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ── Component types ─────────────────────────────────────────────── */

typedef struct {
    jce_vec3 position;
    jce_quat rotation;
    jce_vec3 scale;
} JceTransform;

typedef struct {
    JceModelHandle  model;
    JceShaderHandle shader;
    bool            visible;
    /* Persistent asset paths (for serialization). */
    char            mesh_path[256];
    char            material_path[256];
    int             mesh_shape;         /* procedural: 0=cube, 1=sphere, etc. */
    /* Inline PBR material. */
    float           base_color[4];      /* RGBA linear */
    float           metallic;
    float           roughness;
    float           emissive[3];
    float           normal_scale;
    float           ao_strength;
    int             alpha_mode;         /* 0=OPAQUE, 1=MASK, 2=BLEND */
    float           alpha_cutoff;
    bool            double_sided;
    char            albedo_tex[256];
    char            mr_tex[256];
    char            normal_tex[256];
    char            ao_tex[256];
    char            emissive_tex[256];
    /* Per-instance Material Property Block — sparse uniform overrides
     * applied AFTER the base material binds.  Defaults to "no
     * overrides" (zero-initialised) so existing meshes are unchanged. */
    JceMaterialPropertyBlock prop_block;
} JceMeshRenderer;

typedef struct {
    float fov_deg;
    float near_plane;
    float far_plane;
    bool  is_primary;
    bool  ortho;
} JceCameraComponent;

typedef struct {
    jce_vec3 direction;
    jce_vec3 color;
    float    intensity;
    bool     casts_shadow;
} JceDirectionalLight;

typedef struct {
    jce_vec3 position;
    jce_vec3 color;
    float    intensity;
    float    radius;
} JcePointLight;

typedef struct {
    jce_vec3 position;
    jce_vec3 direction;
    jce_vec3 color;
    float    intensity;
    float    radius;
    float    inner_cone_cos;
    float    outer_cone_cos;
} JceSpotLight;

/* ── Skybox component ───────────────────────────────────────────── */

typedef struct {
    char  hdr_path[256];
    float rotation;
    float exposure;
    bool  use_as_ibl;
} JceSkyboxComponent;

/* ── Sprite renderer component ──────────────────────────────────── */

typedef struct {
    char  sprite_path[256];
    float color[4];         /* RGBA linear */
    bool  flip_x;
    bool  flip_y;
    int   sorting_order;
} JceSpriteRendererComponent;

/* ── Sprite animator component ──────────────────────────────────── */

typedef struct {
    char  sheet_path[256];
    char  atlas_path[256];
    int   frame_width;
    int   frame_height;
    char  current_anim[64];
    float speed;
    bool  loop;
    bool  playing;
} JceSpriteAnimatorComponent;

/* ── Animator component (simple clip playback) ──────────────────── */

typedef struct {
    char  clip_name[64];
    float speed;
    bool  loop;
    bool  playing;
} JceAnimatorComponent;

/* ── Skeletal animator component ────────────────────────────────── */

typedef struct {
    char  skeleton_path[256];
    char  clip_names[8][64];
    int   clip_count;
    int   active_clip;
    float speed;
    bool  loop;
    bool  playing;
} JceSkeletalAnimatorComponent;

/* ── Constraint component ───────────────────────────────────────── */

typedef struct {
    int      constraint_type;   /* 0=point2point, 1=hinge, 2=slider, 3=6dof */
    uint32_t target_entity;
    float    pivot_a[3];
    float    pivot_b[3];
    float    axis[3];
    float    lower_limit;
    float    upper_limit;
    bool     disable_collision;
} JceConstraintComponent;

/* ── Physics components ──────────────────────────────────────────── */

typedef struct {
    uint32_t body_handle_idx;    /* JceBodyHandle.idx (runtime) */
    uint8_t  body_type;          /* JceBodyType enum value */
    uint8_t  shape_type;         /* JceShapeType enum value */
    float    mass;
    float    friction;
    float    restitution;
    /* Authoring properties (editor + physics creation). */
    float    drag;               /* linear damping */
    float    angular_drag;       /* angular damping */
    bool     use_gravity;
    bool     is_kinematic;
} JceRigidBodyComponent;

typedef struct {
    uint32_t body_handle_idx;    /* JceBodyHandle.idx (2D) */
    uint8_t  body_type;
    uint8_t  shape_type;
    float    mass;
    float    friction;
    float    restitution;
    bool     fixed_rotation;
} JceRigidBody2DComponent;

/* ── Collider components ─────────────────────────────────────────── */

typedef struct {
    float center[3];
    float size[3];
    bool  is_trigger;
} JceBoxColliderComponent;

typedef struct {
    float center[3];
    float radius;
    bool  is_trigger;
} JceSphereColliderComponent;

/* Capsule along a chosen local axis (0=X, 1=Y, 2=Z). */
typedef struct {
    float   center[3];
    float   radius;
    float   height;        /* total length including both hemispheres */
    int     axis;          /* 0=X, 1=Y, 2=Z */
    bool    is_trigger;
} JceCapsuleColliderComponent;

typedef struct {
    char    mesh_path[256];
    bool    convex;        /* required true if attached to dynamic Rigidbody */
    bool    is_trigger;
    float   friction;
    float   restitution;
} JceMeshColliderComponent;

/* Combined 2D collider (shape selector keeps bitfield budget tight). */
enum {
    JCE_COLLIDER_2D_BOX     = 0,
    JCE_COLLIDER_2D_CIRCLE  = 1,
    JCE_COLLIDER_2D_CAPSULE = 2,
    JCE_COLLIDER_2D_EDGE    = 3,
    JCE_COLLIDER_2D_POLYGON = 4,
};
#define JCE_COLLIDER_2D_MAX_POINTS 32
typedef struct {
    int   shape;             /* one of JCE_COLLIDER_2D_* */
    float offset[2];
    float size[2];           /* Box/Capsule extents (.x = radius for circle) */
    float radius;            /* Circle/Capsule radius                        */
    int   capsule_direction; /* 0 = vertical, 1 = horizontal                 */
    int   point_count;       /* Edge/Polygon vertex count                    */
    float points[JCE_COLLIDER_2D_MAX_POINTS][2];
    bool  is_trigger;
    float friction;
    float restitution;
} JceCollider2DComponent;

/* ── Character controller component ──────────────────────────────── */

typedef struct {
    float height;
    float radius;
    float step_offset;
    float slope_limit;
} JceCharacterControllerComponent;

/* ── Audio source component ──────────────────────────────────────── */

typedef struct {
    char  clip_path[256];
    float volume;
    float pitch;
    float spatial_blend;
    bool  loop;
    bool  play_on_awake;
} JceAudioSourceComponent;

/* ── Script component ────────────────────────────────────────────── */

typedef struct {
    char  script_path[256];
} JceScriptComponent;

/* ── Particle emitter component ─────────────────────────────────── */

/* Color stop in a "color over lifetime" gradient.  Up to 8 stops
 * across the [0,1] normalised lifetime range. */
typedef struct {
    float position;       /* 0..1 along lifetime */
    float color[4];       /* RGBA */
} JceParticleColorStop;

#define JCE_PARTICLE_GRADIENT_MAX 8
#define JCE_PARTICLE_CURVE_MAX 8   /* keyframes for size-over-lifetime */

typedef struct {
    float time;           /* 0..1 along lifetime */
    float value;
} JceParticleCurveKey;

/* Emission shape controls the volume new particles spawn within.
 * 0 = point, 1 = sphere, 2 = box, 3 = cone (Unity-style). */
typedef enum {
    JCE_PARTICLE_SHAPE_POINT  = 0,
    JCE_PARTICLE_SHAPE_SPHERE = 1,
    JCE_PARTICLE_SHAPE_BOX    = 2,
    JCE_PARTICLE_SHAPE_CONE   = 3,
} JceParticleShape;

typedef struct {
    uint32_t emitter_handle_idx; /* JceEmitterHandle.idx */
    float    emit_rate;
    float    lifetime_min;
    float    lifetime_max;

    /* Emission shape module. */
    int   shape;              /* JceParticleShape */
    float shape_radius;       /* sphere/cone */
    float shape_size[3];      /* box half-extents / cone height (z) */
    float shape_angle_deg;    /* cone */

    /* Color over lifetime (gradient). */
    JceParticleColorStop color_stops[JCE_PARTICLE_GRADIENT_MAX];
    int                  color_stop_count;
    bool                 color_over_lifetime_enabled;

    /* Size over lifetime (curve). */
    JceParticleCurveKey size_curve[JCE_PARTICLE_CURVE_MAX];
    int                 size_curve_count;
    bool                size_over_lifetime_enabled;

    /* Velocity over lifetime — single linear vector (drag) for
     * simplicity; full module deferred. */
    float velocity_over_lifetime[3];
    bool  velocity_over_lifetime_enabled;

    /* Emission rate over time — path to a .curve.json (B7.3 / B8.7).
     * When set, runtime evaluates the curve at the emitter's local
     * time and multiplies emit_rate by the sampled scalar.  Empty
     * string means "use emit_rate as-is". */
    char  emission_rate_curve_path[256];
    bool  emission_rate_curve_enabled;
} JceParticleEmitterComponent;

/* Tag components (zero-size). */
typedef struct { char _unused; } JceTagActive;

/* ── Behavior tree component ───────────────────────────────────── */

typedef struct {
    uint32_t tree_handle_idx;   /* JceBtTreeHandle.idx */
    uint32_t context_handle_idx;/* JceBtContext index (0 for default) */
    bool     active;
} JceBehaviorTree;

/* ── Editor metadata (stored on entities only in editor builds) ── */

typedef struct {
    char     name[64];      /* Display name (not flecs name, to allow duplicates) */
    char     tag[64];
    uint8_t  tag_color;     /* JceTagColor from editor */
    bool     enabled;
    bool     prefab_instance;
    char     prefab_path[260];
    /* Prefab variant: when this entity was created via "Save as Variant",
     * this holds the source prefab path the variant inherits from. Empty
     * string means "not a variant". */
    char     variant_parent_path[260];
    /* Unity-style layer index 0..31. References JceProjectTagsAndLayers.layers[]. */
    int      layer;
} JceEditorMeta;

/* ── Terrain (Phase 2 scene integration) ─────────────────────────── */

typedef struct {
    char  terrain_path[256];        /* path to .terrain.json meta file */
    char  layer_albedo_path[4][256];/* per-layer albedo texture paths   */
    float tile_scale;               /* per-layer UV tile multiplier (0 -> 10) */
    float tint[3];                  /* multiplied into base color */
    bool  visible;
    bool  splat_enabled;            /* false -> render layer0 only       */
} JceTerrainComponent;

/* ── LOD Group (per-entity multi-mesh distance switch) ─────────── */

#define JCE_LOD_COMP_MAX_LEVELS 8

typedef struct {
    /* Optional override mesh per LOD level. Empty string means
     * "reuse the entity's MeshRenderer mesh" so designers can simply
     * tweak distances without authoring distinct meshes. */
    char  level_mesh_paths[JCE_LOD_COMP_MAX_LEVELS][256];
    /* Switch distance (camera->object) for each LOD; ascending. */
    float distances[JCE_LOD_COMP_MAX_LEVELS];
    int   level_count;
    float hysteresis;          /* fraction (0..1) of distance overlap */
    bool  cull_when_too_far;   /* hide instead of pinning to last LOD */
} JceLodGroupComponent;

/* ── Virtual Camera (Cinemachine-style cinematic camera) ───────── */

/* Track mode values (same numeric range as engine's JceVcamTrackMode
 * defined in jce_virtual_camera.h, but kept as a plain int here so the
 * scene component header does not depend on that runtime header). */
enum {
    JCE_VCAM_COMP_TRACK_NONE        = 0,
    JCE_VCAM_COMP_TRACK_FOLLOW      = 1,
    JCE_VCAM_COMP_TRACK_LOOK_AT     = 2,
    JCE_VCAM_COMP_TRACK_FOLLOW_LOOK = 3,
};

typedef struct {
    char     vcam_name[64];
    int32_t  priority;          /* higher wins when multiple are active */
    bool     active;
    int      track_mode;        /* JceVcamTrackMode */
    float    position[3];       /* used when no follow target */
    float    look_at[3];        /* used when no look-at target */
    float    fov_deg;
    float    follow_offset[3];  /* offset from follow target */
    float    damping;           /* 0 = snap, 1 = heavy smoothing */
    uint64_t follow_target;     /* JceEntity supplying follow position  */
    uint64_t look_at_target;    /* JceEntity supplying look-at position */
} JceVirtualCameraComponent;

/* ── Trigger Volume (event-emitting overlap region) ────────────── */

typedef enum {
    JCE_TRIGGER_VOL_AABB   = 0,
    JCE_TRIGGER_VOL_SPHERE = 1,
    JCE_TRIGGER_VOL_OBB    = 2,
} JceTriggerVolumeShape;

typedef struct {
    int   shape;             /* JceTriggerVolumeShape */
    float center[3];
    float half_extents[3];   /* AABB/OBB extents; .x = radius for sphere */
    float axis_x[3];
    float axis_y[3];
    float axis_z[3];
    bool  enabled;
    bool  fire_stay;         /* emit STAY event each tick while overlapping */
    char  tag[64];           /* user label propagated to event payload */
} JceTriggerVolumeComponent;

/* ── Renderer components (P2-C) ────────────────────────────────── */

/* Trail Renderer (Unity TrailRenderer equivalent). */
#define JCE_TRAIL_MAX_POINTS 64
typedef struct {
    char  material_path[256];
    float time;                 /* seconds points persist */
    float min_vertex_distance;  /* drop points closer than this */
    float width_start;
    float width_end;
    float color_start[4];
    float color_end[4];
    bool  emitting;
    bool  autodestruct;
    /* Runtime sample buffer (also captured for round-trip serialization
     * so designers can preview and tweak captured trails). */
    int   point_count;
    float points[JCE_TRAIL_MAX_POINTS][3];
} JceTrailRendererComponent;

/* Line Renderer (Unity LineRenderer equivalent). */
#define JCE_LINE_MAX_POINTS 64
typedef struct {
    char  material_path[256];
    int   position_count;
    float positions[JCE_LINE_MAX_POINTS][3];
    float width_start;
    float width_end;
    float color_start[4];
    float color_end[4];
    bool  use_world_space;
    bool  loop;
} JceLineRendererComponent;

/* Reflection Probe (Unity ReflectionProbe equivalent). */
typedef enum {
    JCE_REFLECTION_PROBE_BAKED    = 0,
    JCE_REFLECTION_PROBE_REALTIME = 1,
    JCE_REFLECTION_PROBE_CUSTOM   = 2,
} JceReflectionProbeMode;

typedef struct {
    int   mode;                /* JceReflectionProbeMode */
    int   resolution;          /* cubemap face px (16/32/64/128/256/512/1024) */
    float intensity;
    float blend_distance;
    float box_size[3];
    float box_offset[3];
    float near_clip;
    float far_clip;
    char  custom_hdr_path[256]; /* used when mode == CUSTOM */
    bool  box_projection;
    bool  hdr;
} JceReflectionProbeComponent;

/* Decal Projector (Unity URP/HDRP decal). */
typedef struct {
    char  material_path[256];
    float size[3];             /* projector box size (x,y depth) */
    float pivot[3];
    float color[4];
    float opacity;             /* 0..1 multiplier */
    float draw_distance;
    float fade_factor;
    int   layer_mask;
} JceDecalComponent;

/* Light Probe Group (Unity LightProbeGroup). */
#define JCE_LIGHT_PROBE_MAX 64
typedef struct {
    int   probe_count;
    float positions[JCE_LIGHT_PROBE_MAX][3];
    bool  dering;              /* enable ring artifact reduction */
} JceLightProbeGroupComponent;

/* ── Audio components (Unity equivalents) ────────────────────────── */

/* Audio Listener — exactly one per scene typically (camera-attached). */
typedef struct {
    float volume;            /* master volume (0..1) */
    bool  paused;
    bool  spatialize;        /* whether listener performs HRTF */
    float doppler_factor;    /* global doppler scale */
} JceAudioListenerComponent;

/* Audio Reverb Zone — Unity AudioReverbZone analogue.
 * Note: this is the Unity-style component-side preset *selector*.
 * The DSP preset *parameters* (decay/wet/dry/...) live in
 * jce_reverb_zones.h as JceReverbPreset (struct). */
typedef enum {
    JCE_REVERB_ZONE_PRESET_OFF        = 0,
    JCE_REVERB_ZONE_PRESET_GENERIC    = 1,
    JCE_REVERB_ZONE_PRESET_PADDED_CELL= 2,
    JCE_REVERB_ZONE_PRESET_ROOM       = 3,
    JCE_REVERB_ZONE_PRESET_BATHROOM   = 4,
    JCE_REVERB_ZONE_PRESET_LIVING_ROOM= 5,
    JCE_REVERB_ZONE_PRESET_STONE_ROOM = 6,
    JCE_REVERB_ZONE_PRESET_AUDITORIUM = 7,
    JCE_REVERB_ZONE_PRESET_CONCERT_HALL=8,
    JCE_REVERB_ZONE_PRESET_CAVE       = 9,
    JCE_REVERB_ZONE_PRESET_ARENA      = 10,
    JCE_REVERB_ZONE_PRESET_HANGAR     = 11,
    JCE_REVERB_ZONE_PRESET_HALLWAY    = 12,
    JCE_REVERB_ZONE_PRESET_STONE_CORRIDOR = 13,
    JCE_REVERB_ZONE_PRESET_ALLEY      = 14,
    JCE_REVERB_ZONE_PRESET_FOREST     = 15,
    JCE_REVERB_ZONE_PRESET_CITY       = 16,
    JCE_REVERB_ZONE_PRESET_MOUNTAINS  = 17,
    JCE_REVERB_ZONE_PRESET_QUARRY     = 18,
    JCE_REVERB_ZONE_PRESET_PLAIN      = 19,
    JCE_REVERB_ZONE_PRESET_PARKINGLOT = 20,
    JCE_REVERB_ZONE_PRESET_SEWER_PIPE = 21,
    JCE_REVERB_ZONE_PRESET_UNDERWATER = 22,
    JCE_REVERB_ZONE_PRESET_USER       = 26,
} JceReverbZonePreset;

typedef struct {
    int   preset;            /* JceReverbZonePreset */
    float min_distance;
    float max_distance;
    /* User-preset detail params (used when preset == USER). */
    float room;              /* dB at mid frequencies (-10000..0) */
    float room_hf;           /* relative HF level (-10000..0) */
    float decay_time;        /* seconds (0.1..20) */
    float decay_hf_ratio;    /* 0.1..2.0 */
    float reflections;       /* early reflections level (-10000..1000) */
    float reflections_delay; /* seconds 0..0.3 */
    float reverb;            /* late reverb level (-10000..2000) */
    float reverb_delay;      /* seconds 0..0.1 */
    float hf_reference;      /* Hz (1000..20000) */
    float diffusion;         /* % (0..100) */
    float density;           /* % (0..100) */
} JceAudioReverbZoneComponent;

/* Audio Occlusion probe — used by audio system to attenuate sources
 * whose line-of-sight to the listener is blocked. */
typedef struct {
    float radius;            /* sphere radius the probe affects */
    float attenuation_db;    /* additional dB attenuation when occluded */
    float lowpass_cutoff_hz; /* lowpass applied to occluded sources */
    int   layer_mask;        /* obstruction layer mask */
    bool  affects_reverb;    /* also dampens reverb send when occluded */
} JceAudioOcclusionComponent;

/* ── Gameplay components ─────────────────────────────────────────── */

/* Spawn Manager — scene-attached spawn zone configuration. Runtime
 * binds JceSpawnManager (jce_spawn_manager.h) to an entity carrying
 * this component to drive distance-based ped/vehicle spawning. */
typedef struct {
    int      enabled;
    int      max_peds;
    int      max_vehicles;
    float    min_spawn_radius;
    float    max_spawn_radius;
    float    despawn_pad;
    float    spawn_interval;
    int      ped_archetype_count;          /* up to 8 archetype ids */
    int      vehicle_archetype_count;      /* up to 8 archetype ids */
    uint32_t ped_archetypes[8];
    uint32_t vehicle_archetypes[8];
    uint64_t rng_seed;                     /* 0 → default */
} JceSpawnManagerComponent;

/* Weapon — scene-attached weapon archetype (Unity-style item config).
 * Runtime instantiates JceWeaponInstance bound to a JceWeaponArchetype
 * derived from this component. */
typedef enum {
    JCE_WEAPON_COMP_HITSCAN    = 0,
    JCE_WEAPON_COMP_PROJECTILE = 1,
} JceWeaponCompKind;

typedef struct {
    char  name[64];
    int   kind;                /* JceWeaponCompKind */
    float damage;
    float range;
    float rpm;
    int   clip_size;
    int   reserve_max;
    float reload_seconds;
    float spread_deg;
    float recoil_per_shot;
    float recoil_recovery;
    int   pellets;
    float projectile_speed;    /* used when kind == PROJECTILE */
    bool  full_auto;
} JceWeaponComponent;

/* Save Point — interaction marker that triggers a save snapshot when
 * the player overlaps it. Multiple save points can share a save_id to
 * implement checkpoint groups. */
typedef enum {
    JCE_SAVE_POINT_MANUAL    = 0,
    JCE_SAVE_POINT_AUTO      = 1,
    JCE_SAVE_POINT_CHECKPOINT= 2,
} JceSavePointKind;

typedef struct {
    char  save_id[64];          /* logical identifier */
    char  display_name[128];    /* shown in UI prompt */
    int   kind;                 /* JceSavePointKind */
    float radius;               /* trigger radius */
    int   slot;                 /* save slot index, -1 = current */
    bool  one_shot;             /* destroy after first use */
    bool  require_interact;     /* player must press interact key */
} JceSavePointComponent;

/* ── Wheel Collider (vehicle physics) ──────────────────────────── */
typedef struct {
    float radius;
    float suspension_distance;
    float suspension_spring;     /* N/m */
    float suspension_damper;
    float suspension_target_pos; /* 0..1 along travel */
    float mass;
    float forward_friction;
    float sideways_friction;
    float center[3];             /* local offset */
    float motor_torque;          /* current applied N·m */
    float brake_torque;
    float steer_angle_deg;
} JceWheelColliderComponent;

/* ── Constant Force (continuous additive force on rigidbody) ───── */
typedef struct {
    float force[3];          /* world-space N */
    float relative_force[3]; /* local-space N */
    float torque[3];         /* world-space N·m */
    float relative_torque[3];/* local-space N·m */
    bool  enabled;
} JceConstantForceComponent;

/* ── Configurable Joint (generic 6DOF) ─────────────────────────── */
enum {
    JCE_CFG_JOINT_LOCKED = 0,
    JCE_CFG_JOINT_LIMITED = 1,
    JCE_CFG_JOINT_FREE = 2,
};
typedef struct {
    uint64_t connected_body;     /* JceEntity, 0 = world */
    float    anchor[3];
    float    connected_anchor[3];
    int      x_motion, y_motion, z_motion;          /* JCE_CFG_JOINT_* */
    int      x_rotation, y_rotation, z_rotation;
    float    linear_limit;
    float    angular_x_limit_deg;
    float    angular_y_limit_deg;
    float    angular_z_limit_deg;
    float    break_force;
    float    break_torque;
    bool     enable_collision;
} JceConfigurableJointComponent;

/* ── 2D Joint (Distance / Hinge / Spring) ──────────────────────── */
enum {
    JCE_JOINT_2D_DISTANCE = 0,
    JCE_JOINT_2D_HINGE    = 1,
    JCE_JOINT_2D_SPRING   = 2,
};
typedef struct {
    int      kind;                /* JCE_JOINT_2D_* */
    uint64_t connected_body;      /* JceEntity, 0 = world */
    float    anchor[2];
    float    connected_anchor[2];
    float    distance;            /* Distance/Spring rest length */
    float    frequency;           /* Spring */
    float    damping_ratio;       /* Spring */
    bool     use_motor;           /* Hinge */
    float    motor_speed_deg_s;   /* Hinge */
    float    motor_max_torque;    /* Hinge */
    bool     use_limits;          /* Hinge */
    float    lower_angle_deg;
    float    upper_angle_deg;
    float    break_force;
    float    break_torque;
    bool     enable_collision;
    bool     auto_configure_distance;
} JceJoint2DComponent;

/* ── Billboard Renderer (always-faces-camera quad) ─────────────── */
enum {
    JCE_BILLBOARD_FULL    = 0, /* face camera fully */
    JCE_BILLBOARD_Y_AXIS  = 1, /* lock Y, rotate around it */
};
typedef struct {
    char  texture_path[256];
    int   mode;                  /* JCE_BILLBOARD_* */
    float size[2];
    float color[4];
    bool  visible;
} JceBillboardRendererComponent;

/* ── UI: Canvas (root render target for 2D overlay) ────────────── */
enum {
    JCE_CANVAS_OVERLAY     = 0, /* screen-space overlay */
    JCE_CANVAS_CAMERA      = 1, /* screen-space camera */
    JCE_CANVAS_WORLD       = 2, /* world-space */
};
typedef struct {
    int   render_mode;       /* JCE_CANVAS_* */
    int   sort_order;
    float reference_resolution[2];
    float scale_factor;      /* world-space only */
    bool  pixel_perfect;
} JceCanvasComponent;

/* ── UI: Canvas Group (alpha + interactivity gating) ───────────── */
typedef struct {
    float alpha;             /* 0..1 */
    bool  interactable;
    bool  blocks_raycasts;
    bool  ignore_parent_groups;
} JceCanvasGroupComponent;

/* ── UI: Layout Group (auto-arrange children) ──────────────────── */
enum {
    JCE_LAYOUT_HORIZONTAL = 0,
    JCE_LAYOUT_VERTICAL   = 1,
    JCE_LAYOUT_GRID       = 2,
};
typedef struct {
    int   layout_kind;        /* JCE_LAYOUT_* */
    float padding[4];         /* L,R,T,B */
    float spacing[2];
    float cell_size[2];       /* grid only */
    int   child_alignment;    /* 0..8 (Unity TextAnchor) */
    bool  control_child_size_w;
    bool  control_child_size_h;
    bool  reverse_arrangement;
} JceLayoutGroupComponent;

/* ── UI: Image (textured RectTransform graphic) ────────────────── */
enum {
    JCE_UI_IMAGE_SIMPLE   = 0,
    JCE_UI_IMAGE_SLICED   = 1,
    JCE_UI_IMAGE_TILED    = 2,
    JCE_UI_IMAGE_FILLED   = 3,
};
typedef struct {
    char  sprite_path[256];
    int   image_type;         /* JCE_UI_IMAGE_* */
    float color[4];
    float fill_amount;        /* 0..1 (filled only) */
    bool  preserve_aspect;
    bool  raycast_target;
} JceUIImageComponent;

/* ── UI: Text (font-rendered string) ───────────────────────────── */
enum {
    JCE_UI_TEXT_ALIGN_LEFT   = 0,
    JCE_UI_TEXT_ALIGN_CENTER = 1,
    JCE_UI_TEXT_ALIGN_RIGHT  = 2,
};
typedef struct {
    char  text[512];
    char  font_path[256];
    float font_size;
    int   alignment;          /* JCE_UI_TEXT_ALIGN_* */
    float color[4];
    float line_spacing;
    bool  rich_text;
    bool  best_fit;
    int   min_size, max_size; /* best_fit range */
} JceUITextComponent;

/* ── UI: Button (clickable Image + state colors) ───────────────── */
typedef struct {
    bool  interactable;
    float normal_color[4];
    float highlighted_color[4];
    float pressed_color[4];
    float disabled_color[4];
    float fade_duration;
    char  on_click_handler[128]; /* script handler name (placeholder) */
} JceUIButtonComponent;

/* ── UI: Toggle (checkbox / radio) ─────────────────────────────── */
typedef struct {
    bool  interactable;
    bool  is_on;
    int   group_id;                /* >0 = exclusive group; 0 = standalone */
    float fade_duration;
    char  on_value_changed[128];
} JceUIToggleComponent;

/* ── UI: Slider ────────────────────────────────────────────────── */
typedef struct {
    bool  interactable;
    float min_value;
    float max_value;
    float value;
    bool  whole_numbers;
    int   direction;              /* 0=LTR 1=RTL 2=BTT 3=TTB */
    char  on_value_changed[128];
} JceUISliderComponent;

/* ── UI: Dropdown ──────────────────────────────────────────────── */
#define JCE_UI_DROPDOWN_MAX_OPTIONS 32
typedef struct {
    bool  interactable;
    int   value;                  /* selected option index */
    int   option_count;
    char  options[JCE_UI_DROPDOWN_MAX_OPTIONS][48];
    char  on_value_changed[128];
} JceUIDropdownComponent;

/* ── UI: InputField (single-line text input) ───────────────────── */
typedef struct {
    bool  interactable;
    int   character_limit;        /* 0 = unbounded (capped by buffer) */
    int   content_type;           /* 0=Std 1=Int 2=Decimal 3=Email 4=Password */
    bool  read_only;
    bool  multi_line;
    char  text[256];
    char  placeholder[128];
    char  on_value_changed[128];
    char  on_end_edit[128];
    bool  has_focus;
    int   caret_pos;
} JceUIInputFieldComponent;

/* ── UI: RawImage (display a texture without slicing) ──────────── */
typedef struct {
    char  texture_path[128];
    float uv_rect[4];             /* u0, v0, u1, v1 */
    float color[4];
    bool  preserve_aspect;
} JceUIRawImageComponent;

/* ── UI Effects (siblings of Text/Image, modify rendering) ─────── */
typedef struct {
    float effect_color[4];
    float effect_distance[2];     /* px offset, both axes */
    bool  use_graphic_alpha;
} JceUIOutlineEffect;

typedef struct {
    float effect_color[4];
    float effect_distance[2];
    bool  use_graphic_alpha;
} JceUIShadowEffect;

/* ── 2D Lighting components (URP 2D Renderer parity) ──────────── */
typedef struct {
    float    color[4];
    float    intensity;
    float    outer_radius;
    float    inner_radius;
    uint32_t target_layer_mask;
    int      blend;                 /* 0 = additive, 1 = multiply */
    bool     volumetric;
} JcePointLight2DComponent;

typedef struct {
    float    color[4];
    float    intensity;
    float    outer_radius;
    float    inner_radius;
    float    inner_angle_deg;
    float    outer_angle_deg;
    uint32_t target_layer_mask;
    int      blend;
    bool     volumetric;
} JceSpotLight2DComponent;

typedef struct {
    float    color[4];
    float    intensity;
    uint32_t target_layer_mask;
    int      blend;
} JceGlobalLight2DComponent;

/* ── Component type flags (bitmask for enumeration) ──────────────── */

typedef uint64_t JceComponentFlag;

#define JCE_COMP_FLAG_TRANSFORM            (UINT64_C(1) <<  0)
#define JCE_COMP_FLAG_MESH_RENDERER        (UINT64_C(1) <<  1)
#define JCE_COMP_FLAG_CAMERA               (UINT64_C(1) <<  2)
#define JCE_COMP_FLAG_DIR_LIGHT            (UINT64_C(1) <<  3)
#define JCE_COMP_FLAG_POINT_LIGHT          (UINT64_C(1) <<  4)
#define JCE_COMP_FLAG_SPOT_LIGHT           (UINT64_C(1) <<  5)
#define JCE_COMP_FLAG_SKYBOX               (UINT64_C(1) <<  6)
#define JCE_COMP_FLAG_SPRITE_RENDERER      (UINT64_C(1) <<  7)
#define JCE_COMP_FLAG_SPRITE_ANIMATOR      (UINT64_C(1) <<  8)
#define JCE_COMP_FLAG_ANIMATOR             (UINT64_C(1) <<  9)
#define JCE_COMP_FLAG_SKELETAL_ANIMATOR    (UINT64_C(1) << 10)
#define JCE_COMP_FLAG_CONSTRAINT           (UINT64_C(1) << 11)
#define JCE_COMP_FLAG_RIGIDBODY            (UINT64_C(1) << 12)
#define JCE_COMP_FLAG_RIGIDBODY_2D         (UINT64_C(1) << 13)
#define JCE_COMP_FLAG_BOX_COLLIDER         (UINT64_C(1) << 14)
#define JCE_COMP_FLAG_SPHERE_COLLIDER      (UINT64_C(1) << 15)
#define JCE_COMP_FLAG_CHARACTER_CONTROLLER (UINT64_C(1) << 16)
#define JCE_COMP_FLAG_AUDIO_SOURCE         (UINT64_C(1) << 17)
#define JCE_COMP_FLAG_SCRIPT               (UINT64_C(1) << 18)
#define JCE_COMP_FLAG_PARTICLE_EMITTER     (UINT64_C(1) << 19)
#define JCE_COMP_FLAG_BEHAVIOR_TREE        (UINT64_C(1) << 20)
#define JCE_COMP_FLAG_EDITOR_META          (UINT64_C(1) << 21)
#define JCE_COMP_FLAG_TERRAIN              (UINT64_C(1) << 22)
#define JCE_COMP_FLAG_LOD_GROUP            (UINT64_C(1) << 23)
#define JCE_COMP_FLAG_VIRTUAL_CAMERA       (UINT64_C(1) << 24)
#define JCE_COMP_FLAG_TRIGGER_VOLUME       (UINT64_C(1) << 25)
#define JCE_COMP_FLAG_CAPSULE_COLLIDER     (UINT64_C(1) << 26)
#define JCE_COMP_FLAG_MESH_COLLIDER        (UINT64_C(1) << 27)
#define JCE_COMP_FLAG_COLLIDER_2D          (UINT64_C(1) << 28)
#define JCE_COMP_FLAG_TRAIL_RENDERER       (UINT64_C(1) << 29)
#define JCE_COMP_FLAG_LINE_RENDERER        (UINT64_C(1) << 30)
#define JCE_COMP_FLAG_REFLECTION_PROBE     (UINT64_C(1) << 31)
#define JCE_COMP_FLAG_DECAL                (UINT64_C(1) << 32)
#define JCE_COMP_FLAG_LIGHT_PROBE_GROUP    (UINT64_C(1) << 33)
#define JCE_COMP_FLAG_AUDIO_LISTENER       (UINT64_C(1) << 34)
#define JCE_COMP_FLAG_AUDIO_REVERB_ZONE    (UINT64_C(1) << 35)
#define JCE_COMP_FLAG_AUDIO_OCCLUSION      (UINT64_C(1) << 36)
#define JCE_COMP_FLAG_SPAWN_MANAGER        (UINT64_C(1) << 37)
#define JCE_COMP_FLAG_WEAPON               (UINT64_C(1) << 38)
#define JCE_COMP_FLAG_SAVE_POINT           (UINT64_C(1) << 39)
#define JCE_COMP_FLAG_WHEEL_COLLIDER       (UINT64_C(1) << 40)
#define JCE_COMP_FLAG_CONSTANT_FORCE       (UINT64_C(1) << 41)
#define JCE_COMP_FLAG_CONFIGURABLE_JOINT   (UINT64_C(1) << 42)
#define JCE_COMP_FLAG_JOINT_2D             (UINT64_C(1) << 43)
#define JCE_COMP_FLAG_BILLBOARD_RENDERER   (UINT64_C(1) << 44)
#define JCE_COMP_FLAG_CANVAS               (UINT64_C(1) << 45)
#define JCE_COMP_FLAG_CANVAS_GROUP         (UINT64_C(1) << 46)
#define JCE_COMP_FLAG_LAYOUT_GROUP         (UINT64_C(1) << 47)
#define JCE_COMP_FLAG_UI_IMAGE             (UINT64_C(1) << 48)
#define JCE_COMP_FLAG_UI_TEXT              (UINT64_C(1) << 49)
#define JCE_COMP_FLAG_UI_BUTTON            (UINT64_C(1) << 50)
#define JCE_COMP_FLAG_AUDIO_BUS_ROUTE      (UINT64_C(1) << 51)
#define JCE_COMP_FLAG_ANIMATION_LAYER_STATE (UINT64_C(1) << 52)
#define JCE_COMP_FLAG_BLEND_SHAPE_WEIGHTS  (UINT64_C(1) << 53)
#define JCE_COMP_FLAG_UI_TOGGLE            (UINT64_C(1) << 54)
#define JCE_COMP_FLAG_UI_SLIDER            (UINT64_C(1) << 55)
#define JCE_COMP_FLAG_UI_DROPDOWN          (UINT64_C(1) << 56)
#define JCE_COMP_FLAG_UI_INPUT_FIELD       (UINT64_C(1) << 57)
#define JCE_COMP_FLAG_UI_RAW_IMAGE         (UINT64_C(1) << 58)
#define JCE_COMP_FLAG_UI_OUTLINE_EFFECT    (UINT64_C(1) << 59)
#define JCE_COMP_FLAG_UI_SHADOW_EFFECT     (UINT64_C(1) << 60)
#define JCE_COMP_FLAG_POINT_LIGHT_2D       (UINT64_C(1) << 61)
#define JCE_COMP_FLAG_SPOT_LIGHT_2D        (UINT64_C(1) << 62)
#define JCE_COMP_FLAG_GLOBAL_LIGHT_2D      (UINT64_C(1) << 63)

/* ── Blend Shape weights (Unity-style morph target driving) ─────── *
 *
 * Names blend-shape targets and their current weights for the mesh
 * referenced by the entity's MeshRenderer / SkinnedMesh.  The mesh's
 * morph_targets array is the source of authoring data; this component
 * just stores the per-instance weights that drive them.  Up to 16
 * named targets — projects with denser face rigs can extend the cap. */

#define JCE_BLEND_SHAPE_MAX 16
#define JCE_BLEND_SHAPE_NAME_LEN 48

typedef struct {
    char  name[JCE_BLEND_SHAPE_NAME_LEN];
    float weight;        /* 0..1 typical; > 1 allowed for over-shoot */
} JceBlendShapeEntry;

typedef struct {
    JceBlendShapeEntry shapes[JCE_BLEND_SHAPE_MAX];
    int                shape_count;
} JceBlendShapeWeightsComponent;

/* ── Audio Bus Route (Unity-style routing of an AudioSource to a bus) ─ */

/* Names a target bus ("SFX" / "Music" / "Voice") so the audio runtime
 * can call jce_audio_mixer_assign_voice with the matching JceAudioBusId
 * each time the entity's AudioSource starts a voice.  If `bus_name` is
 * empty, voices route to Master (default behaviour). */
typedef struct {
    char bus_name[64];
} JceAudioBusRouteComponent;

/* ── Animation Layer State (per-entity layer stack config) ─────────── */

#define JCE_ANIM_LAYER_STATE_MAX 4

/* Per-layer authoring data persisted on the entity.  Runtime systems
 * read this and feed a JceAnimLayerStack each frame.  `clip_path` is
 * empty for unused slots; weight=0 also disables the layer. */
typedef struct {
    char  clip_path[256];      /* asset path of the clip */
    float weight;              /* 0..1 */
    int   blend_mode;          /* JceAnimBlendMode enum value */
    float speed;               /* clip playback speed (default 1.0) */
    bool  enabled;
} JceAnimLayerSlot;

typedef struct {
    JceAnimLayerSlot layers[JCE_ANIM_LAYER_STATE_MAX];
    int              active_layer_count;  /* 0..JCE_ANIM_LAYER_STATE_MAX */
    bool             apply_root_motion;   /* propagate root delta to Transform */
} JceAnimationLayerStateComponent;

/* ── Entity handle ───────────────────────────────────────────────── */

typedef uint64_t JceEntity;
#define JCE_ENTITY_INVALID 0

/* ── Scene ───────────────────────────────────────────────────────── */

typedef struct JceScene JceScene;

/* Create / destroy. */
JCE_API JceScene *jce_scene_create(void);
JCE_API void      jce_scene_destroy(JceScene *scene);

/* Entity management. */
JCE_API JceEntity jce_scene_create_entity(JceScene *s, const char *name);
JCE_API void      jce_scene_destroy_entity(JceScene *s, JceEntity e);
JCE_API const char *jce_scene_entity_name(const JceScene *s, JceEntity e);
JCE_API const char *jce_scene_entity_registered_name(const JceScene *s, JceEntity e);
JCE_API void      jce_scene_set_entity_name(JceScene *s, JceEntity e, const char *name);

/* Parent / child hierarchy. */
JCE_API void      jce_scene_set_parent(JceScene *s, JceEntity child, JceEntity parent);
JCE_API JceEntity jce_scene_get_parent(const JceScene *s, JceEntity e);
int       jce_scene_get_children(const JceScene *s, JceEntity parent,
                                 JceEntity *out, int max_out);
JCE_API int       jce_scene_get_child_count(const JceScene *s, JceEntity parent);

/* Component access — Transform. */
JCE_API void           jce_scene_set_transform(JceScene *s, JceEntity e, const JceTransform *t);
JCE_API JceTransform  *jce_scene_get_transform(JceScene *s, JceEntity e);
JCE_API bool           jce_scene_has_transform(const JceScene *s, JceEntity e);
JCE_API void           jce_scene_remove_transform(JceScene *s, JceEntity e);

/* Component access — MeshRenderer. */
JCE_API void               jce_scene_set_mesh_renderer(JceScene *s, JceEntity e, const JceMeshRenderer *mr);
JCE_API JceMeshRenderer   *jce_scene_get_mesh_renderer(JceScene *s, JceEntity e);
JCE_API bool               jce_scene_has_mesh_renderer(const JceScene *s, JceEntity e);
JCE_API void               jce_scene_remove_mesh_renderer(JceScene *s, JceEntity e);

/* Component access — Camera. */
JCE_API void                   jce_scene_set_camera(JceScene *s, JceEntity e, const JceCameraComponent *c);
JCE_API JceCameraComponent    *jce_scene_get_camera(JceScene *s, JceEntity e);
JCE_API bool                   jce_scene_has_camera(const JceScene *s, JceEntity e);
JCE_API void                   jce_scene_remove_camera(JceScene *s, JceEntity e);

/* Component access — DirectionalLight. */
JCE_API void                   jce_scene_set_dir_light(JceScene *s, JceEntity e, const JceDirectionalLight *l);
JCE_API JceDirectionalLight   *jce_scene_get_dir_light(JceScene *s, JceEntity e);
JCE_API bool                   jce_scene_has_dir_light(const JceScene *s, JceEntity e);
JCE_API void                   jce_scene_remove_dir_light(JceScene *s, JceEntity e);

/* Component access — PointLight. */
JCE_API void                   jce_scene_set_point_light(JceScene *s, JceEntity e, const JcePointLight *l);
JCE_API JcePointLight         *jce_scene_get_point_light(JceScene *s, JceEntity e);
JCE_API bool                   jce_scene_has_point_light(const JceScene *s, JceEntity e);
JCE_API void                   jce_scene_remove_point_light(JceScene *s, JceEntity e);

/* Component access — SpotLight. */
JCE_API void                   jce_scene_set_spot_light(JceScene *s, JceEntity e, const JceSpotLight *l);
JCE_API JceSpotLight          *jce_scene_get_spot_light(JceScene *s, JceEntity e);
JCE_API bool                   jce_scene_has_spot_light(const JceScene *s, JceEntity e);
JCE_API void                   jce_scene_remove_spot_light(JceScene *s, JceEntity e);

/* Component access — Skybox. */
JCE_API void                          jce_scene_set_skybox(JceScene *s, JceEntity e, const JceSkyboxComponent *c);
JCE_API JceSkyboxComponent           *jce_scene_get_skybox(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_skybox(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_skybox(JceScene *s, JceEntity e);

/* Component access — SpriteRenderer. */
JCE_API void                          jce_scene_set_sprite_renderer(JceScene *s, JceEntity e, const JceSpriteRendererComponent *c);
JCE_API JceSpriteRendererComponent   *jce_scene_get_sprite_renderer(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_sprite_renderer(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_sprite_renderer(JceScene *s, JceEntity e);

/* Component access — SpriteAnimator. */
JCE_API void                          jce_scene_set_sprite_animator(JceScene *s, JceEntity e, const JceSpriteAnimatorComponent *c);
JCE_API JceSpriteAnimatorComponent   *jce_scene_get_sprite_animator(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_sprite_animator(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_sprite_animator(JceScene *s, JceEntity e);

/* Component access — Animator. */
JCE_API void                          jce_scene_set_animator(JceScene *s, JceEntity e, const JceAnimatorComponent *c);
JCE_API JceAnimatorComponent         *jce_scene_get_animator(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_animator(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_animator(JceScene *s, JceEntity e);

/* Component access — SkeletalAnimator. */
JCE_API void                             jce_scene_set_skeletal_animator(JceScene *s, JceEntity e, const JceSkeletalAnimatorComponent *c);
JCE_API JceSkeletalAnimatorComponent    *jce_scene_get_skeletal_animator(JceScene *s, JceEntity e);
JCE_API bool                             jce_scene_has_skeletal_animator(const JceScene *s, JceEntity e);
JCE_API void                             jce_scene_remove_skeletal_animator(JceScene *s, JceEntity e);

/* Component access — Constraint. */
JCE_API void                          jce_scene_set_constraint(JceScene *s, JceEntity e, const JceConstraintComponent *c);
JCE_API JceConstraintComponent       *jce_scene_get_constraint(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_constraint(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_constraint(JceScene *s, JceEntity e);

/* Component access — RigidBody. */
JCE_API void                          jce_scene_set_rigidbody(JceScene *s, JceEntity e, const JceRigidBodyComponent *c);
JCE_API JceRigidBodyComponent        *jce_scene_get_rigidbody(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_rigidbody(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_rigidbody(JceScene *s, JceEntity e);

/* Component access — RigidBody2D. */
JCE_API void                          jce_scene_set_rigidbody2d(JceScene *s, JceEntity e, const JceRigidBody2DComponent *c);
JCE_API JceRigidBody2DComponent      *jce_scene_get_rigidbody2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_rigidbody2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_rigidbody2d(JceScene *s, JceEntity e);

/* Component access — BoxCollider. */
JCE_API void                          jce_scene_set_box_collider(JceScene *s, JceEntity e, const JceBoxColliderComponent *c);
JCE_API JceBoxColliderComponent      *jce_scene_get_box_collider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_box_collider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_box_collider(JceScene *s, JceEntity e);

/* Component access — SphereCollider. */
JCE_API void                               jce_scene_set_sphere_collider(JceScene *s, JceEntity e, const JceSphereColliderComponent *c);
JCE_API JceSphereColliderComponent        *jce_scene_get_sphere_collider(JceScene *s, JceEntity e);
JCE_API bool                               jce_scene_has_sphere_collider(const JceScene *s, JceEntity e);
JCE_API void                               jce_scene_remove_sphere_collider(JceScene *s, JceEntity e);

/* Component access — CharacterController. */
JCE_API void                                    jce_scene_set_character_controller(JceScene *s, JceEntity e, const JceCharacterControllerComponent *c);
JCE_API JceCharacterControllerComponent        *jce_scene_get_character_controller(JceScene *s, JceEntity e);
JCE_API bool                                    jce_scene_has_character_controller(const JceScene *s, JceEntity e);
JCE_API void                                    jce_scene_remove_character_controller(JceScene *s, JceEntity e);

/* Component access — AudioSource. */
JCE_API void                          jce_scene_set_audio_source(JceScene *s, JceEntity e, const JceAudioSourceComponent *c);
JCE_API JceAudioSourceComponent      *jce_scene_get_audio_source(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_audio_source(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_audio_source(JceScene *s, JceEntity e);

/* Component access — Script. */
JCE_API void                          jce_scene_set_script(JceScene *s, JceEntity e, const JceScriptComponent *c);
JCE_API JceScriptComponent           *jce_scene_get_script(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_script(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_script(JceScene *s, JceEntity e);

/* Component access — ParticleEmitter. */
JCE_API void                          jce_scene_set_particle_emitter(JceScene *s, JceEntity e, const JceParticleEmitterComponent *c);
JCE_API JceParticleEmitterComponent  *jce_scene_get_particle_emitter(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_particle_emitter(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_particle_emitter(JceScene *s, JceEntity e);

/* Component access — BehaviorTree. */
JCE_API void                          jce_scene_set_behavior_tree(JceScene *s, JceEntity e, const JceBehaviorTree *c);
JCE_API JceBehaviorTree              *jce_scene_get_behavior_tree(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_behavior_tree(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_behavior_tree(JceScene *s, JceEntity e);

/* Component access — EditorMeta (editor-only metadata). */
JCE_API void                          jce_scene_set_editor_meta(JceScene *s, JceEntity e, const JceEditorMeta *c);
JCE_API JceEditorMeta                *jce_scene_get_editor_meta(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_editor_meta(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_editor_meta(JceScene *s, JceEntity e);

/* Component access — Terrain (heightmap-driven static geometry). */
JCE_API void                          jce_scene_set_terrain(JceScene *s, JceEntity e, const JceTerrainComponent *c);
JCE_API JceTerrainComponent          *jce_scene_get_terrain(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_terrain(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_terrain(JceScene *s, JceEntity e);

/* Component access — LOD Group. */
JCE_API void                          jce_scene_set_lod_group(JceScene *s, JceEntity e, const JceLodGroupComponent *c);
JCE_API JceLodGroupComponent         *jce_scene_get_lod_group(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_lod_group(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_lod_group(JceScene *s, JceEntity e);

/* Component access — Virtual Camera. */
JCE_API void                          jce_scene_set_virtual_camera(JceScene *s, JceEntity e, const JceVirtualCameraComponent *c);
JCE_API JceVirtualCameraComponent    *jce_scene_get_virtual_camera(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_virtual_camera(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_virtual_camera(JceScene *s, JceEntity e);

/* Component access — Trigger Volume. */
JCE_API void                          jce_scene_set_trigger_volume(JceScene *s, JceEntity e, const JceTriggerVolumeComponent *c);
JCE_API JceTriggerVolumeComponent    *jce_scene_get_trigger_volume(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_trigger_volume(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_trigger_volume(JceScene *s, JceEntity e);

/* Component access — Capsule Collider (3D). */
JCE_API void                          jce_scene_set_capsule_collider(JceScene *s, JceEntity e, const JceCapsuleColliderComponent *c);
JCE_API JceCapsuleColliderComponent  *jce_scene_get_capsule_collider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_capsule_collider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_capsule_collider(JceScene *s, JceEntity e);

/* Component access — Mesh Collider (3D). */
JCE_API void                          jce_scene_set_mesh_collider(JceScene *s, JceEntity e, const JceMeshColliderComponent *c);
JCE_API JceMeshColliderComponent     *jce_scene_get_mesh_collider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_mesh_collider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_mesh_collider(JceScene *s, JceEntity e);

/* Component access — Collider 2D (combined Box/Circle/Capsule/Edge/Polygon). */
JCE_API void                          jce_scene_set_collider2d(JceScene *s, JceEntity e, const JceCollider2DComponent *c);
JCE_API JceCollider2DComponent       *jce_scene_get_collider2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_collider2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_collider2d(JceScene *s, JceEntity e);

/* Component access — Trail Renderer. */
JCE_API void                          jce_scene_set_trail_renderer(JceScene *s, JceEntity e, const JceTrailRendererComponent *c);
JCE_API JceTrailRendererComponent    *jce_scene_get_trail_renderer(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_trail_renderer(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_trail_renderer(JceScene *s, JceEntity e);

/* Component access — Line Renderer. */
JCE_API void                          jce_scene_set_line_renderer(JceScene *s, JceEntity e, const JceLineRendererComponent *c);
JCE_API JceLineRendererComponent     *jce_scene_get_line_renderer(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_line_renderer(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_line_renderer(JceScene *s, JceEntity e);

/* Component access — Reflection Probe. */
JCE_API void                          jce_scene_set_reflection_probe(JceScene *s, JceEntity e, const JceReflectionProbeComponent *c);
JCE_API JceReflectionProbeComponent  *jce_scene_get_reflection_probe(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_reflection_probe(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_reflection_probe(JceScene *s, JceEntity e);

/* Component access — Decal Projector. */
JCE_API void                          jce_scene_set_decal(JceScene *s, JceEntity e, const JceDecalComponent *c);
JCE_API JceDecalComponent            *jce_scene_get_decal(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_decal(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_decal(JceScene *s, JceEntity e);

/* Component access — Light Probe Group. */
JCE_API void                          jce_scene_set_light_probe_group(JceScene *s, JceEntity e, const JceLightProbeGroupComponent *c);
JCE_API JceLightProbeGroupComponent  *jce_scene_get_light_probe_group(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_light_probe_group(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_light_probe_group(JceScene *s, JceEntity e);

/* Component access — Audio Listener. */
JCE_API void                          jce_scene_set_audio_listener(JceScene *s, JceEntity e, const JceAudioListenerComponent *c);
JCE_API JceAudioListenerComponent    *jce_scene_get_audio_listener(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_audio_listener(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_audio_listener(JceScene *s, JceEntity e);

/* Component access — Audio Reverb Zone. */
JCE_API void                          jce_scene_set_audio_reverb_zone(JceScene *s, JceEntity e, const JceAudioReverbZoneComponent *c);
JCE_API JceAudioReverbZoneComponent  *jce_scene_get_audio_reverb_zone(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_audio_reverb_zone(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_audio_reverb_zone(JceScene *s, JceEntity e);

/* Component access — Audio Occlusion. */
JCE_API void                          jce_scene_set_audio_occlusion(JceScene *s, JceEntity e, const JceAudioOcclusionComponent *c);
JCE_API JceAudioOcclusionComponent   *jce_scene_get_audio_occlusion(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_audio_occlusion(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_audio_occlusion(JceScene *s, JceEntity e);

/* Component access — Spawn Manager. */
JCE_API void                          jce_scene_set_spawn_manager(JceScene *s, JceEntity e, const JceSpawnManagerComponent *c);
JCE_API JceSpawnManagerComponent     *jce_scene_get_spawn_manager(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_spawn_manager(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_spawn_manager(JceScene *s, JceEntity e);

/* Component access — Weapon. */
JCE_API void                          jce_scene_set_weapon(JceScene *s, JceEntity e, const JceWeaponComponent *c);
JCE_API JceWeaponComponent           *jce_scene_get_weapon(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_weapon(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_weapon(JceScene *s, JceEntity e);

/* Component access — Save Point. */
JCE_API void                          jce_scene_set_save_point(JceScene *s, JceEntity e, const JceSavePointComponent *c);
JCE_API JceSavePointComponent        *jce_scene_get_save_point(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_save_point(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_save_point(JceScene *s, JceEntity e);

/* Component access — Wheel Collider. */
JCE_API void                          jce_scene_set_wheel_collider(JceScene *s, JceEntity e, const JceWheelColliderComponent *c);
JCE_API JceWheelColliderComponent    *jce_scene_get_wheel_collider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_wheel_collider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_wheel_collider(JceScene *s, JceEntity e);

/* Component access — Constant Force. */
JCE_API void                          jce_scene_set_constant_force(JceScene *s, JceEntity e, const JceConstantForceComponent *c);
JCE_API JceConstantForceComponent    *jce_scene_get_constant_force(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_constant_force(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_constant_force(JceScene *s, JceEntity e);

/* Component access — Configurable Joint. */
JCE_API void                          jce_scene_set_configurable_joint(JceScene *s, JceEntity e, const JceConfigurableJointComponent *c);
JCE_API JceConfigurableJointComponent*jce_scene_get_configurable_joint(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_configurable_joint(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_configurable_joint(JceScene *s, JceEntity e);

/* Component access — Joint 2D. */
JCE_API void                          jce_scene_set_joint2d(JceScene *s, JceEntity e, const JceJoint2DComponent *c);
JCE_API JceJoint2DComponent          *jce_scene_get_joint2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_joint2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_joint2d(JceScene *s, JceEntity e);

/* Component access — Billboard Renderer. */
JCE_API void                          jce_scene_set_billboard_renderer(JceScene *s, JceEntity e, const JceBillboardRendererComponent *c);
JCE_API JceBillboardRendererComponent*jce_scene_get_billboard_renderer(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_billboard_renderer(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_billboard_renderer(JceScene *s, JceEntity e);

/* Component access — UI Canvas. */
JCE_API void                          jce_scene_set_canvas(JceScene *s, JceEntity e, const JceCanvasComponent *c);
JCE_API JceCanvasComponent           *jce_scene_get_canvas(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_canvas(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_canvas(JceScene *s, JceEntity e);

/* Component access — UI Canvas Group. */
JCE_API void                          jce_scene_set_canvas_group(JceScene *s, JceEntity e, const JceCanvasGroupComponent *c);
JCE_API JceCanvasGroupComponent      *jce_scene_get_canvas_group(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_canvas_group(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_canvas_group(JceScene *s, JceEntity e);

/* Component access — UI Layout Group. */
JCE_API void                          jce_scene_set_layout_group(JceScene *s, JceEntity e, const JceLayoutGroupComponent *c);
JCE_API JceLayoutGroupComponent      *jce_scene_get_layout_group(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_layout_group(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_layout_group(JceScene *s, JceEntity e);

/* Component access — UI Image. */
JCE_API void                          jce_scene_set_ui_image(JceScene *s, JceEntity e, const JceUIImageComponent *c);
JCE_API JceUIImageComponent          *jce_scene_get_ui_image(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_image(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_image(JceScene *s, JceEntity e);

/* Component access — UI Text. */
JCE_API void                          jce_scene_set_ui_text(JceScene *s, JceEntity e, const JceUITextComponent *c);
JCE_API JceUITextComponent           *jce_scene_get_ui_text(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_text(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_text(JceScene *s, JceEntity e);

/* Component access — UI Button. */
JCE_API void                          jce_scene_set_ui_button(JceScene *s, JceEntity e, const JceUIButtonComponent *c);
JCE_API JceUIButtonComponent         *jce_scene_get_ui_button(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_button(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_button(JceScene *s, JceEntity e);

/* Component access — UI Toggle. */
JCE_API void                          jce_scene_set_ui_toggle(JceScene *s, JceEntity e, const JceUIToggleComponent *c);
JCE_API JceUIToggleComponent         *jce_scene_get_ui_toggle(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_toggle(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_toggle(JceScene *s, JceEntity e);

/* Component access — UI Slider. */
JCE_API void                          jce_scene_set_ui_slider(JceScene *s, JceEntity e, const JceUISliderComponent *c);
JCE_API JceUISliderComponent         *jce_scene_get_ui_slider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_slider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_slider(JceScene *s, JceEntity e);

/* Component access — UI Dropdown. */
JCE_API void                          jce_scene_set_ui_dropdown(JceScene *s, JceEntity e, const JceUIDropdownComponent *c);
JCE_API JceUIDropdownComponent       *jce_scene_get_ui_dropdown(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_dropdown(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_dropdown(JceScene *s, JceEntity e);

/* Component access — UI InputField. */
JCE_API void                          jce_scene_set_ui_input_field(JceScene *s, JceEntity e, const JceUIInputFieldComponent *c);
JCE_API JceUIInputFieldComponent     *jce_scene_get_ui_input_field(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_input_field(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_input_field(JceScene *s, JceEntity e);

/* Component access — UI RawImage. */
JCE_API void                          jce_scene_set_ui_raw_image(JceScene *s, JceEntity e, const JceUIRawImageComponent *c);
JCE_API JceUIRawImageComponent       *jce_scene_get_ui_raw_image(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_raw_image(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_raw_image(JceScene *s, JceEntity e);

/* Component access — UI Outline effect. */
JCE_API void                          jce_scene_set_ui_outline_effect(JceScene *s, JceEntity e, const JceUIOutlineEffect *c);
JCE_API JceUIOutlineEffect           *jce_scene_get_ui_outline_effect(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_outline_effect(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_outline_effect(JceScene *s, JceEntity e);

/* Component access — UI Shadow effect. */
JCE_API void                          jce_scene_set_ui_shadow_effect(JceScene *s, JceEntity e, const JceUIShadowEffect *c);
JCE_API JceUIShadowEffect            *jce_scene_get_ui_shadow_effect(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_shadow_effect(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_shadow_effect(JceScene *s, JceEntity e);

/* Component access — 2D Point Light. */
JCE_API void                          jce_scene_set_point_light_2d(JceScene *s, JceEntity e, const JcePointLight2DComponent *c);
JCE_API JcePointLight2DComponent     *jce_scene_get_point_light_2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_point_light_2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_point_light_2d(JceScene *s, JceEntity e);

/* Component access — 2D Spot Light. */
JCE_API void                          jce_scene_set_spot_light_2d(JceScene *s, JceEntity e, const JceSpotLight2DComponent *c);
JCE_API JceSpotLight2DComponent      *jce_scene_get_spot_light_2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_spot_light_2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_spot_light_2d(JceScene *s, JceEntity e);

/* Component access — 2D Global Light. */
JCE_API void                          jce_scene_set_global_light_2d(JceScene *s, JceEntity e, const JceGlobalLight2DComponent *c);
JCE_API JceGlobalLight2DComponent    *jce_scene_get_global_light_2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_global_light_2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_global_light_2d(JceScene *s, JceEntity e);

/* Component access — Audio Bus Route. */
JCE_API void                          jce_scene_set_audio_bus_route(JceScene *s, JceEntity e, const JceAudioBusRouteComponent *c);
JCE_API JceAudioBusRouteComponent    *jce_scene_get_audio_bus_route(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_audio_bus_route(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_audio_bus_route(JceScene *s, JceEntity e);

/* Component access — Animation Layer State. */
JCE_API void                              jce_scene_set_animation_layer_state(JceScene *s, JceEntity e, const JceAnimationLayerStateComponent *c);
JCE_API JceAnimationLayerStateComponent  *jce_scene_get_animation_layer_state(JceScene *s, JceEntity e);
JCE_API bool                              jce_scene_has_animation_layer_state(const JceScene *s, JceEntity e);
JCE_API void                              jce_scene_remove_animation_layer_state(JceScene *s, JceEntity e);

/* Component access — Blend Shape Weights. */
JCE_API void                              jce_scene_set_blend_shape_weights(JceScene *s, JceEntity e, const JceBlendShapeWeightsComponent *c);
JCE_API JceBlendShapeWeightsComponent    *jce_scene_get_blend_shape_weights(JceScene *s, JceEntity e);
JCE_API bool                              jce_scene_has_blend_shape_weights(const JceScene *s, JceEntity e);
JCE_API void                              jce_scene_remove_blend_shape_weights(JceScene *s, JceEntity e);

/* Component enumeration — returns bitmask of JceComponentFlag. */
JCE_API uint64_t jce_scene_get_component_flags(const JceScene *s, JceEntity e);

/* Iteration helpers for the editor. */
typedef void (*JceEntityCallback)(JceScene *s, JceEntity e, void *user_data);
JCE_API void jce_scene_each_entity(JceScene *s, JceEntityCallback cb, void *user_data);

/* ── Discovery (Unity-style GameObject.Find / FindWithTag) ───────── */

/* Return the first entity whose display-name (EditorMeta.name if present,
 * else flecs registered name) matches `name` exactly.  Returns
 * JCE_ENTITY_INVALID if none. */
JCE_API JceEntity jce_scene_find_by_name(const JceScene *s, const char *name);

/* Return the first entity whose EditorMeta.tag matches `tag`.  Entities
 * without EditorMeta are skipped.  JCE_ENTITY_INVALID if none. */
JCE_API JceEntity jce_scene_find_by_tag(const JceScene *s, const char *tag);

/* Collect up to `max_out` entities whose EditorMeta.tag matches `tag`.
 * Returns the actual count written. */
JCE_API uint32_t  jce_scene_find_all_by_tag(const JceScene *s, const char *tag,
                                            JceEntity *out, uint32_t max_out);

/* Collect up to `max_out` entities that carry every component bit set
 * in `component_flags` (a bitmask of JCE_COMP_FLAG_*).  Pass a single
 * flag to find all entities with one component (Unity's
 * Object.FindObjectsOfType<T>()). */
JCE_API uint32_t  jce_scene_find_all_with_components(const JceScene *s,
                                                     uint64_t component_flags,
                                                     JceEntity *out,
                                                     uint32_t max_out);

/* Get the flecs world (for advanced queries). */
JCE_API void *jce_scene_get_world(JceScene *s);

/* Progress the scene (runs flecs systems). */
JCE_API void jce_scene_update(JceScene *s, float dt);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_H */
