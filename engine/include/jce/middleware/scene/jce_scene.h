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
#include <jce/renderer/jce_texture_types.h>
#include <jce/renderer/jce_volume_profile.h>
#include <jce/middleware/video/jce_video_types.h>

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
} JceMeshRenderer;

typedef enum {
    JCE_CAMERA_CLEAR_SKYBOX     = 0, /* clear with skybox (default) */
    JCE_CAMERA_CLEAR_COLOR      = 1, /* clear with solid background colour */
    JCE_CAMERA_CLEAR_DEPTH_ONLY = 2, /* clear depth buffer only (overlay) */
    JCE_CAMERA_CLEAR_NOTHING    = 3, /* no clear (transparent overlay) */
} JceCameraClearMode;

typedef struct {
    float   fov_deg;
    float   near_plane;
    float   far_plane;
    bool    is_primary;
    bool    ortho;
    uint8_t stack_index; /* 0 = base camera; 1..3 = overlay cameras */
    uint8_t clear_mode;  /* JceCameraClearMode */
} JceCameraComponent;

typedef struct {
    jce_vec3 direction;       /* local direction light travels */
    jce_vec3 color;
    float    intensity;
    bool     casts_shadow;
    /* P3-E.5 — Light cookies (directional projector mask). Optional. */
    JceTexture cookie_texture;       /* JCE_TEXTURE_INVALID = no cookie */
    float      cookie_strength;      /* 0..1 lerp from cookie sample to white */
    char       cookie_path[256];     /* asset path (for serializer / inspector) */
} JceDirectionalLight;

typedef struct {
    jce_vec3 position;
    jce_vec3 color;
    float    intensity;
    float    radius;
    bool     casts_shadow;    /* P1 — opt-in local (atlas) shadow */
    float    shadow_bias;     /* depth bias; 0 = engine default */
} JcePointLight;

typedef struct {
    jce_vec3 position;
    jce_vec3 direction;       /* local direction the spot points */
    jce_vec3 color;
    float    intensity;
    float    radius;
    float    inner_cone_cos;
    float    outer_cone_cos;
    bool     casts_shadow;    /* P1 — opt-in local (atlas) shadow */
    float    shadow_bias;     /* depth bias; 0 = engine default */
    /* P3-E.5 — Light cookies + IES profile. Optional, opt-in per light. */
    JceTexture cookie_texture;       /* JCE_TEXTURE_INVALID = no cookie */
    JceTexture ies_lut_texture;      /* JCE_TEXTURE_INVALID = no IES profile */
    float      cookie_strength;      /* 0..1 lerp from cookie sample to white */
    char       cookie_path[256];     /* asset path for serializer */
    char       ies_path[256];        /* .ies asset path for serializer */
} JceSpotLight;

/* ── Skybox component ───────────────────────────────────────────── */

typedef struct {
    char  hdr_path[256];
    float rotation;
    float exposure;
    bool  use_as_ibl;
} JceSkyboxComponent;

/* ── Scene rendering environment ────────────────────────────────── */

#define JCE_SCENE_RENDERING_POSTFX_COUNT 7

typedef enum {
    JCE_SCENE_FOG_NONE   = 0,
    JCE_SCENE_FOG_LINEAR = 1,
    JCE_SCENE_FOG_EXP    = 2,
    JCE_SCENE_FOG_EXP2   = 3,
} JceSceneFogMode;

typedef enum {
    JCE_SCENE_SOFT_SHADOW_OFF = 0,
    JCE_SCENE_SOFT_SHADOW_PCF = 1,
    JCE_SCENE_SOFT_SHADOW_VSM = 2,
} JceSceneSoftShadowMode;

