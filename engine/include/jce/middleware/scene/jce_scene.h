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
    char            mesh_path[128];
    char            material_path[128];
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
    char            albedo_tex[128];
    char            mr_tex[128];
    char            normal_tex[128];
    char            ao_tex[128];
    char            emissive_tex[128];
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
    char  sprite_path[128];
    float color[4];         /* RGBA linear */
    bool  flip_x;
    bool  flip_y;
    int   sorting_order;
} JceSpriteRendererComponent;

/* ── Sprite animator component ──────────────────────────────────── */

typedef struct {
    char  sheet_path[128];
    char  atlas_path[128];
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
    char  skeleton_path[128];
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

/* ── Character controller component ──────────────────────────────── */

typedef struct {
    float height;
    float radius;
    float step_offset;
    float slope_limit;
} JceCharacterControllerComponent;

/* ── Audio source component ──────────────────────────────────────── */

typedef struct {
    char  clip_path[128];
    float volume;
    float pitch;
    float spatial_blend;
    bool  loop;
    bool  play_on_awake;
} JceAudioSourceComponent;

/* ── Script component ────────────────────────────────────────────── */

typedef struct {
    char  script_path[128];
} JceScriptComponent;

/* ── Particle emitter component ─────────────────────────────────── */

typedef struct {
    uint32_t emitter_handle_idx; /* JceEmitterHandle.idx */
    float    emit_rate;
    float    lifetime_min;
    float    lifetime_max;
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

/* ── Component type flags (bitmask for enumeration) ──────────────── */

typedef enum {
    JCE_COMP_FLAG_TRANSFORM            = (1 <<  0),
    JCE_COMP_FLAG_MESH_RENDERER        = (1 <<  1),
    JCE_COMP_FLAG_CAMERA               = (1 <<  2),
    JCE_COMP_FLAG_DIR_LIGHT            = (1 <<  3),
    JCE_COMP_FLAG_POINT_LIGHT          = (1 <<  4),
    JCE_COMP_FLAG_SPOT_LIGHT           = (1 <<  5),
    JCE_COMP_FLAG_SKYBOX               = (1 <<  6),
    JCE_COMP_FLAG_SPRITE_RENDERER      = (1 <<  7),
    JCE_COMP_FLAG_SPRITE_ANIMATOR      = (1 <<  8),
    JCE_COMP_FLAG_ANIMATOR             = (1 <<  9),
    JCE_COMP_FLAG_SKELETAL_ANIMATOR    = (1 << 10),
    JCE_COMP_FLAG_CONSTRAINT           = (1 << 11),
    JCE_COMP_FLAG_RIGIDBODY            = (1 << 12),
    JCE_COMP_FLAG_RIGIDBODY_2D         = (1 << 13),
    JCE_COMP_FLAG_BOX_COLLIDER         = (1 << 14),
    JCE_COMP_FLAG_SPHERE_COLLIDER      = (1 << 15),
    JCE_COMP_FLAG_CHARACTER_CONTROLLER = (1 << 16),
    JCE_COMP_FLAG_AUDIO_SOURCE         = (1 << 17),
    JCE_COMP_FLAG_SCRIPT               = (1 << 18),
    JCE_COMP_FLAG_PARTICLE_EMITTER     = (1 << 19),
    JCE_COMP_FLAG_BEHAVIOR_TREE        = (1 << 20),
    JCE_COMP_FLAG_EDITOR_META          = (1 << 21),
    JCE_COMP_FLAG_TERRAIN              = (1 << 22),
} JceComponentFlag;

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

/* Component enumeration — returns bitmask of JceComponentFlag. */
JCE_API uint32_t jce_scene_get_component_flags(const JceScene *s, JceEntity e);

/* Iteration helpers for the editor. */
typedef void (*JceEntityCallback)(JceScene *s, JceEntity e, void *user_data);
JCE_API void jce_scene_each_entity(JceScene *s, JceEntityCallback cb, void *user_data);

/* Get the flecs world (for advanced queries). */
JCE_API void *jce_scene_get_world(JceScene *s);

/* Progress the scene (runs flecs systems). */
JCE_API void jce_scene_update(JceScene *s, float dt);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_H */