typedef struct {
    uint32_t version;

    float ambient_color[3];
    float ambient_intensity;

    bool  fog_enabled;
    int   fog_mode;            /* JceSceneFogMode */
    float fog_color[3];
    float fog_density;
    float fog_start;
    float fog_end;
    float fog_height_falloff;
    float fog_height_origin;

    float shadow_distance;
    int   cascade_count;
    float split_lambda;
    int   shadow_resolution;   /* pixels: 512 / 1024 / 2048 / 4096 */
    int   soft_shadow_mode;    /* JceSceneSoftShadowMode */

    bool  postfx_enabled[JCE_SCENE_RENDERING_POSTFX_COUNT];
    float exposure;
    float gamma;
    float bloom_threshold;
    float bloom_intensity;
    float fxaa_span_max;
    float vignette_intensity;
    float vignette_smoothness;
    float chromatic_strength;

    /* Generic data-driven custom post pass (engine stays style-agnostic).
     * postfx_enabled[6] toggles it; the named shader + params define the look
     * (e.g. a client-supplied stylize / NPR shader). */
    char  custom_post_shader[64];   /* "" = none */
    bool  custom_post_needs_depth;
    int   custom_post_param_count;  /* number of authored vec4s (<= 8) */
    float custom_post_params[32];   /* 8 * vec4 (matches JCE_POSTFX_CUSTOM_PARAMS) */

    /* ── Time-of-day (P2-weather-decals-tod) ──────────────────────────
     * When tod_enabled, the scene renderer advances an internal hour-of-
     * day clock seeded from tod_hour at tod_speed hours/second, evaluates
     * jce_time_of_day_evaluate() and drives the sky / sun / ambient via
     * jce_scene_renderer_set_time_of_day().  tod_speed == 0 freezes the
     * clock at tod_hour (static lighting from a chosen moment). */
    bool  tod_enabled;
    float tod_hour;            /* start hour-of-day [0,24)                */
    float tod_speed;           /* hours advanced per real second (0=frozen)*/
    float tod_latitude;        /* sun-arc latitude in degrees (default 35)*/
    float tod_dawn_hour;       /* sun crosses horizon ascending  (def 6)  */
    float tod_dusk_hour;       /* sun crosses horizon descending (def 18) */

    /* ── Weather (P2-weather-decals-tod) ──────────────────────────────
     * weather_type maps to JceWeatherType (0=clear,1=rain,2=snow); the
     * renderer lazily creates a JceWeatherSystem and renders a screen-
     * space overlay scaled by weather_intensity [0,1]. */
    int   weather_type;        /* JceWeatherType                          */
    float weather_intensity;   /* 0..1                                    */
} JceSceneRenderingSettings;

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
    /* Optional .anim_sm.json state machine. When non-empty it drives the
       active clip every frame (parameter-driven transitions) instead of the
       fixed active_clip. Empty = classic single-clip playback. */
    char  sm_path[256];
    /* Optional 1D blend tree over clip_names[], driven by blend_param (e.g.
       movement speed). When enabled it takes precedence over sm_path and the
       fixed clip: the two clips bracketing blend_param are cross-blended.
       blend_thresholds[i] is the parameter value at which clip_names[i] is
       fully weighted. */
    bool  use_blend_tree;
    float blend_param;
    float blend_thresholds[8];
    /* Opt-in convenience: when true, the engine auto-feeds the entity's planar
       movement speed into the SM "Speed" param AND blend_param (during Play),
       so a model "just works" as a locomotion character. Default false — the
       engine then stays generic: blend_param / SM params are whatever game code
       or the authored values set them to (the renderer only evaluates). */
    bool  auto_speed;
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
    /* Continuous Collision Detection (P3-C.3).  Defaults to DISCRETE.
     * ccd_sphere_radius == 0 ⇒ auto-derive from collision-shape AABB
     * when CCD is enabled. */
    uint8_t  ccd_mode;           /* JceCcdMode enum value (0 = DISCRETE) */
    float    ccd_threshold;      /* metres/frame; 0 ⇒ engine default */
    float    ccd_sphere_radius;  /* metres; 0 ⇒ auto */
    /* Per-body gravity: world gravity * gravity_scale (1 = normal). When
     * use_gravity is false the body gets factor 0 regardless of scale. */
    float    gravity_scale;      /* default 1.0 */
    /* Collision layer 0..31, indexes the project Layer Collision Matrix
     * (drives the broadphase group/mask filter at spawn). */
    uint32_t physics_layer;
    /* Optional .physmat.json overriding this body's friction/restitution.
     * Empty ⇒ use the inline friction/restitution fields above. */
    char     physmat_path[256];
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

/*
 * Compound collider — per-object colliders cooked from a model that holds
 * several separated objects. Each object becomes one child shape and the
 * children combine into a single compound, so the holes between objects
 * stay empty instead of being filled by one fat box/hull.
 *
 * The runtime loads `model_path`, splits it into parts, cooks each part
 * per `mode`/`split` (honoring COL_/UCX_/UBX_/USP_/UCP_/TRI_ naming when
 * detect_naming is set), and instantiates the result as a compound body.
 */
typedef struct {
    char     model_path[256]; /* source model the collider is cooked from   */
    uint8_t  mode;            /* JceColliderMode (0 = AUTO)                  */
    uint8_t  split;           /* JceColliderSplitMode (0 = by-part)          */
    bool     is_static;       /* affects AUTO; trimesh requires static       */
    bool     detect_naming;   /* honor COL_/UCX_/UBX_/USP_/UCP_/TRI_         */
    bool     is_trigger;
    float    friction;
    float    restitution;
    uint32_t vhacd_resolution;         /* 0 ⇒ library default                */
    uint32_t vhacd_max_hulls;          /* 0 ⇒ library default                */
    uint32_t vhacd_max_verts_per_hull; /* 0 ⇒ library default                */
    /* Optional .physmat.json overriding friction/restitution above. */
    char     physmat_path[256];
} JceCompoundColliderComponent;

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

/* ── Video player component (video-as-texture) ───────────────────────
 *
 * Decodes an MP4/WebM clip frame-by-frame into `output_tex` (RGBA8), which
 * the scene renderer binds as the entity's mesh albedo (and which UI image
 * sources may reference).  Persisted fields are clip_path / loop / autoplay;
 * the runtime fields (video handle, GPU texture, dimensions, frame counter,
 * playing/started flags) are populated by jce_scene_video_update() and never
 * serialized.  jce_scene_video_update() advances exactly one clip frame per
 * frame and uploads it via the same zero-copy ref path the editor file
 * viewer uses. */
typedef struct {
    /* Persisted authoring fields. */
    char     clip_path[256];
    bool     loop;
    bool     autoplay;     /* begin playback as soon as the clip opens */

    /* Live transport / runtime state (NOT serialized). */
    bool     playing;      /* current play/pause state */
    bool     started;      /* clip has been opened at least once this run */

    /* Engine-owned runtime handles (NOT serialized). The system clears
     * these on scene load (memset to 0 via the JSON parser). */
    JceVideo   video;         /* 0 == JCE_VIDEO_INVALID == not opened */
    JceTexture output_tex;    /* { UINT16_MAX } when no frame yet */
    int        tex_w;
    int        tex_h;
    uint64_t   uploaded_counter; /* last frame counter uploaded to output_tex */
    uint64_t   opened_hash;      /* hash of clip_path the decoder was opened/attempted
                                  * with; a mismatch (edit / reset / undo) makes the
                                  * driver release & re-open. 0 == nothing attempted. */
} JceVideoPlayerComponent;

/* ── Script component ────────────────────────────────────────────── */

typedef struct {
    char  script_path[256];
} JceScriptComponent;

/* ── Particle emitter component ─────────────────────────────────────
 *
 * Persisted authoring fields are `asset_path` (a `*.particles.json`
 * authored in the Particle Editor) plus the legacy quick-tune
 * emit_rate / lifetime_min / lifetime_max used when no asset is set.
 *
 * The runtime fields (emitter handle into the scene's shared
 * JceParticleSystem + load bookkeeping) are populated by
 * jce_scene_particles_update() and are never serialized; the JSON loader
 * memsets them to 0 on scene load so a fresh emitter is created on first
 * tick. */
typedef struct {
    /* Persisted authoring fields. */
    char     asset_path[256];    /* `*.particles.json` (empty = use legacy fields) */
    float    emit_rate;          /* legacy quick-tune (used when asset_path empty) */
    float    lifetime_min;
    float    lifetime_max;

    /* Engine-owned runtime state (NOT serialized; cleared on scene load). */
    uint32_t emitter_handle_idx; /* JceEmitterHandle.idx; UINT32_MAX = none */
    bool     loaded;             /* emitter created in the scene particle system */
    uint64_t asset_epoch;        /* path-change marker so edits re-load the asset */
} JceParticleEmitterComponent;

/* Tag components (zero-size). */
typedef struct { char _unused; } JceTagActive;

/* ── Tags & Layers (P4-A.4) ─────────────────────────────────────────
 *
 * Unity-style scene-level Tag (interned string id) and Layer (uint8
 * index 0..31) per entity, distinct from the physics-only collision
 * mask in <jce/api_physics.h>.  Other systems (camera culling, ray
 * filters, render queue groupings) consume the same layer index.
 *
 * Persisted to "<project>/Settings/TagsAndLayers.json" — loaded on
 * scene init via jce_scene_tags_layers_load(), saved on edit. */

#define JCE_LAYER_COUNT             32
#define JCE_TAG_NAME_MAX            32
#define JCE_TAG_REGISTRY_MAX        1024
#define JCE_LAYER_NAME_MAX          32

/* Reserved tag id: 0 = "Untagged". */

typedef struct {
    uint16_t tag_id;
} JceTagComponent;

typedef struct {
    uint8_t  layer;
} JceLayerComponent;

/* Tag registry — interned strings (global, process-wide). */
JCE_API uint16_t    jce_tag_intern(const char *name);
JCE_API const char *jce_tag_name(uint16_t tag_id);
JCE_API int         jce_tag_count(void);
JCE_API const char *jce_tag_at(int idx);
/* Returns false if name is "Untagged" or unknown (cannot remove
 * reserved slot 0). */
JCE_API bool        jce_tag_remove(const char *name);

/* Layer registry — 32 named slots.  Unity defaults pre-populated by
 * jce_layer_reset_defaults() (also called on first scene init). */
JCE_API void        jce_layer_set_name(uint8_t layer, const char *name);
JCE_API const char *jce_layer_name(uint8_t layer);
JCE_API void        jce_layer_reset_defaults(void);

/* Persistence (host filesystem).  `project_root` may be NULL/"" to use
 * the current working directory; the file lives at
 * "<root>/Settings/TagsAndLayers.json".  Both return true on success. */
JCE_API bool        jce_scene_tags_layers_load(const char *project_root);
JCE_API bool        jce_scene_tags_layers_save(const char *project_root);

/* ── Behavior tree component ───────────────────────────────────── */

typedef struct {
    char     tree_path[256];    /* authored *.xml behavior-tree asset (host/pak path) */
    uint32_t tree_handle_idx;   /* JceBtTreeHandle.idx (set by runtime on load) */
    uint32_t context_handle_idx;/* JceBtContext index (0 for default) */
    float    tick_hz;           /* desired tick rate; <=0 = every gameplay frame */
    bool     active;
    /* Perception sensing params, consumed by the runtime BT/perception binding
     * (rt_spawn_gameplay).  A value <=0 means "use the engine default at spawn"
     * so scenes authored before these fields existed keep the old behaviour. */
    float    sight_range;       /* metres; <=0 ⇒ default 25 */
    float    sight_half_angle;  /* radians; <=0 ⇒ default 60° (1.0472) */
    float    hearing_range;     /* metres; <=0 ⇒ default 15 */
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
    char  baked_cubemap_path[256]; /* set by reflection probe bake (P3-E.3) */
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
    /* SH9: 9 coeffs × 3 channels (RGB) per probe; valid when sh9_baked. */
    float sh9[JCE_LIGHT_PROBE_MAX][9][3];
    bool  sh9_baked;
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

/* ── Cloth (P3-C.4 follow-up) ──────────────────────────────────── */
/*
 * Authoring data for a regular cloth grid patch. Mirrors JceClothDesc
 * (engine/include/jce/middleware/physics/jce_cloth.h) without dragging
 * that header into the scene public surface — the runtime `handle` is
 * kept as a plain uint32_t alias of JceClothHandle.
 *
 * Setting `dirty = true` instructs the scene's cloth reconciliation pass
 * (jce_scene_update) to destroy the current handle and rebuild from the
 * authoring fields below.  The handle is opaque to JSON.
 */
enum { JCE_CLOTH_MAX_PINNED = 64 };
typedef struct {
    /* Grid patch geometry. */
    jce_vec3 corner_00;
    jce_vec3 corner_10;
    jce_vec3 corner_01;
    jce_vec3 corner_11;
    uint32_t res_u;            /* >=2 */
    uint32_t res_v;            /* >=2 */

    /* Solver tuning. */
    float    mass_total;
    float    stiffness_linear;
    float    stiffness_angular;
    float    damping;
    uint32_t iterations;

    /* Pinned vertex indices (row-major: i = v*res_u + u). */
    uint32_t pinned_indices[JCE_CLOTH_MAX_PINNED];
    uint32_t pinned_count;

    /* Misc. */
    bool     self_collision;
    bool     wind_enabled;
    jce_vec3 wind_velocity;

    /* Runtime — not serialized. */
    uint32_t handle;           /* JceClothHandle; 0 = none */
    bool     dirty;            /* true = needs (re)create */
} JceClothComponent;

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

/* ── UI: RectTransform (anchor/pivot/size of a UI element) ──────────
 *
 * Unity-shaped 2D layout primitive shared by every UI graphic
 * (UIImage / UIText).  It is embedded in those components rather than
 * being a standalone ECS component because the per-entity component
 * flag bitmask (jce_scene_get_component_flags) is already fully
 * allocated (bits 0..63 in use).  Coordinates are resolved against the
 * parent's screen rect by the UI layout pass (jce_ui_canvas.*):
 *
 *   parent_rect  = canvas (root) or the parent UI element's rect
 *   anchor_min/anchor_max ∈ [0,1] : fraction of the parent rect the
 *      element's edges are pinned to.  min==max ⇒ fixed-size element
 *      positioned by anchored_position + size_delta; min!=max ⇒ the
 *      element stretches and size_delta becomes an inset (margin).
 *   pivot ∈ [0,1] : the element's own reference point (0,0 = top-left,
 *      0.5,0.5 = centre) that anchored_position offsets from.
 *   anchored_position : pixel offset of the pivot from the anchor.
 *   size_delta : when not stretching, the element's pixel size; when
 *      stretching on an axis, the inset from each anchored edge.
 *
 * A zero-initialised RectTransform (all fields 0) is treated as a
 * full-stretch rect (anchor 0..1, no inset) so legacy scenes authored
 * before RectTransform existed still fill their parent canvas. */
typedef struct {
    float anchor_min[2];        /* {x,y} 0..1 (default 0,0) */
    float anchor_max[2];        /* {x,y} 0..1 (default 1,1) */
    float pivot[2];             /* {x,y} 0..1 (default 0.5,0.5) */
    float anchored_position[2]; /* pixel offset of pivot from anchor */
    float size_delta[2];        /* {w,h} px (fixed) or inset (stretch) */
} JceRectTransform;

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
    /* 9-slice borders in source-texture pixels (L,R,T,B); 0 ⇒ simple
       quad even when image_type == JCE_UI_IMAGE_SLICED. */
    float slice_border[4];
    JceRectTransform rect;    /* layout (see JceRectTransform) */
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
    /* Runtime localization: when non-empty, jce_loc_t(locale_key)
       overrides `text` at display time; `text` acts as fallback. */
    char  locale_key[64];
    JceRectTransform rect;    /* layout (see JceRectTransform) */
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

/* ── Network object component (P3-D.3) ────────────────────────────
 *
 * Tags an entity as networked. `net_id` and `owner` mirror
 * JceNetObjectId / JceClientId (declared in jce_replication.h); we
 * carry plain integer types here to keep this scene header free of
 * any L4-net dependency.  `is_owner` is a cached convenience flag
 * (owner == local client id), refreshed by replication whenever
 * either side of that equation changes. */
typedef struct {
    uint32_t net_id;   /* server-assigned; 0 until spawned   */
    uint16_t owner;    /* 0 = server, 1..N = remote clients  */
    uint16_t flags;    /* reserved (interp / prediction ...) */
    bool     is_owner; /* convenience: owner == local client */
} JceNetworkObjectComponent;

/* ── Network ECS component overrides (P4-C.1) ───────────────────────
 *
 * These thin ECS components hang the P3-D networking state onto
 * scene entities in a data-driven way.  sync_rate_hz / interp_ms /
 * tolerance mirror JceNetTransformConfig fields; authority_mode
 * mirrors JceNetTransformAuthorityMode (stored as uint8_t to keep
 * the struct POD / serialiser-friendly). */
typedef struct {
    uint8_t  sync_rate_hz;   /* snapshot Hz; 0 = use global default (20) */
    uint16_t interp_ms;      /* interpolation buffer ms; 0 = default (100) */
    float    tolerance;      /* divergence snap distance (m); 0 = default  */
    uint8_t  authority_mode; /* 0=server, 1=owner */
} JceNetTransformComponent;

typedef struct {
    uint8_t  sync_rate_hz;
    uint16_t interp_ms;
    uint8_t  authority_mode;
} JceNetAnimatorComponent;

typedef struct {
    uint8_t  sync_rate_hz;
    uint16_t interp_ms;
    float    tolerance;
    uint8_t  authority_mode;
} JceNetRigidbodyComponent;

/* ── P4-C.3: VFX graph + tilemap rendering/collider ──────────────── */

typedef struct {
    char     graph_path[128];   /* asset path to .vfxgraph file */
    bool     play_on_awake;
    bool     loop;
    float    rate_multiplier;   /* 1.0 = nominal */
    float    intensity;         /* 0..1 emission scale */
} JceVfxGraphComponent;

typedef struct {
    char     tilemap_path[128]; /* asset path to .tilemap data */
    char     sprites_path[128]; /* shared sprite atlas */
    uint16_t cell_size_px;      /* px per cell */
    uint16_t sort_order;
    uint8_t  orientation;       /* 0=ortho, 1=iso */
    bool     visible;
    float    color[4];          /* tint */
} JceTilemapComponent;

typedef struct {
    bool     used_by_composite;
    bool     trigger;
    float    offset[2];
    uint16_t friction_x100;     /* 0..10000 -> 0..100.0 */
    uint16_t bounciness_x100;
} JceTilemapCollider2DComponent;

/*
 * Avatar (humanoid rig) — points at a skeleton asset plus an optional bone
 * mask. The runtime evaluator lands in P5; today this only stores authoring
 * intent so scenes round-trip and the inspector can show the configuration.
 */
typedef struct {
    char     avatar_path[128];        /* .avatar asset */
    char     mask_path  [128];        /* optional .mask asset */
    char     override_controller[128];/* optional anim override controller */
    bool     apply_root_motion;
    bool     human_rig;               /* false = generic */
} JceAvatarComponent;

/* ── Volume component (P4-C — post-FX blending volumes, flag 62) ── */

typedef enum {
    JCE_VOLUME_SHAPE_BOX    = 0,
    JCE_VOLUME_SHAPE_SPHERE = 1,
} JceVolumeShape;

typedef struct {
    JceVolumeProfile profile;        /* per-field post-FX overrides      */
    JceVolumeShape   shape;          /* bounding shape for weight falloff */
    jce_vec3         extents;        /* box half-extents or sphere radius in .x */
    float            blend_distance; /* fade-in distance in world units   */
    float            weight;         /* global weight multiplier 0..1     */
    bool             is_global;      /* true = always applies, no distance test */
} JceVolumeComponent;

/* ── Occlusion portal (P4-C — flag 63) ────────────────────────────── */

typedef struct {
    jce_vec3 size;      /* portal extents (world-space box around the entity) */
    bool     open;      /* true = portal open; objects behind it are visible  */
    int32_t  portal_id; /* user-assigned ID for pairing portals               */
} JceOcclusionPortalComponent;

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
#define JCE_COMP_FLAG_NETWORK_OBJECT       (UINT64_C(1) << 51)
#define JCE_COMP_FLAG_CLOTH                (UINT64_C(1) << 52)
#define JCE_COMP_FLAG_NET_TRANSFORM        (UINT64_C(1) << 53)
#define JCE_COMP_FLAG_NET_ANIMATOR         (UINT64_C(1) << 54)
#define JCE_COMP_FLAG_NET_RIGIDBODY        (UINT64_C(1) << 55)
#define JCE_COMP_FLAG_VFX_GRAPH            (UINT64_C(1) << 56)
#define JCE_COMP_FLAG_TILEMAP              (UINT64_C(1) << 57)
#define JCE_COMP_FLAG_TILEMAP_COLLIDER_2D  (UINT64_C(1) << 58)
#define JCE_COMP_FLAG_AVATAR               (UINT64_C(1) << 59)
#define JCE_COMP_FLAG_TAG                  (UINT64_C(1) << 60)
#define JCE_COMP_FLAG_LAYER                (UINT64_C(1) << 61)
#define JCE_COMP_FLAG_VOLUME               (UINT64_C(1) << 62)
#define JCE_COMP_FLAG_OCCLUSION_PORTAL     (UINT64_C(1) << 63)

/* ── Entity handle ───────────────────────────────────────────────── */

typedef uint64_t JceEntity;
#define JCE_ENTITY_INVALID 0

/* ── Scene ───────────────────────────────────────────────────────── */

typedef struct JceScene JceScene;

/* Create / destroy. */
JCE_API JceScene *jce_scene_create(void);
JCE_API void      jce_scene_destroy(JceScene *scene);

/* Scene-level rendering environment. This data is serialized with the
 * scene and is independent of editor panel visibility. Project settings
 * may seed defaults, but authored scenes own their final values. */
JCE_API JceSceneRenderingSettings
                  jce_scene_rendering_settings_default(void);
JCE_API bool      jce_scene_has_rendering_settings(const JceScene *scene);
JCE_API void      jce_scene_set_rendering_settings(
                      JceScene *scene,
                      const JceSceneRenderingSettings *settings);
JCE_API const JceSceneRenderingSettings *
                  jce_scene_get_rendering_settings(const JceScene *scene);
JCE_API JceSceneRenderingSettings *
                  jce_scene_get_rendering_settings_mut(JceScene *scene);
JCE_API void      jce_scene_clear_rendering_settings(JceScene *scene);

/*
 * Clear all user entities from the scene without destroying the scene object.
 *
 * Removes every entity that was created via jce_scene_create_entity()
 * (identified by the presence of a JceTransform component), so callers can
 * load a different scene file into the same JceScene/renderer pair without
 * paying the cost of re-registering components and rebuilding asset caches.
 *
 * The scene handle, registered component IDs, and any external observers
 * remain valid after this call.
 *
 * Returns the number of entities actually deleted, or -1 on invalid input.
 *
 * Typical use: implementing scene transitions (e.g. caged_kingdom level
 * stitching) where a single director swaps scene content at runtime.
 */
JCE_API int       jce_scene_clear(JceScene *scene);

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

/* World transform = composition of the entity's local TRS up its parent chain
 * (world = parent_world * local). A root entity returns its local matrix, so
 * unparented/flat scenes are unchanged. Use this (not raw JceTransform) when a
 * world-space matrix is needed for rendering, picking, or gizmos. */
JCE_API jce_mat4  jce_scene_get_world_matrix(const JceScene *s, JceEntity e);

/* Invalidate the per-frame world-matrix cache that backs
 * jce_scene_get_world_matrix. jce_scene_get_world_matrix memoizes each
 * entity's composed world matrix for the current frame (so a parent shared
 * by K children is composed once, not K times); this call begins a new
 * frame, dropping every memoized entry. jce_scene_update calls it
 * automatically each tick, and jce_scene_set_parent calls it on reparent;
 * a host that mutates transforms and re-reads world matrices WITHOUT going
 * through jce_scene_update first (e.g. an editor that only renders) should
 * call this once at the top of its render frame to avoid reading a stale
 * cached matrix. Cheap and idempotent. */
JCE_API void      jce_scene_invalidate_world_cache(JceScene *s);

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

/* Component access — VideoPlayer (video-as-texture). */
JCE_API void                          jce_scene_set_video_player(JceScene *s, JceEntity e, const JceVideoPlayerComponent *c);
JCE_API JceVideoPlayerComponent      *jce_scene_get_video_player(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_video_player(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_video_player(JceScene *s, JceEntity e);

/* Per-frame video-as-texture driver.
 *
 * Iterates every entity holding a JceVideoPlayerComponent and, for those
 * that are playing (or set to autoplay), opens the clip on first use,
 * advances the decoder by `dt` seconds, and uploads the freshest decoded
 * frame into the component's `output_tex`.  Call ONCE per frame.  Both the
 * runtime (jce_runtime_step) and the editor (when not in play mode) drive
 * this; the renderer only reads output_tex, so it stays idempotent across
 * multiple render passes/viewports.
 *
 * `resolve_path`, when non-NULL, maps the asset-relative clip_path to a
 * host-openable path (mirrors the scene renderer's resolve_path callback);
 * pass NULL to open clip_path as-is. */
typedef bool (*JceVideoResolvePathFn)(const char *in, char *out, int outsz, void *ud);
JCE_API void jce_scene_video_update(JceScene *s, double dt,
                                    JceVideoResolvePathFn resolve_path,
                                    void *resolve_ud);

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

/* Particle system tick (P2-particle-vfx-runtime).
 *
 * Drives every entity carrying a JceParticleEmitterComponent: lazily
 * creates a scene-owned JceParticleSystem, loads each component's
 * `*.particles.json` into a real emitter (falling back to the legacy
 * quick-tune fields when no asset is set), keeps the emitter origin synced
 * to the entity's world position, ticks the simulation, and submits alive
 * particles as debug-draw billboards (the CPU particle backend has no
 * dedicated GPU billboard pass yet).  Runs once per runtime step after
 * jce_scene_update so it reads the freshest transforms. */
JCE_API void jce_scene_particles_update(JceScene *s, float dt);

/* Release the scene-owned particle system (called from jce_scene_destroy). */
JCE_API void jce_scene_particles_shutdown(JceScene *s);

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

/* Component access — Compound Collider (per-object cooked colliders). */
JCE_API void                          jce_scene_set_compound_collider(JceScene *s, JceEntity e, const JceCompoundColliderComponent *c);
JCE_API JceCompoundColliderComponent *jce_scene_get_compound_collider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_compound_collider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_compound_collider(JceScene *s, JceEntity e);

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

/* ── Network object (P3-D.3) ─────────────────────────────────────── */
JCE_API void                          jce_scene_set_network_object(JceScene *s, JceEntity e, const JceNetworkObjectComponent *c);
JCE_API JceNetworkObjectComponent    *jce_scene_get_network_object(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_network_object(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_network_object(JceScene *s, JceEntity e);

/* ── Cloth (P3-C.4 follow-up) ────────────────────────────────────── */
JCE_API void                          jce_scene_set_cloth(JceScene *s, JceEntity e, const JceClothComponent *c);
JCE_API JceClothComponent            *jce_scene_get_cloth(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_cloth(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_cloth(JceScene *s, JceEntity e);

/* ── Network ECS components (P4-C.1) ────────────────────────────────── */
JCE_API void                          jce_scene_set_net_transform(JceScene *s, JceEntity e, const JceNetTransformComponent *c);
JCE_API JceNetTransformComponent     *jce_scene_get_net_transform(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_net_transform(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_net_transform(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_net_animator(JceScene *s, JceEntity e, const JceNetAnimatorComponent *c);
JCE_API JceNetAnimatorComponent      *jce_scene_get_net_animator(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_net_animator(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_net_animator(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_net_rigidbody(JceScene *s, JceEntity e, const JceNetRigidbodyComponent *c);
JCE_API JceNetRigidbodyComponent     *jce_scene_get_net_rigidbody(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_net_rigidbody(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_net_rigidbody(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_vfx_graph(JceScene *s, JceEntity e, const JceVfxGraphComponent *c);
JCE_API JceVfxGraphComponent         *jce_scene_get_vfx_graph(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_vfx_graph(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_vfx_graph(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_tilemap(JceScene *s, JceEntity e, const JceTilemapComponent *c);
JCE_API JceTilemapComponent          *jce_scene_get_tilemap(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_tilemap(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_tilemap(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_tilemap_collider2d(JceScene *s, JceEntity e, const JceTilemapCollider2DComponent *c);
JCE_API JceTilemapCollider2DComponent*jce_scene_get_tilemap_collider2d(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_tilemap_collider2d(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_tilemap_collider2d(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_avatar(JceScene *s, JceEntity e, const JceAvatarComponent *c);
JCE_API JceAvatarComponent           *jce_scene_get_avatar(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_avatar(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_avatar(JceScene *s, JceEntity e);

/* ── Tag / Layer per-entity components (P4-A.4) ──────────────────── */
JCE_API void                          jce_scene_set_tag_component(JceScene *s, JceEntity e, const JceTagComponent *c);
JCE_API JceTagComponent              *jce_scene_get_tag_component(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_tag_component(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_tag_component(JceScene *s, JceEntity e);

JCE_API void                          jce_scene_set_layer_component(JceScene *s, JceEntity e, const JceLayerComponent *c);
JCE_API JceLayerComponent            *jce_scene_get_layer_component(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_layer_component(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_layer_component(JceScene *s, JceEntity e);

/* Convenience: set tag by name (interns automatically) / read tag name. */
JCE_API void                          jce_scene_set_entity_tag_name(JceScene *s, JceEntity e, const char *tag);
JCE_API const char                   *jce_scene_get_entity_tag_name(JceScene *s, JceEntity e);
JCE_API void                          jce_scene_set_entity_layer(JceScene *s, JceEntity e, uint8_t layer);
JCE_API uint8_t                       jce_scene_get_entity_layer(JceScene *s, JceEntity e);

/* Lookup helpers — return first/all entities with a given tag/layer. */
JCE_API JceEntity                     jce_scene_find_with_tag(JceScene *s, const char *tag);
JCE_API int                           jce_scene_find_all_with_tag(JceScene *s, const char *tag, JceEntity *out, int max);
JCE_API int                           jce_scene_find_all_in_layer(JceScene *s, uint8_t layer, JceEntity *out, int max);

/* ── Volume component (P4-C) ─────────────────────────────────────── */
JCE_API void                          jce_scene_set_volume(JceScene *s, JceEntity e, const JceVolumeComponent *c);
JCE_API JceVolumeComponent           *jce_scene_get_volume(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_volume(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_volume(JceScene *s, JceEntity e);

/* ── Occlusion portal (P4-C) ─────────────────────────────────────── */
JCE_API void                          jce_scene_set_occlusion_portal(JceScene *s, JceEntity e, const JceOcclusionPortalComponent *c);
JCE_API JceOcclusionPortalComponent  *jce_scene_get_occlusion_portal(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_occlusion_portal(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_occlusion_portal(JceScene *s, JceEntity e);

/* Component enumeration — returns bitmask of JceComponentFlag. */
JCE_API uint64_t jce_scene_get_component_flags(const JceScene *s, JceEntity e);

/* ── Per-component enable / disable (Unity-style) ──────────────────────
 * Disabling a component PRESERVES its data; it just stops being processed by
 * the systems (render / physics / animation / audio …) and persists in the
 * scene file. `flag` is a single JCE_COMP_FLAG_* bit. Default = enabled. */
JCE_API bool     jce_scene_component_enabled(const JceScene *s, JceEntity e, uint64_t flag);
JCE_API void     jce_scene_set_component_enabled(JceScene *s, JceEntity e, uint64_t flag, bool enabled);
/* Whole disabled-bitmask accessors (for serialization). */
JCE_API uint64_t jce_scene_get_disabled_components(const JceScene *s, JceEntity e);
JCE_API void     jce_scene_set_disabled_components(JceScene *s, JceEntity e, uint64_t disabled_mask);

/* Iteration helpers for the editor. */
typedef void (*JceEntityCallback)(JceScene *s, JceEntity e, void *user_data);
JCE_API void jce_scene_each_entity(JceScene *s, JceEntityCallback cb, void *user_data);

/* Get the flecs world (for advanced queries). */
JCE_API void *jce_scene_get_world(JceScene *s);

/* Progress the scene (runs flecs systems). */
JCE_API void jce_scene_update(JceScene *s, float dt);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_H */
