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
#include <jce/renderer/jce_skinned_mesh.h>      /* JCE_MAX_BONES (ragdoll pose relay) */
#include <jce/middleware/video/jce_video_types.h>
#include <jce/middleware/scene/jce_water.h>
#include <jce/middleware/animation/jce_morph.h>  /* JCE_MORPH_MAX_WEIGHTS */

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
    /* Mesh-local point used as the object's transform pivot. Zero preserves
     * legacy Transform-only scenes. */
    jce_vec3 local_position;
    /* Optional editor manipulator orientation, stored with the scene so Maya-
     * style custom pivot axes survive reloads. Runtime model composition uses
     * local_position; rotation is consumed by editor tooling. */
    jce_quat local_rotation;
} JcePivotComponent;

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
    /* Per-renderer shadow flags (Unity-style). Stored INVERTED so that
       zero-initialized components and legacy scenes default to the standard
       behaviour: cast ON, receive ON. */
    bool            shadow_cast_off;    /* true = this mesh casts no shadows */
    bool            shadow_receive_off; /* true = this mesh ignores shadows  */
    char            albedo_tex[256];
    char            mr_tex[256];
    char            normal_tex[256];
    char            ao_tex[256];
    char            emissive_tex[256];
    /* Runtime albedo override (NOT serialized; zero-init = off).  When
       has_albedo_runtime is set, the renderer uses albedo_runtime_idx (a bgfx
       texture handle index) as the base-colour texture instead of resolving
       albedo_tex.  Lets code assign a GPU texture directly — procedural content,
       video textures, and the texture-diverse instancing benchmark (many
       entities sharing a mesh but each with its own albedo). */
    bool            has_albedo_runtime;
    uint16_t        albedo_runtime_idx;
    uint16_t        albedo_runtime_w, albedo_runtime_h; /* dims (raw handles aren't in the size registry) */
    uint8_t         albedo_runtime_mips; /* source mip levels (0 ⇒ 1; avoids blitting non-resident mips) */
    /* Runtime Shader Graph program resolved while the material is parsed.
     * It is deliberately not serialized: the persisted source is the material
     * file's customProgramVs/customProgramFs pair.  Carrying the resolved handle
     * here preserves the VFS source (PAK or loose) through to the draw phase. */
    bool            has_custom_program;
    uint16_t        custom_program_idx;
    /* Per-character toon (stylized-slice §5.6).  Zero-default = OFF: legacy
     * scenes without these keys round-trip byte-identical and render as plain
     * PBR.  The scene renderer only honours `toon` when sr_toon_allowed
     * (quality>=HIGH + valid pbr_toon program). */
    bool            toon;
    int             toon_bands;        /* cel bands (2..4); 0 => treated as 3 */
    float           rim_power;         /* fresnel rim exponent */
    float           rim_intensity;     /* 0 = no rim */
    float           rim_color[3];      /* linear */
    float           outline_width;     /* world-units hull extrusion; 0 = no outline */
    float           outline_color[3];  /* linear */
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

/* Tonemap operator for the postfx finish.  ACES is the legacy hardcoded
 * curve (neutral default).  The postfx layer (plan 06) consumes the SAME
 * 0/1/2 values — there is NO separate divergent enum. */
typedef enum {
    JCE_TONEMAP_ACES    = 0,
    JCE_TONEMAP_NEUTRAL = 1,
    JCE_TONEMAP_AGX     = 2,
} JceSceneTonemapOp;

/* Sky-rendering mode for the scene's sky pass.
 *   GRADIENT = the legacy 3-colour procedural gradient (default; also the
 *              target the time-of-day driver overrides each frame).
 *   EQUIRECT = an HDR equirectangular skybox (the existing skybox path).
 *   PREETHAM = the analytic Preetham daylight model (jce_sky.h).  Uses the
 *              ToD sun direction when time-of-day is active. */
typedef enum {
    JCE_SCENE_SKY_GRADIENT = 0,
    JCE_SCENE_SKY_EQUIRECT = 1,
    JCE_SCENE_SKY_PREETHAM = 2,
    JCE_SCENE_SKY_STYLIZED = 3,   /* multi-stop dome + horizon glow + sun disk/halo (golden-hour) */
} JceSceneSkyMode;

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

    /* ── Sky (analytic Preetham, additive) ────────────────────────────
     * sky_mode selects the sky pass: GRADIENT (0, default — unchanged
     * legacy behaviour), EQUIRECT (1, the existing HDR skybox path) or
     * PREETHAM (2, the analytic daylight model in jce_sky.h).  When
     * PREETHAM is selected the renderer evaluates jce_sky_evaluate() with
     * the ToD sun direction (or a default high sun) and sky_turbidity.
     * Absent in older scenes → defaults to GRADIENT (no visual change). */
    int   sky_mode;            /* JceSceneSkyMode (0 = gradient)          */
    float sky_turbidity;       /* Preetham haze, ~2-3 clear; clamp [1,10] */

    /* ── Floating origin (large-world precision; opt-in, default OFF) ───
     * When floating_origin_enabled, the runtime periodically re-bases the
     * world so the camera returns toward (0,0,0), keeping float32 transforms
     * precise across very large maps (see jce_world_origin.h).  A rebase
     * fires when |camera_local| exceeds floating_origin_threshold metres and
     * atomically shifts entities + camera + physics bodies by the same delta,
     * so it is visually transparent.  Absent in older scenes → disabled →
     * the frame path is byte-identical to before this feature. */
    bool  floating_origin_enabled;
    float floating_origin_threshold;   /* metres; default 4096 */

    /* Screen-space ambient occlusion (SSAO).  Off by default; absent in older
     * scenes -> disabled -> byte-identical frame path.  When enabled the
     * renderer runs a camera depth pre-pass + SSAO and modulates ONLY the
     * ambient term.  (Reuses the material-AO sampler stage, so an authored AO
     * map is replaced by SSAO while enabled - combining both is a future
     * ORM-pack refinement.) */
    bool  ssao_enabled;
    float ssao_intensity;   /* default 1.5 */
    float ssao_radius;      /* world units; default 1.0 */

    /* Screen-space reflections.  Reflects the lit color buffer along surface
     * normals (reconstructed from the SSAO depth pre-pass).  Composited
     * additively over the scene color by the SSR pass's own coverage/fade. */
    bool  ssr_enabled;
    float ssr_intensity;     /* reflection strength, default 0.6 */
    float ssr_max_distance;  /* ray-march world distance, default 8.0 */

    /* ── TAA tuning (temporal AA).  0 = engine defaults so scenes saved before
     * these existed are byte-identical (feedback 0.9, clamps 1.0). Consumed in
     * the editor game-render TAA setup (jce_postfx_set_taa). */
    float taa_feedback;      /* 0 => 0.9 (higher = softer/more accumulation) */
    float taa_luma_clamp;    /* 0 => 1.0 (variance-box softening) */
    float taa_motion_clamp;  /* 0 => 1.0 (high-motion feedback drop) */

    /* ── Look Profile (stylized vertical slice; foundation plan 02) ─────
     * Continuous APPENDED block.  EVERY field is a neutral / algebraic
     * no-op default so an old scene (block absent → defaults-first parse)
     * and a LOW-tier render are BYTE-IDENTICAL to before this feature.
     * Consumed by: plan 03 (wrap/hemisphere/rim in fs_pbr_body.sh),
     * plan 04 (sky/fog reuse the EXISTING fog_* fields), plan 06 (tonemap/
     * LUT/bloom_knee in the postfx layer).  AERIAL PERSPECTIVE reuses the
     * existing fog_* fields above — do NOT add aerial_* fields here. */
    float wrap_factor;             /* 0 = hard Lambert (neutral), ~0.35 soft */
    bool  ambient_hemisphere;      /* false = neutral (flat u_ambientColor only) */
    float ambient_ground_color[3]; /* bottom hemisphere bounce; TOP=u_ambientColor */
    float rim_color[3];
    float rim_power;
    float rim_intensity;           /* 0 = neutral */
    int   tonemap_op;              /* JceSceneTonemapOp (default ACES=0) */
    char  lut_path[256];           /* "" = no LUT */
    float lut_strength;            /* 0 = neutral */
    bool  toon_character;          /* per-scene default; per-entity MeshRenderer.toon overrides */
    float bloom_knee;              /* 0 = current hard cutoff (neutral) */

    /* ── Stylized sky dome (sky_mode == JCE_SCENE_SKY_STYLIZED == 3) ───
     * A multi-stop vertical color ramp + a horizon glow band + a sun
     * disk/halo aligned to the directional sun (u_sky_sun_dir).  Absent
     * in older scenes → loaded to the golden-hour defaults below but
     * UNUSED unless sky_mode == 3 AND the "stylized_sky" feature gate is
     * enabled (else the renderer downgrades mode 3 → Preetham/Gradient).
     * Pure ALU in the fragment shader: no new view, RT, sampler or pass. */
    float sky_dome_zenith[3];     /* top color (dir.y = +1)                 */
    float sky_dome_mid[3];        /* mid-sky color                          */
    float sky_dome_mid_pos;       /* dir.y where mid sits, [0,1] (def 0.45) */
    float sky_dome_horizon[3];    /* horizon color (dir.y = 0)              */
    float sky_dome_ground[3];     /* below-horizon color (dir.y < 0)        */
    float sky_dome_glow[3];       /* horizon glow band color (additive)     */
    float sky_dome_glow_falloff;  /* exp falloff: band=exp(-|dir.y|*this)   */
    float sky_dome_sun_color[3];  /* sun disk + halo tint                   */
    float sky_dome_sun_size;      /* cos-threshold for the disk core        */
    float sky_dome_sun_softness;  /* smoothstep softness around the disk    */
    float sky_dome_halo_power;    /* halo = pow(max(dot,0), this)           */
    float sky_dome_halo_strength; /* halo additive strength                 */
    float sky_dome_sun_dir[3];    /* authored disk direction (sun by day /
                                   * moon at night). Zero vector = unset →
                                   * legacy behavior (ToD sun, else the
                                   * default high sun), byte-identical.     */
    /* Stylized sun rays (anime-style petal spokes around the disk; the
     * Elemental-Serenity reference draws cos(angle*count)^sharpness rays
     * between sunSize*0.8 and sunSize+length).  count 0 = OFF (bit-exact
     * no-op — scenes authored before these fields are byte-identical).   */
    float sky_dome_anchor_radius; /* > 0: dome anchored at the WORLD origin
                                   * with this sphere radius (reference-style
                                   * sky mesh: horizon + sun/moon parallax
                                   * with the camera).  0 = legacy
                                   * view-direction dome, byte-identical.  */
    float gi_dynamic;             /* GI L1: dynamic irradiance-probe grid
                                   * intensity (screen-gather SH9 through
                                   * the baked-GI funnel).  0 = OFF
                                   * (byte-identical); ~1 = reference.    */
    float sky_dome_ray_count;     /* petal count (e.g. 12); < 0.5 = off    */
    float sky_dome_ray_length;    /* angular length past the disk, RADIANS */
    float sky_dome_ray_sharpness; /* petal pow() exponent (e.g. 8)         */
    float sky_dome_ray_strength;  /* additive strength multiplier          */

    /* Sky IBL toggle: when false, fs_pbr surfaces fall back to the FLAT
     * authored ambient instead of the sky irradiance/prefilter (which fully
     * REPLACES flat ambient when a skybox is active).  Stylized dioramas
     * whose look is driven by an authored AmbientLight (e.g. hand-tuned
     * three.js ports) need this off — otherwise up-facing surfaces take the
     * dome zenith color as ambient and ignore the authored ambient entirely.
     * Absent in older scenes → true (byte-identical).                      */
    bool  ibl_enabled;
} JceSceneRenderingSettings;

/* ── Scene-level world-streaming settings ───────────────────────────
 *
 * Authoring-side description of the open-world streaming setup: the
 * streamer config (mirrors JceWorldStreamConfig field-for-field, kept
 * as plain ints/floats so this header stays free of streaming-layer
 * includes) plus the explicit chunk registry.  Serialized with the
 * scene as a top-level "streaming" object; consumed by the editor's
 * preview streamer and the runtime's default_main world streamer via
 * jce_world_streamer_register_from_scene_settings(). */

/* MUST stay equal to MAX_WORLD_CHUNKS in jce_world_streamer.c — the
 * streamer's roster pool refuses registration #257 onward, so allowing
 * more authored chunks here would silently drop them at runtime. */
#define JCE_SCENE_MAX_STREAM_CHUNKS 256

typedef struct JceSceneStreamChunk {
    uint32_t id;          /* unique chunk id (grid cell index etc.)   */
    float    center[3];   /* world-space chunk centre                 */
    float    radius;      /* spatial radius for the distance test     */
    char     path[256];   /* project-relative scene-fragment JSON     */
} JceSceneStreamChunk;

typedef struct JceSceneStreamingSettings {
    uint32_t version;
    bool     enabled;          /* default false — streaming is opt-in  */
    int      mode;             /* 0 = radial, 1 = rectangular          */
    float    load_radius;      /* metres — start loading inside        */
    float    unload_radius;    /* metres — unload beyond (>= load)     */
    uint32_t max_pending;      /* max concurrent background loads      */
    uint32_t budget_mb;        /* raw chunk-file byte budget, MiB      */
    float    frame_budget_ms;  /* cooperative-mode per-frame budget    */
    uint32_t chunk_count;
    JceSceneStreamChunk chunks[JCE_SCENE_MAX_STREAM_CHUNKS];
} JceSceneStreamingSettings;

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
    /* Optional RETARGET source skeleton. When non-empty AND different from
       skeleton_path, the active clip is treated as authored for THIS (source)
       skeleton: the renderer samples it against the source rig, transfers the
       pose onto this entity's (dst) skeleton via the name-based bind-relative
       retargeter (jce_anim_retarget_*), then evaluates the dst skeleton. Empty
       (the default) ⇒ no retargeting ⇒ legacy single-skeleton path, byte-
       identical. */
    char  retarget_source_skeleton[256];
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
    /* Blend-tree dimensionality (matches JceAnimBlendTreeMode but kept as a
       plain int so this header has no animation dependency):
         0 = 1D (scalar blend_param over blend_thresholds[] — the classic path)
         1 = 2D Freeform Cartesian   (gradient band in (x,y))
         2 = 2D Freeform Directional (gradient band in (angle,magnitude))
       In 2D modes the sample positions are (blend_thresholds[i], blend_pos_y[i])
       and the query point is (blend_param, blend_param_y). Only valid when
       use_blend_tree is set. */
    int   blend_mode;
    float blend_param_y;
    float blend_pos_y[8];
    /* Opt-in convenience: when true, the engine auto-feeds the entity's planar
       movement speed into the SM "Speed" param AND blend_param (during Play),
       so a model "just works" as a locomotion character. Default false — the
       engine then stays generic: blend_param / SM params are whatever game code
       or the authored values set them to (the renderer only evaluates). */
    bool  auto_speed;
    /* ── Transient locomotion state (NEVER serialized) ─────────────────
       Written each fixed tick by the runtime's character driver from live
       physics, consumed by the renderer's SM/blend-tree driver when
       auto_speed is set: Speed/blend_param get loco_speed (steadier than
       the transform-derived estimate), plus the optional SM params
       IsGrounded / VerticalVel / Jump (trigger; loco_jump is one-shot —
       the consumer clears it). loco_valid marks the data live. */
    bool  loco_valid;
    bool  loco_grounded;
    bool  loco_jump;
    float loco_speed;
    float loco_vert_vel;
    /* ── Root motion delta (FEATURE 3.2 — transient, NEVER serialized) ──
       Produced each frame by the renderer's pose evaluator (which owns the
       clip + playhead + skeleton) when the entity's JceAvatarComponent has
       apply_root_motion=true: the root joint's local-space translation step
       and yaw step extracted by jce_anim_extract_root_delta over this frame's
       (prev,cur] playhead window (loop-wrap aware), with the root translation
       stripped from the rendered pose so the mesh stays put.  The runtime's
       character/transform step consumes rm_dx/rm_dy/rm_dz (rotated by the
       entity orientation) + rm_dyaw, then clears rm_valid.  rm_valid marks the
       data live for exactly one consumer read. */
    bool  rm_valid;
    float rm_dx, rm_dy, rm_dz;   /* root local-space translation delta */
    float rm_dyaw;               /* root yaw delta, radians            */
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

/* ── Animation-rigging IK constraints ────────────────────────────────
 *
 * Authored in the editor's Animation Rigging panel and consumed by the
 * scene renderer after pose sampling.  ALL kinds are solved: 0=Aim,
 * 1=TwoBoneIK, 5=CCD, 6=FABRIK (chain/aim solvers) and the single-target
 * parent constraints MultiParent(2)/Position(3)/Rotation(4) — these drive the
 * root bone toward the single target_entity (Position blends translation,
 * Rotation blends orientation, MultiParent both), in the rigged entity's
 * model space, then propagate to descendants.
 *
 * NOTE: there is NO JCE_COMP_FLAG bit for this component — the 64-bit
 * flag field is full.  It is presence-gated (jce_scene_has_ik_constraints),
 * exactly like VideoPlayer/NavAgent, for enumeration and serialization. */
typedef struct {
    int      kind;            /* 0=Aim 1=TwoBoneIK 2=MultiParent 3=Position
                                4=Rotation 5=CCD 6=FABRIK */
    char     name[48];
    float    weight;          /* 0..1 */
    bool     enabled;
    char     root_bone[64];
    char     mid_bone[64];
    char     end_bone[64];
    uint32_t target_entity;   /* 0 = none */
    uint32_t pole_entity;     /* 0 = use pole_offset */
    float    pole_offset[3];  /* model-space offset from root when pole_entity==0 */
} JceIkConstraint;

typedef struct {
    int             count;
    JceIkConstraint constraints[16];
} JceIkConstraintComponent;

/* ── Foot IK (ground-adaptive foot placement) ────────────────────────
 *
 * Authored on a skeletal-animator entity; consumed by the scene renderer
 * (sr_apply_foot_ik) after pose sampling + the rigging IK pass.  Each frame
 * the renderer reads the sampled foot world positions, raycasts DOWN under
 * each foot via the runtime ground-query hook, and bends the legs (two-bone
 * solver) so the feet plant on the ground while the hips drop to the lowest
 * foot.  Bone-name fields are looked up in the animator's skeleton; an empty
 * ankle name disables that leg.  When no ground-query hook is installed (the
 * default in tools / before Play) the renderer pass is a NO-OP, so authoring
 * this component never changes the rendered pose until a runtime wires ground
 * queries in.
 *
 * NOTE: there is NO JCE_COMP_FLAG bit — the 64-bit flag space is full.  It is
 * presence-gated (jce_scene_has_foot_ik), like IkConstraints / NavAgent. */
typedef struct {
    bool     enabled;
    char     pelvis_bone[48];     /* hips/pelvis bone (empty => no hip drop) */
    char     hip_bone  [2][48];   /* [0]=left [1]=right upper-leg root       */
    char     knee_bone [2][48];   /* mid joint per leg                       */
    char     ankle_bone[2][48];   /* end effector per leg (empty => leg off) */
    float    max_step_height;     /* clamp pelvis drop + foot raise (m)      */
    float    foot_offset;         /* lift ankle above the sole (m)           */
    float    cast_up;             /* ray starts this far above the ankle (m) */
    float    cast_down;           /* ray length below the ankle (m)          */
    bool     rotate_to_normal;    /* aim the foot bone to the ground normal  */
    float    blend;               /* 0..1 overall IK weight                  */
    uint32_t reserved;
} JceFootIkComponent;

/* ── Full-Body IK (coordinated multi-effector solve) ─────────────────────
 *
 * Authored on a skeletal-animator entity; consumed by the scene renderer
 * (sr_apply_full_body_ik) after pose sampling + the rigging/foot IK passes.
 * Each effector pulls a named bone toward a WORLD target; the FBBIK solver
 * (jce_anim_fbbik, FABRIK-on-tree) reaches all targets at once while keeping
 * bone lengths rigid — moving a hand propagates through the arm into the shared
 * spine, unlike the single-chain IkConstraints.  Targets are world positions a
 * script / gameplay sets (jce.* or directly); an empty bone name disables a
 * slot.  Like FootIk / IkConstraints there is NO JCE_COMP_FLAG bit (flag space
 * full) — it is presence-gated (jce_scene_has_full_body_ik).  POD only. */
typedef struct {
    char     bone[64];            /* effector joint bone name (empty => off) */
    jce_vec3 target;              /* WORLD-space target position             */
    float    weight;             /* 0..1 per-effector pull                   */
} JceFullBodyIkEffector;

typedef struct {
    bool     enabled;
    int      effector_count;      /* 0..8 active effectors                   */
    JceFullBodyIkEffector effectors[8];
    int      iterations;          /* solver iterations (<=0 => 10)           */
    float    blend;               /* 0..1 overall IK weight (lerp from anim) */
    uint32_t reserved;
} JceFullBodyIkComponent;

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
    /* Lock all 3 angular axes so a DYNAMIC body never tips/rolls but still
     * collides linearly (Unity RigidbodyConstraints.FreezeRotation / Godot
     * lock_rotation).  Applied post-create via setAngularFactor(0). */
    bool     freeze_rotation;
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
    /* Movement feel (0 = engine default, kept for scenes saved before
       these fields existed). Speeds in m/s, accel in m/s^2, turn speed
       in deg/s; sprint_mult scales move_speed while sprinting and
       air_control scales accel while airborne (0..1). */
    float move_speed;
    float sprint_mult;
    float jump_speed;
    float accel;
    float air_control;
    float turn_speed_deg;
} JceCharacterControllerComponent;

/* ── Audio source component ──────────────────────────────────────── */

typedef struct {
    char  clip_path[256];
    float volume;
    float pitch;
    float spatial_blend;
    bool  loop;
    bool  play_on_awake;
    /* 3D attenuation authoring (large-world audio).  0 = legacy defaults
     * (INVERSE, 1..25m, rolloff 1) so scenes saved before these existed are
     * byte-identical.  attenuation_model: 0=default(INVERSE),1=NONE,2=INVERSE,
     * 3=LINEAR,4=EXPONENTIAL.  mixer_bus "" = auto-derive by role. */
    int   attenuation_model;
    float min_distance;     /* 0 => 1.0 default */
    float max_distance;     /* 0 => 25.0 default */
    float rolloff_factor;   /* 0 => 1.0 default */
    char  mixer_bus[64];
} JceAudioSourceComponent;

/* ── Music track component (adaptive / interactive music director) ────
 *
 * Authoring shim for the jce_music director: identifies a music config /
 * asset (track_path) and the initial director state.  When play_on_awake
 * is set, the runtime builds a JceMusicDirector for this track on scene
 * spawn (one director per runtime), seeded with bpm / initial_intensity.
 * Kept deliberately simple — full layer/segment authoring lives in the
 * track config the runtime expands via jce_music_track_desc_default. */
typedef struct {
    char  track_path[256];     /* music config / asset id                */
    bool  play_on_awake;       /* build the director on scene spawn      */
    float initial_intensity;   /* starting intensity (clamped 0..1)      */
    int   bpm;                 /* tempo in beats-per-minute (> 0)        */
} JceMusicTrackComponent;

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
    bool     gpu;                /* request compute-driven simulation; silently
                                  * falls back to CPU when unsupported */

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

/* ── Vegetation Scatter (foliage / grass / trees, P0 roadmap 2.2) ──
 * Deterministically scatters instances of `mesh_path` over the terrain
 * heightfield within an `area` rectangle centered on the entity, drawn with
 * GPU instancing.  Scatter math lives in jce_foliage.h; the renderer caches
 * the instance buffer and rebuilds it only when a parameter changes. */
#define JCE_VEG_PAINT_DIM 64    /* in-editor density-paint grid resolution */
typedef struct {
    char     mesh_path[256];     /* instanced mesh (.glb/.obj/model)              */
    int      mesh_shape;         /* when mesh_path is empty, instance this primitive
                                  * (0=cube,1=sphere,2=plane,3=capsule,4=cylinder) —
                                  * a low-poly ISM path for huge counts of one shape */
    char     albedo_path[256];   /* optional albedo override ("" = mesh material) */
    char     density_mask_path[256]; /* optional grayscale density mask (large-world
                                      * #8a): R channel over the area rect modulates
                                      * per-instance keep-probability ("" = uniform) */
    float    density;            /* instances per square world unit (>0)          */
    uint32_t seed;               /* deterministic scatter seed                    */
    float    area_x;             /* scatter rectangle X size (world units)        */
    float    area_z;             /* scatter rectangle Z size                      */
    float    max_slope_deg;      /* skip terrain steeper than this (>=90 = off)   */
    float    scale_min;          /* per-instance uniform scale range (min<=max)   */
    float    scale_max;
    float    tint[3];            /* multiplied into instance base color           */
    bool     align_to_normal;    /* orient up-axis to terrain normal (else upright) */
    bool     cast_shadow;        /* submit instances to the shadow pass           */
    bool     visible;

    /* In-editor painted density grid (large-world #8a foliage brush): a
     * JCE_VEG_PAINT_DIM² grid of 0(sparse)..255(full) over the area rect.  When
     * density_paint_active it SUPERSEDES density_mask_path; the brush carves it,
     * the renderer feeds it to the scatter, and it serializes inline.  Default
     * all-255 (full density = no change) when first enabled. */
    bool     density_paint_active;
    uint8_t  density_paint[JCE_VEG_PAINT_DIM * JCE_VEG_PAINT_DIM];
} JceVegetationScatterComponent;

/* ── Grass Field (GPU-instanced procedural blades + wind, Stage 1b.6) ──
 * Showcase-tier grass: a dedicated component drawn by sr_draw_grass with ONE
 * GPU-instanced submit per field (NOT the 4096-cap foliage loop, NOT the shared
 * fs_pbr_body lit path).  Scatter fields mirror JceVegetationScatterComponent
 * (deterministic jce_foliage_scatter over the bound terrain); the look fields
 * drive the standalone vs_grass/fs_grass blade shaders.  Gated to
 * instancing-capable HIGH/ULTRA GPUs + JceRenderSettings.grass_enabled. */
typedef struct {
    /* Scatter (mirrors JceVegetationScatterComponent's authorable scatter set). */
    float    density;            /* blades per square world unit (>0)             */
    uint32_t seed;               /* deterministic scatter seed                    */
    float    area_x;             /* scatter rectangle X size (world units)        */
    float    area_z;             /* scatter rectangle Z size                      */
    float    max_slope_deg;      /* skip terrain steeper than this (>=90 = off)   */
    float    scale_min;          /* per-blade uniform scale range (min<=max)      */
    float    scale_max;
    /* Look. */
    float    blade_height;       /* world-unit blade height (mesh + per-blade scale base) */
    float    blade_width;        /* world-unit blade base width                   */
    int      cards;              /* crossed alpha cards per blade (3..6)          */
    float    root_color[3];      /* darker root color (uv.y=0)                    */
    float    tip_color[3];       /* lighter tip color (uv.y=1)                    */
    float    wind_dir[2];        /* wind XZ direction (normalized in shader)      */
    float    wind_speed;         /* wind temporal frequency                       */
    float    wind_amplitude;     /* tip-sway world-unit amplitude                 */
    float    fade_start;         /* distance (m) where height-collapse begins     */
    float    fade_end;           /* distance (m) where blades fully collapse      */
    float    hue_jitter;         /* per-blade green<->blue-green tint jitter [0..1] */
    bool     cast_shadow;        /* v1: ignored (grass non-casting); reserved     */
    bool     visible;
    /* Density mask (large-world/diorama path support): a texture whose GREEN
     * channel gates blade placement — a blade survives only where the mask at
     * its world XZ is >= density_threshold, so dirt paths / water read through
     * the grass instead of being carpeted over.  Empty path = no masking. */
    char     density_mask_path[256];
    float    density_threshold;  /* keep blades where mask.g >= this (0 = off)    */
    float    mask_world_size;    /* world extent the mask UV spans (matches ground) */
} JceGrassFieldComponent;

/* ── Water (Gerstner surface, P0 roadmap 2.3) ─────────────────────
 * A flat `size_x` × `size_z` water plane centered on the entity whose surface
 * is displaced by up to JCE_WATER_COMP_MAX_WAVES Gerstner/sine waves (see
 * jce_water.h for the exact equation).  The CPU model (jce_water_sample_*) is
 * the source of truth the renderer's water vertex shader mirrors; shading uses
 * the shallow/deep color gradient, transparency, and sun specular below. */
#define JCE_WATER_COMP_MAX_WAVES 4

/* Surface synthesis model.  GERSTNER (default, value 0) is the analytic
 * sum-of-sines model in jce_water.h — byte-identical to every prior scene.  FFT
 * (value 1) replaces it with a Tessendorf statistical ocean (jce_water_fft.h):
 * the renderer maintains a per-entity CPU FFT patch + a dynamic displacement
 * texture sampled in the water vertex shader.  Absent in old scenes => GERSTNER,
 * so loading is byte-identical. */
typedef enum {
    JCE_WATER_MODE_GERSTNER = 0,
    JCE_WATER_MODE_FFT      = 1,
    /* STYLIZED (value 2): a flat, discard-everywhere-but-strokes ripple
     * OVERLAY in the hand-painted-diorama style — the water BODY color is
     * painted in the ground beneath; this pass draws only thin shore-hugging
     * ripple arcs (+ rain splash circles + winter ice plates) from a
     * shore-distance data texture.  Requires data_tex. */
    JCE_WATER_MODE_STYLIZED = 2
} JceWaterMode;

typedef struct {
    float        size_x;          /* plane size on X (world units)            */
    float        size_z;          /* plane size on Z (world units)            */
    int          wave_count;      /* active waves [0..JCE_WATER_COMP_MAX_WAVES]*/
    JceWaterWave waves[JCE_WATER_COMP_MAX_WAVES];
    float        base_height;     /* still-water plane Y (entity-local)       */
    float        color_shallow[3];/* color at grazing / shallow depth         */
    float        color_deep[3];   /* color at steep / deep view               */
    float        transparency;    /* 0 = opaque, 1 = fully transparent        */
    float        sun_specular;    /* sun highlight intensity                  */
    float        shore_ripple;    /* stylized shore ripple-ring strength
                                   * (0 = off; old scenes render identically) */
    float        ice_ratio;       /* frozen-surface crackle blend [0..1]
                                   * (0 = off; drives the winter look)        */
    float        splash_ratio;    /* STYLIZED mode: rain splash-circle density
                                   * [0..1] (0 = off)                          */
    char         data_tex[256];   /* STYLIZED mode: shore-distance data map —
                                   * R = normalized distance from shore (0 at
                                   * the waterline), G = in-water mask         */
    bool         visible;

    /* ── FFT ocean (Tessendorf) — additive; inert unless water_mode==FFT ──
     * Defaults give a plausible open-ocean patch; GERSTNER (the default mode)
     * ignores all of these, so old scenes round-trip byte-identically. */
    int          water_mode;      /* JceWaterMode (0 GERSTNER default, 1 FFT)  */
    float        fft_patch_size;  /* tiling patch world side length (>0)       */
    float        fft_wind_speed;  /* wind speed m/s -> dominant wavelength     */
    float        fft_wind_dir_x;  /* wind direction (normalized in the core)   */
    float        fft_wind_dir_z;
    float        fft_amplitude;   /* Phillips energy scale (wave height)       */
    int          fft_resolution;  /* FFT grid N; clamped to a power-of-2 [32,256] */
} JceWaterComponent;

/* ── Buoyancy (floats a dynamic body on the active water surface, gap 2.3) ─
 * Mass-independent vertical buoyancy + drag applied per fixed physics tick by
 * the runtime.  The entity must ALSO have a dynamic RigidBody + a collider; the
 * runtime samples the first enabled JceWaterComponent in the scene (like
 * foliage finds terrain), computes submersion depth at the body's XZ, and feeds
 * jce_water_buoyancy_force().  See jce_water.h for the exact force equation. */
typedef struct {
    float buoyancy_strength;  /* upward force per metre of submersion (>0)     */
    float drag;               /* vertical linear drag while submerged (>=0)    */
    bool  enabled;            /* false ⇒ runtime skips this body (zero force)  */
} JceBuoyancyComponent;

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
    /* Cross-fade band width (world meters) around each switch distance over
     * which the LOD transition is blended (large-world-opt P1 #6).  0 = hard
     * switch (the legacy behaviour, with hysteresis only).  Authoring hint
     * consumed by the LOD selection (wider band = softer pop); serialized so a
     * project can tune popping per LODGroup. */
    float fade_width;

    /* ── Octahedral impostor terminal LOD (roadmap P2 #10) ─────────────
     * Beyond the last mesh LOD, render a single camera-facing card sampling a
     * pre-baked octahedral atlas (RDR2 / Genshin / UE technique).  A whole
     * forest of far trees collapses to a handful of instanced quads.
     *
     * impostor_meta_path: project-relative .impostor.json (atlas + grid + bounds
     *   metadata produced by the editor "Bake Impostor" button).  Empty =>
     *   no impostor (byte-identical to the pre-impostor LODGroup).
     * impostor_distance: camera distance (world meters) at/beyond which the
     *   impostor card replaces the mesh.  Should be >= the last LOD distance. */
    char  impostor_meta_path[256];
    float impostor_distance;
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
    /* Prefab instantiated for each spawned ped (project-relative path, resolved
     * through the runtime asset resolver).  Empty ⇒ the manager runs its
     * distance logic but spawns nothing (the previous inert behaviour). */
    char     ped_prefab_path[128];
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
    bool  math_text;          /* render `text` as math markup (\mu ^x _y) */
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

/* ── UI: Slider (draggable value track + handle) ───────────────────
 *
 * A draggable value widget.  The canvas hit-tests the resolved rect and,
 * while the pointer is held, maps the pointer position along `direction`
 * to a value in [min_value,max_value], WRITING IT BACK into `value`
 * (Unity model: the widget is the source of truth).  Unlike UIButton this
 * component embeds its OWN RectTransform so a slider needs no sibling
 * UIImage to be laid out / raycast.  POD only. */
typedef struct {
    float value;            /* current value (canvas writes this back) */
    float min_value;
    float max_value;
    int   direction;        /* 0=L→R, 1=R→L, 2=B→T, 3=T→B */
    bool  interactable;
    bool  whole_numbers;    /* round value to the nearest integer */
    float bg_color[4];      /* track background quad */
    float fill_color[4];    /* filled portion quad */
    float handle_color[4];  /* handle quad */
    float handle_size;      /* handle px size; 0 ⇒ default ~20 */
    char  handle_sprite[256];
    char  fill_sprite[256];
    char  on_value_changed[128]; /* script handler name (placeholder) */
    JceRectTransform rect;  /* layout (see JceRectTransform) */
} JceUISliderComponent;

/* ── UI: Toggle (clickable on/off with checkmark) ──────────────────
 *
 * A click toggles `is_on`; the canvas flips it in place (Unity model:
 * the widget is the source of truth).  Embeds its OWN RectTransform.
 * POD only. */
typedef struct {
    bool  is_on;            /* current state (canvas writes this back) */
    bool  interactable;
    float bg_color[4];      /* background quad */
    float checkmark_color[4]; /* checkmark quad, drawn only when is_on */
    char  bg_sprite[256];
    char  checkmark_sprite[256];
    char  on_value_changed[128]; /* script handler name (placeholder) */
    JceRectTransform rect;  /* layout (see JceRectTransform) */
} JceUIToggleComponent;

/* ── UI: InputField (single-line text entry) ───────────────────────
 *
 * A focusable single-line text-entry widget (forms, name entry, search,
 * chat).  The component's `text` field is the SOURCE OF TRUTH (Unity
 * model): the canvas writes every edit (insert / delete / caret motion)
 * back into it in place, exactly like UISlider.value / UIToggle.is_on.
 * Embeds its OWN RectTransform so it needs no sibling UIImage to be laid
 * out / raycast.  POD only (no renderer / bgfx types). */
typedef struct {
    char  text[256];           /* current value (canvas writes this back) */
    char  placeholder[128];    /* shown (greyed) when text empty & unfocused */
    int   content_type;        /* 0=any 1=integer 2=decimal 3=alphanumeric */
    int   char_limit;          /* max chars; 0 ⇒ buffer cap (255) */
    bool  is_password;         /* render as '*' (value stored in clear) */
    bool  read_only;           /* focusable but text input is ignored */
    bool  interactable;        /* false ⇒ cannot be focused */
    float bg_color[4];         /* background quad */
    float text_color[4];       /* entered-text colour */
    float placeholder_color[4];/* placeholder colour */
    float caret_color[4];      /* blinking caret quad */
    float font_size;           /* px; 0 ⇒ default ~16 */
    char  font_path[256];
    char  on_submit[128];        /* script handler name (RETURN; placeholder) */
    char  on_value_changed[128]; /* script handler name (edit; placeholder) */
    JceRectTransform rect;     /* layout (see JceRectTransform) */
} JceUIInputFieldComponent;

/* ── UI: ScrollView (clipped, scrollable content viewport) ─────────
 *
 * A CONTAINER widget: its descendants are OFFSET by -scroll_position and
 * CLIPPED to the viewport (the ScrollView's own resolved rect) so content
 * larger than the rect (lists, inventories, chat logs) can scroll.  The
 * canvas tracks the scroll-view under the pointer and applies wheel deltas
 * to `scroll_position`, clamping each axis to [0, max(0, content-viewport)]
 * and WRITING IT BACK in place (Unity model: the widget is the source of
 * truth, exactly like UISlider.value / UIInputField.text).  Embeds its OWN
 * RectTransform so it needs no sibling UIImage to be laid out / raycast.
 * POD only (no renderer / bgfx types). */
typedef struct {
    float content_size[2];     /* px extent of scrollable content; 0 ⇒ treat as viewport size (no scroll) */
    float scroll_position[2];  /* current px offset (canvas writes back); clamped [0, max(0,content-viewport)] */
    bool  horizontal;          /* enable horizontal scroll axis */
    bool  vertical;            /* enable vertical scroll axis */
    float scroll_sensitivity;  /* px per wheel notch; 0 ⇒ default ~30 */
    bool  show_scrollbar;      /* draw scrollbar track + thumb */
    float scrollbar_thickness; /* px; 0 ⇒ default ~8 */
    float bg_color[4];         /* viewport background quad */
    float scrollbar_color[4];  /* scrollbar thumb quad */
    float scrollbar_bg_color[4];/* scrollbar track quad */
    bool  interactable;        /* false ⇒ wheel/drag ignored */
    JceRectTransform rect;     /* layout (see JceRectTransform) */
} JceUIScrollViewComponent;

/* ── UI: ProgressBar (read-only fill bar) ──────────────────────────
 *
 * A purely visual fill widget (loading/health/XP bars).  Unlike UISlider it
 * is NOT interactive: the canvas only DRAWS it (bg quad + a fill quad sized
 * by clamp((value-min)/(max-min)) along `direction`) and never hit-tests or
 * mutates it — `value` is driven entirely by gameplay/script.  Embeds its OWN
 * RectTransform so it needs no sibling UIImage to be laid out.  POD only. */
typedef struct {
    float value;            /* current fill value (read-only; gameplay sets) */
    float min_value;
    float max_value;
    int   direction;        /* 0=L→R, 1=R→L, 2=B→T, 3=T→B */
    float bg_color[4];      /* track background quad */
    float fill_color[4];    /* filled portion quad */
    char  fill_sprite[256]; /* optional sprite for the fill quad */
    JceRectTransform rect;  /* layout (see JceRectTransform) */
} JceUIProgressBarComponent;

/* ── UI: Dropdown (expandable option selector) ─────────────────────
 *
 * A click on the collapsed main rect toggles `expanded`; while expanded a
 * click on an option row sets `selected_index` and collapses, and a click
 * OUTSIDE collapses without changing selection.  The canvas writes both
 * `selected_index` and `expanded` back into the component in place (Unity
 * source-of-truth model, exactly like UISlider.value / UIToggle.is_on).
 * Embeds its OWN RectTransform so it needs no sibling UIImage.  POD only. */
typedef struct {
    char  options[8][64];     /* option labels (up to JCE_UI_DROPDOWN_MAX_OPTIONS) */
    int   option_count;       /* number of valid options (0..8) */
    int   selected_index;     /* current selection (canvas writes this back) */
    bool  expanded;           /* popup open state (canvas writes this back) */
    bool  interactable;       /* false ⇒ clicks ignored (cannot expand) */
    float bg_color[4];        /* collapsed main-rect background quad */
    float text_color[4];      /* selected/option label colour */
    float popup_color[4];     /* expanded option-list background quad */
    float highlight_color[4]; /* hovered/selected option-row highlight quad */
    float font_size;          /* px; 0 ⇒ default ~16 */
    char  font_path[256];
    char  on_value_changed[128]; /* script handler name (placeholder) */
    JceRectTransform rect;    /* layout (see JceRectTransform) */
} JceUIDropdownComponent;

enum { JCE_UI_DROPDOWN_MAX_OPTIONS = 8 };

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

/* RETIRED (v0.9.9 consolidation): the orphaned VFX Graph runtime duplicated
 * the JceParticleSystem pipeline and was removed.  The scene JSON loader
 * migrates legacy "VfxGraph" components one-way onto
 * JceParticleEmitterComponent (graphPath -> asset_path); nothing sets,
 * saves, or draws this component anymore.  The struct, accessors, and
 * JCE_COMP_FLAG_VFX_GRAPH bit are kept only for source/ABI stability —
 * do not wire new features to them. */
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
/* One additive/override animation layer authored on the Avatar (FEATURE 3.3).
 * Composed on top of the SkeletalAnimator's base pose by the scene renderer
 * via jce_anim_player_blend_layers: each layer samples `clip` (looked up by
 * name in the model), optionally masked by `mask_path` (.mask asset), scaled
 * by `weight`, and composited per `mode`. */
typedef struct {
    char  clip[64];                   /* clip name in the rigged model */
    char  mask_path[128];             /* optional .mask asset (empty = all bones) */
    float weight;                     /* 0..1 layer weight */
    int   mode;                       /* 0 = additive, 1 = masked override
                                         (matches JceAnimLayerMode) */
} JceAvatarLayer;

#define JCE_AVATAR_MAX_LAYERS 4

typedef struct {
    char     avatar_path[128];        /* .avatar asset */
    char     mask_path  [128];        /* optional .mask asset */
    char     override_controller[128];/* optional anim override controller */
    bool     apply_root_motion;
    bool     human_rig;               /* false = generic */
    /* Additive/override layer stack (FEATURE 3.3). layer_count layers are
       composited over the SkeletalAnimator's base pose during Play. */
    int            layer_count;
    JceAvatarLayer layers[JCE_AVATAR_MAX_LAYERS];
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

/* ── Occlusion portal (P4-C — flag 63) ──────────────────────────────
 *
 * RESERVED for a future portal/cell occlusion culler.  This component
 * is authored/serialized but NOT consumed by the current GPU-query
 * occlusion culler (jce_occlusion_culler.c), which tests per-renderable
 * bounding boxes and has no portal/cell concept.  The inspector shows
 * an "unwired" badge for it until that culler lands. */

typedef struct {
    jce_vec3 size;      /* portal extents (world-space box around the entity) */
    bool     open;      /* true = portal open; objects behind it are visible  */
    int32_t  portal_id; /* user-assigned ID for pairing portals               */
} JceOcclusionPortalComponent;

/* ── Nav-mesh agent (Detour crowd steering) ──────────────────────────
 *
 * NOTE: there is NO JCE_COMP_FLAG bit for this component — the 64-bit
 * flag field is full.  It is presence-gated (jce_scene_has_nav_agent),
 * exactly like VideoPlayer, for enumeration and serialization. */
typedef struct {
    float radius;            /* m; <=0 -> engine default 0.4 */
    float height;            /* m; editor gizmo only */
    float max_speed;         /* m/s; <=0 -> 3.0 */
    float max_accel;         /* m/s^2; <=0 -> 12.0 */
    float arrive_radius;     /* <=0 -> 1.5 */
    float waypoint_radius;   /* <=0 -> 0.5 */
    float target[3];         /* world destination; only XZ consumed */
    uint64_t target_entity;  /* 0 = use target[]; else follow that entity */
    bool  auto_repath;       /* re-issue set_destination when target moves */
    bool  enabled;
} JceNavAgentComponent;

/* ── Sequence player (Sequencer .seq.json playback) ──────────────────
 *
 * Binds an authored sequencer asset to scene entities and drives it at
 * runtime via jce_scene_sequencer_update() (jce_scene_sequencer.h).
 *
 * NOTE: there is NO JCE_COMP_FLAG bit for this component — the 64-bit
 * flag field is full.  It is presence-gated (jce_scene_has_sequence_player),
 * exactly like VideoPlayer/NavAgent/IkConstraints, for enumeration and
 * serialization. */
#define JCE_SEQ_PLAYER_MAX_BINDINGS 32
typedef struct {
    char     seq_path[256];
    bool     play_on_awake;
    bool     loop_override;     /* loop value used when override_loop */
    bool     override_loop;     /* true = force loop_override over the asset's */
    float    speed;             /* <=0 => 1.0 */
    int      binding_count;
    uint64_t bindings[JCE_SEQ_PLAYER_MAX_BINDINGS]; /* per-track entity refs */
    /* runtime (not serialized; zeroed on load) */
    void    *seq;               /* JceSeqPlayerRuntime* (jce_scene_sequencer.c) */
    float    prev_time;
    bool     started;
    uint64_t opened_hash;       /* FNV-1a of seq_path the runtime opened with */
} JceSequencePlayerComponent;

/* ── Morph weights (per-instance blendshape authoring, FEATURE 3.1) ──
 *
 * Closes the morph last-mile: a designer pins STATIC per-instance morph-target
 * weights on a skinned/morph-target entity (e.g. a permanent "smile" 0.7).
 * `weights[i]` is target i's authored weight; a bit set in `override_mask`
 * means that target's authored value OVERRIDES any animation-track value
 * (so a designer can pin a blendshape to exactly 0.0).  The scene renderer
 * combines these with the clip-driven track weights via
 * jce_morph_resolve_weights before jce_morph_apply.
 *
 * `count` is how many sliders the inspector exposed (== the model's morph
 * target count, clamped); it bounds resolution/serialization.  There is NO
 * JCE_COMP_FLAG bit — the 64-bit flag field is full; presence-gated like
 * VideoPlayer/NavAgent/SequencePlayer. No component present -> the renderer
 * morph path is byte-identical to today (track-only / inert). */
typedef struct {
    int      count;                          /* authored target count (0..MAX) */
    uint32_t override_mask;                   /* bit t set => weights[t] overrides track */
    float    weights[JCE_MORPH_MAX_WEIGHTS]; /* per-target static weight 0..1   */
} JceMorphWeightsComponent;

/* ── Network variable authoring (FEATURE 7.2 last-mile) ──────────────
 *
 * Closes the networking authoring gap: typed NetworkVariables already exist
 * as ENGINE flecs components (JceNetVarF32/I32) replicated on the snapshot
 * substrate (authority gate, OnValueChanged, delta/baseline/late-joiner) —
 * but a DESIGNER could not, in the editor, mark an entity as carrying a
 * replicated variable.  This authorable scene component is that mark: it
 * records WHICH typed variable an entity carries, under WHICH authority,
 * with an initial value.  At Play the runtime net bridge reads it and
 * registers the matching typed NetVar for replication via the public net
 * API (jce_net_var_f32_set / jce_net_var_i32_set under the authored
 * authority).  There is NO JCE_COMP_FLAG bit — the 64-bit flag field is
 * full; presence-gated like VideoPlayer/NavAgent/SequencePlayer/MorphWeights.
 * No component present -> the runtime net path is byte-identical to today. */
typedef enum {
    JCE_NETVAR_AUTHOR_TYPE_F32  = 0,   /* replicated 32-bit float       */
    JCE_NETVAR_AUTHOR_TYPE_I32  = 1,   /* replicated 32-bit signed int  */
    JCE_NETVAR_AUTHOR_TYPE_BOOL = 2    /* replicated bool (wire: i32 0/1) */
} JceNetVarAuthorType;

typedef enum {
    JCE_NETVAR_AUTHOR_AUTH_SERVER = 0, /* server is authoritative        */
    JCE_NETVAR_AUTHOR_AUTH_CLIENT = 1, /* any owning client may write    */
    JCE_NETVAR_AUTHOR_AUTH_OWNER  = 2  /* only the owner client may write */
} JceNetVarAuthorAuthority;

typedef struct {
    char    var_name[64];   /* designer label for the replicated variable     */
    uint8_t var_type;       /* JceNetVarAuthorType (F32 / I32 / Bool)         */
    uint8_t authority;      /* JceNetVarAuthorAuthority (Server/Client/Owner) */
    float   initial_value;  /* covers F32 directly; I32 via round, Bool via !=0 */
} JceNetworkVariableComponent;

/* ── Gameplay Ability System authoring (consumption last-mile) ───────
 *
 * Closes the GAS authoring gap: the GAS CORE (engine/middleware/world
 * jce_gas.h — attributes/effects/abilities, deterministic, headless) is done
 * and tested, but a DESIGNER could not, in the editor, mark an entity as
 * carrying an ability system, and a SCRIPT could not act on a live one.  This
 * authorable scene component is the SETUP: it records the entity's attribute
 * table (name/base/min/max) and ability table (name/id/cost-attr/cost/cooldown)
 * — the serializable definition, NOT the transient runtime active-effects /
 * cooldown state.  At Play the runtime (rt_spawn_gameplay) reads it, inits a
 * live per-entity JceGameplayAbilitySystem from these tables, ticks it each
 * frame (jce_gas_tick), and exposes it to scripts (jce.gas_activate /
 * jce.gas_get / jce.gas_apply) via jce_runtime_entity_gas.
 *
 * The capacities here are deliberately SMALLER than (and independent of) the
 * GAS core caps so that scene bytes stay stable regardless of any later core
 * cap change; the runtime clamps when copying into the live system.  There is
 * NO JCE_COMP_FLAG bit — the 64-bit flag field is full; presence-gated like
 * VideoPlayer/NavAgent/SequencePlayer/MorphWeights/NetworkVariable.  No
 * component present -> the runtime gameplay path is byte-identical to today. */
#define JCE_GAS_AUTHOR_MAX_ATTRIBUTES 16
#define JCE_GAS_AUTHOR_MAX_ABILITIES  16
#define JCE_GAS_AUTHOR_NAME_LEN       64

typedef struct {
    char  name[JCE_GAS_AUTHOR_NAME_LEN]; /* attribute label (e.g. "Health")    */
    float base;                          /* persistent stored value            */
    float min;                           /* clamp floor                        */
    float max;                           /* clamp ceiling                      */
} JceGasAttributeAuthor;

typedef struct {
    char     name[JCE_GAS_AUTHOR_NAME_LEN]; /* ability label                   */
    uint32_t id;                            /* caller identity (script handle)  */
    int32_t  cost_attr_idx;                 /* -1 = free; else attribute index  */
    float    cost_magnitude;                /* deducted from base on activate   */
    float    cooldown_seconds;              /* re-activation gate               */
} JceGasAbilityAuthor;

typedef struct {
    int32_t               attribute_count;  /* 0..JCE_GAS_AUTHOR_MAX_ATTRIBUTES */
    JceGasAttributeAuthor attributes[JCE_GAS_AUTHOR_MAX_ATTRIBUTES];
    int32_t               ability_count;    /* 0..JCE_GAS_AUTHOR_MAX_ABILITIES  */
    JceGasAbilityAuthor   abilities[JCE_GAS_AUTHOR_MAX_ABILITIES];
} JceGameplayAbilitySystemComponent;

/* ── Ragdoll authoring (skeleton-driven physics, scene-pass last-mile) ──
 *
 * Closes the ragdoll authoring gap: the ragdoll CORE (engine/src/middleware/
 * animation jce_ragdoll.{h,c} — a per-bone dynamic body chain built from a
 * skeleton's bind pose, stepped in a JcePhysicsWorld, with an
 * animation<->physics blend weight) is done and unit-tested, but a DESIGNER
 * could not mark a skeletal-animator entity as carrying a ragdoll.  This
 * authorable scene component is that mark + its tuning.
 *
 * At Play the runtime (rt_spawn_gameplay) reads it, loads the entity's
 * SkeletalAnimator skeleton, builds a live JceRagdoll in rt->physics, drives
 * the bodies toward the source pose BEFORE jce_physics_step (sync_from) and
 * publishes the resolved per-bone LOCAL pose back into the SHARED scene via the
 * transient JceRagdollPoseRelay AFTER the step (sync_to).  The scene renderer
 * reads ONLY that relay and feeds it through jce_skeleton_evaluate into the skin
 * palette — it never touches physics, keeping the scene layer physics-agnostic.
 *
 *   blend_weight = 1 : full animation drive (bodies track the source pose).
 *   blend_weight = 0 : full physics (the body chain collapses — death blend).
 *
 * There is NO JCE_COMP_FLAG bit — the 64-bit flag field is full; presence-gated
 * like VideoPlayer/NavAgent/SequencePlayer/MorphWeights/NetworkVariable/GAS.
 * No component present -> the runtime + renderer paths are byte-identical to
 * today (zero ragdolls -> the hot loop is unchanged). */
typedef struct {
    bool     enable;        /* designer toggle (runtime spawns only if true)   */
    float    blend_weight;  /* 1 = animation, 0 = physics; live-editable        */
    float    radius;        /* per-bone capsule radius (metres)                 */
    float    height_scale;  /* scales each capsule length vs the bind segment   */
    uint32_t reserved[4];   /* forward-compat padding (serialized as zeros)     */
} JceRagdollComponent;

/* ── Ragdoll pose relay (TRANSIENT runtime pose hand-off, NOT serialized) ──
 *
 * The runtime publishes the ragdoll's resolved per-bone LOCAL transforms here
 * each step; the scene renderer reads them and evaluates them into the skin
 * palette.  It carries ONLY public math (jce_mat4) + POD — NO physics types —
 * so BOTH the application layer (runtime) and the scene layer (renderer) compile
 * against it without a layer-up dependency.  It is registered as a flecs
 * component but is explicitly EXCLUDED from scene serialization (it is runtime-
 * only state that is rebuilt every frame). */
typedef struct {
    jce_mat4 locals[JCE_MAX_BONES]; /* per-joint LOCAL transforms (sync_to_pose) */
    uint32_t count;                 /* number of valid joints in locals[]        */
    bool     valid;                 /* true once the runtime has published a pose */
} JceRagdollPoseRelay;

/* ── Fracture / destruction authoring (Voronoi shatter, opt-in) ──────
 *
 * Marks an entity as FRACTURABLE: when it breaks, its intact rigid body is
 * destroyed and replaced by `fragment_count` dynamic CONVEX_HULL fragment
 * bodies produced by a deterministic Voronoi box-shatter of the entity's AABB
 * (engine/middleware/physics jce_fracture.{h,c}).  The fragments inherit the
 * parent body's velocity (plus a small outward impulse) and get
 * mass = density * cell_volume.
 *
 * The break is triggered explicitly via jce_runtime_fracture_entity() (and may
 * be auto-triggered from a hard contact in a follow-up — see runtime).  The
 * body-swap is performed POST-step (deferred), never mid-step, so Bullet's
 * solver is never mutated mid-solve.
 *
 * `enabled` is the GATE: default OFF.  No component (or enabled == false) ->
 * the runtime fracture path is never entered and the frame is byte-identical to
 * today.  There is NO JCE_COMP_FLAG bit — the 64-bit flag field is full;
 * presence-gated like Ragdoll/GAS/MorphWeights. */
typedef struct {
    bool     enabled;        /* designer toggle (default OFF -> inert)          */
    int      fragment_count; /* Voronoi seeds == fragment count (default 8)     */
    float    break_impulse;  /* contact impulse / rel-velocity break threshold  */
    float    density;        /* fragment mass = density * cell volume (kg/m^3)  */
    uint32_t seed;           /* deterministic shatter seed (default 12345)      */
    uint32_t reserved[3];    /* forward-compat padding (serialized as zeros)    */
} JceFractureComponent;

/* ── Vehicle chassis authoring (btRaycastVehicle, opt-in) ────────────
 *
 * Marks an entity as the CHASSIS of a raycast vehicle.  When enabled, the
 * runtime (rt_try_spawn_vehicle) creates a single dynamic chassis rigid body
 * via the engine vehicle API (jce_physics_vehicle_*) and attaches wheels: one
 * per child entity carrying a JceWheelColliderComponent, or — when the chassis
 * has no wheel-collider children — four default wheels synthesized at the
 * chassis corners so a bare vehicle entity still drives.  The chassis box,
 * mass and drive forces below override the engine defaults.
 *
 * `enabled` is the GATE: default OFF.  No component (or enabled == false) ->
 * the entity spawns as a normal rigid body and the frame is byte-identical to
 * today.  Like Fracture/Ragdoll this is presence-gated — there is NO
 * JCE_COMP_FLAG bit (the 64-bit flag field is full).
 *
 * POD only: no physics types leak into this scene header. */
enum {
    JCE_VEHICLE_DRIVE_RWD = 0,   /* rear-wheel drive (default)      */
    JCE_VEHICLE_DRIVE_FWD = 1,   /* front-wheel drive               */
    JCE_VEHICLE_DRIVE_AWD = 2,   /* all-wheel drive                 */
};
enum {
    JCE_VEHICLE_INPUT_SCRIPT = 0, /* driven by script/host API only */
    JCE_VEHICLE_INPUT_PLAYER = 1, /* auto-mapped from player input  */
};
typedef struct {
    bool     enabled;                /* designer toggle (default OFF -> inert)   */
    float    chassis_half_extents[3];/* (0,0,0) = derive from BoxCollider/default*/
    float    chassis_mass;           /* kg; <=0 -> default 1500                  */
    float    max_engine_force;       /* N;  <=0 -> default 4000                  */
    float    max_brake_force;        /* N per wheel; <=0 -> default 100          */
    float    max_steering_deg;       /* deg; <=0 -> default 30                   */
    int      drive_mode;             /* JCE_VEHICLE_DRIVE_* (default RWD)        */
    int      input_mode;             /* JCE_VEHICLE_INPUT_* (default PLAYER)     */
    uint32_t reserved;               /* forward-compat padding (=0)              */
} JceVehicleComponent;

/* ── Volumetric / pressure soft-body authoring (presence-gated, opt-in) ──
 *
 * Marks an entity as a CLOSED-volume "squishy" soft body (a pressurised
 * ellipsoid shell that compresses on impact and rebounds), complementing the
 * surface Cloth component.  When enabled, the runtime (rt_try_spawn_softbody)
 * creates the body in the shared secondary soft world via the public soft-body
 * API (jce_softbody_*) at the entity transform, mirrors the scene's static box
 * colliders into that world so the body can rest on the ground, and writes the
 * body's centroid back to the entity Transform each step.
 *
 * `enabled` is the GATE: default OFF.  No component (or enabled == false) ->
 * the entity spawns as a normal rigid body and the frame is byte-identical to
 * today.  Like Vehicle/Fracture/Ragdoll this is presence-gated — there is NO
 * JCE_COMP_FLAG bit (the 64-bit flag field is full).
 *
 * POD only: no physics types leak into this scene header. */
typedef struct {
    bool     enabled;          /* designer toggle (default OFF -> inert)        */
    float    radius[3];        /* per-axis ellipsoid radii (default 0.5 each)   */
    float    mass;             /* kg; <=0 -> default 1                          */
    float    pressure;         /* kPR; >0 resists volume loss (squish)          */
    float    stiffness_linear; /* 0..1 (Bullet kLST)                            */
    float    stiffness_volume; /* 0..1 (Bullet kVST)                            */
    float    damping;          /* 0..1 (Bullet kDP)                             */
    float    friction;         /* 0..1 (Bullet kDF)                             */
    int      resolution;       /* node density (e.g. 64..256); default 96       */
    bool     self_collision;   /* expensive cluster self-collision; off = cheap */
    uint32_t reserved;         /* forward-compat padding (=0)                   */
} JceSoftBodyComponent;

/* ── Simulation LOD (distance-tiered gameplay tick, opt-in) ──────────
 *
 * Closes the open-world gameplay-CPU gap: M3 streams gameplay binary in/out
 * with a cell, but every RESIDENT NPC/script/AI ticks at full frame rate.
 * This component opts an entity into distance-tiered simulation: the runtime
 * (rt_tick_gameplay) classifies it each frame by distance(entity, viewer)
 * into NEAR / MID / FAR (with jce_lod hysteresis so a tier doesn't thrash on
 * a boundary), and gates that entity's gameplay subsystems — its script
 * on_update, nav-agent steering, and behavior-tree tick — to the tier's Hz
 * via a per-entity accumulator (generalising the BehaviorTree tick cadence).
 * When a gated update fires it is fed the ACCUMULATED dt since its last tick
 * (not a fixed step), so time-based logic stays correct at any rate.
 *
 * NEAR (within near_radius) runs at near_hz (0 == every frame, the default —
 * identical to no component); MID (near..mid radius) at mid_hz; FAR (beyond
 * mid_radius) at far_hz.  An Hz of 0 means "every frame"; a NEGATIVE Hz means
 * "pause this tier" (skip the gated subsystems entirely for that tier).
 *
 * `flags` selects WHICH subsystems the tier rate gates (a clear bit means the
 * subsystem always runs full-rate even on a tiered entity).  Animation is
 * gated separately via gate_anim_far (only far entities ever reduce anim
 * sample rate; near/mid stay smooth).
 *
 * Presence-gated like Vehicle/Fracture/Ragdoll/SoftBody — there is NO
 * JCE_COMP_FLAG bit (the 64-bit flag field is full).  No component present ->
 * the entity ticks full-rate exactly as before (zero regression). */
#define JCE_SIMLOD_GATE_SCRIPT  (1u << 0)  /* gate the Script on_update          */
#define JCE_SIMLOD_GATE_NAV     (1u << 1)  /* gate the NavAgent steering         */
#define JCE_SIMLOD_GATE_BT      (1u << 2)  /* gate the BehaviorTree tick         */
/* Cost-tier gates (large-world #3): far tier sleeps the rigid body / freezes the
 * skinned pose, bounding per-actor CPU in crowds.  OPT-IN — deliberately NOT in
 * _ALL, so an entity with gate_mask 0 (=> _ALL) keeps full-rate physics+anim
 * exactly as before.  Author gate_mask |= these bits to enable. */
#define JCE_SIMLOD_GATE_PHYSICS (1u << 3)  /* far tier -> sleep the rigid body   */
#define JCE_SIMLOD_GATE_ANIM    (1u << 4)  /* far tier -> freeze the skinned pose*/
#define JCE_SIMLOD_GATE_ALL    (JCE_SIMLOD_GATE_SCRIPT | \
                                JCE_SIMLOD_GATE_NAV | JCE_SIMLOD_GATE_BT)
typedef struct {
    bool     enabled;        /* designer toggle (default ON; OFF -> full-rate)  */
    float    near_radius;    /* m; <=0 -> default 25                            */
    float    mid_radius;     /* m; <=0 -> default 80 (>= near_radius)           */
    float    near_hz;        /* tick Hz inside near_radius (0 = every frame)    */
    float    mid_hz;         /* tick Hz in the mid band   (~10; 0 = every frame)*/
    float    far_hz;         /* tick Hz beyond mid_radius (~1; <0 = pause)      */
    uint32_t gate_mask;      /* JCE_SIMLOD_GATE_* bits (0 -> default = ALL)     */
    bool     gate_anim_far;  /* authoring hint: reduce FAR-tier anim sample rate.
                              * Read via jce_scene_get_sim_lod by a renderer that
                              * opts in; the runtime gameplay tick does not own
                              * the animation sample path, so this is a forward
                              * authoring field (the script/nav/BT gating above is
                              * the active CPU win). */
} JceSimLodComponent;

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
#define JCE_COMP_FLAG_VFX_GRAPH            (UINT64_C(1) << 56) /* RETIRED v0.9.9 — loader migrates to ParticleEmitter; bit kept reserved */
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

/* Thin JSON round-trip wrappers for rendering settings — useful for tests
 * and tooling that operates on settings in isolation (without a full scene).
 * to_json: serializes to a JSON string (caller must jce_json_free_string() the result).
 * from_json: parses from that same JSON string; seeds from defaults so
 *   absent keys land on golden-hour/neutral values (old-scene compat).
 * Both accept a minimal flat format, e.g. {"sky":{"mode":2}}, as well as
 * the full nested format produced by to_json. */
JCE_API char *jce_scene_rendering_settings_to_json(
                  const JceSceneRenderingSettings *r);
JCE_API bool  jce_scene_rendering_settings_from_json(
                  const char *json, JceSceneRenderingSettings *out);

/* Scene-level world-streaming settings.  Same lifecycle contract as the
 * rendering settings (has/set/get/get_mut/clear), but the ~70 KB block is
 * heap-allocated lazily — scenes that never author streaming pay one
 * pointer.  set() sanitizes (unload >= load, counts/paths clamped);
 * get() returns NULL until authored; clear() frees the block. */
JCE_API JceSceneStreamingSettings
                  jce_scene_streaming_settings_default(void);
JCE_API bool      jce_scene_has_streaming_settings(const JceScene *scene);
JCE_API void      jce_scene_set_streaming_settings(
                      JceScene *scene,
                      const JceSceneStreamingSettings *settings);
JCE_API const JceSceneStreamingSettings *
                  jce_scene_get_streaming_settings(const JceScene *scene);
JCE_API JceSceneStreamingSettings *
                  jce_scene_get_streaming_settings_mut(JceScene *scene);
JCE_API void      jce_scene_clear_streaming_settings(JceScene *scene);

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

/* Is `e` still the entity it was when the handle was obtained?
 *
 * A JceEntity carries an index in its low 32 bits and a GENERATION in its
 * high 32.  Destroying an entity frees its index for reuse and bumps the
 * generation, so a handle kept across a destroy refers to a slot that now
 * holds somebody else.  This is the predicate that tells them apart, and it
 * is the only safe thing to call on a handle of unknown age: every other
 * entity entry point treats a stale handle as "no such entity", which is
 * correct but silent.
 *
 * Returns false for a NULL scene and for entity 0. */
JCE_API bool      JCE_CALL jce_scene_entity_alive(const JceScene *s, JceEntity e);

/* Resolve a BARE INDEX (no generation) to whatever entity currently occupies
 * that slot.
 *
 * This deliberately has NO dangling protection — that is the entire content
 * of an index without a generation, and no implementation can recover it.
 * If the original entity was destroyed and its slot reused, this returns the
 * NEW occupant, happily and with no error.
 *
 * It exists because the editor stores 32-bit ids (audit C5-02) and needs a
 * documented way back.  New code must NOT use it: keep the full JceEntity.
 * Returns 0 when no live entity occupies the index. */
JCE_API JceEntity JCE_CALL jce_scene_entity_from_index(const JceScene *s,
                                                       uint32_t index);
JCE_API const char *jce_scene_entity_name(const JceScene *s, JceEntity e);
JCE_API const char *jce_scene_entity_registered_name(const JceScene *s, JceEntity e);
JCE_API void      jce_scene_set_entity_name(JceScene *s, JceEntity e, const char *name);

/* Parent / child hierarchy. */
/* Compatibility setter: keeps the local transform and silently rejects dead
 * entities, self-parenting, and hierarchy cycles. Use jce_scene_reparent when
 * the caller needs keep-world behavior or an explicit success result. */
JCE_API void      jce_scene_set_parent(JceScene *s, JceEntity child, JceEntity parent);
/* Change hierarchy ownership with validation. When preserve_world is true,
 * the rendered model matrix remains unchanged, including an enabled custom
 * pivot. Passing JCE_ENTITY_INVALID makes child a root entity. Returns false
 * for dead entities, self-parenting, or a hierarchy cycle. */
JCE_API bool      jce_scene_reparent(JceScene *s, JceEntity child,
                                     JceEntity parent, bool preserve_world);
JCE_API JceEntity jce_scene_get_parent(const JceScene *s, JceEntity e);
JCE_API int       jce_scene_get_children(const JceScene *s, JceEntity parent,
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

/* Per-frame world-matrix memo drop WITHOUT marking a structural edit.  Same
 * intra-frame effect as jce_scene_invalidate_world_cache (drops the memoized
 * matrices so a re-render reads transforms as they are now), but does NOT bump
 * the structural epoch — so a host that renders WITHOUT jce_scene_update (the
 * editor scene renderer + pick pass) can begin each render frame here and the
 * renderer's cross-frame persistent static world-matrix/AABB cache survives a
 * frame in which nothing was actually edited.  Use jce_scene_invalidate_world_
 * cache (not this) after a real transform/parent/component edit. */
JCE_API void      jce_scene_begin_render_world_cache(JceScene *s);

/* Monotonic counter bumped only by jce_scene_invalidate_world_cache (i.e. by a
 * real structural edit), never by the per-frame drops.  The scene renderer keys
 * its cross-frame persistent static world-matrix + AABB cache on this. */
JCE_API uint64_t  jce_scene_get_structural_epoch(const JceScene *s);

/* Per-entity transform generation (dynamic-scene world-cache, large-world opt).
 * set_transform / set_pivot bump ONLY the moved entity's subtree's gen instead
 * of the global structural epoch, so a moving camera / script-animated entity no
 * longer drops the renderer's static world-cache for the whole scene.  The
 * renderer keys a static entity's cache validity on BOTH the structural epoch
 * (global edits) AND this per-entity gen (transform edits).  jce_scene_xgen_active
 * is false until the first per-entity bump, so a never-moved scene stays on the
 * cheap epoch-only fast path. */
JCE_API uint64_t  jce_scene_entity_xform_gen(const JceScene *s, JceEntity e);
JCE_API bool      jce_scene_xgen_active(const JceScene *s);
JCE_API void      jce_scene_invalidate_entity_world(JceScene *s, JceEntity e);

/* Frame-invariance generation counters (DOTS-floor slice 1).  Together with
 * structural_epoch they form the frozen-frame key a cross-frame consumer can
 * compare to prove "nothing observable changed since last frame":
 *  - roster_epoch:  entity create/destroy (flecs swap-remove silently reorders
 *    table rows, so ANY roster change invalidates positional index caches).
 *  - enable_gen:    any EditorMeta.enabled flip (routed through the editor's
 *    single jce_state_set_entity_enabled entry; bumped via
 *    jce_scene_bump_enable_gen because the flag is mutated in place).
 *  - xform_counter: bumped by every jce_scene_invalidate_entity_world call
 *    (set_transform/set_pivot subtree bumps) AND by the runtime physics
 *    write-back (jce_scene_notify_physics_writeback) which otherwise mutates
 *    Transforms in place with no signal — one integer compare answers "did
 *    ANY world matrix change" without probing 150k per-entity gens. */
JCE_API uint64_t  jce_scene_get_roster_epoch(const JceScene *s);
JCE_API uint64_t  jce_scene_get_enable_gen(const JceScene *s);
JCE_API uint64_t  jce_scene_get_xform_counter(const JceScene *s);
JCE_API void      jce_scene_bump_enable_gen(JceScene *s);
JCE_API void      jce_scene_notify_physics_writeback(JceScene *s);
/* Per-entity variant: the writer KNOWS which entity it mutated in place —
 * ring-push it (plus descendants) so the renderer's L2 incremental repair
 * rebuilds just those slots instead of the whole frame.  Falls back to
 * dirty-overflow (== the blanket notify) when the ring/subtree bounds hit. */
JCE_API void      jce_scene_notify_physics_writeback_entity(JceScene *s, JceEntity e);

/* Drain the dirty-entity ring (DOTS-floor L2): every entity whose world was
 * invalidated since the last take (subtree-expanded).  out may be NULL to
 * discard.  *out_overflow is set when the ring overflowed OR an in-place
 * mutation with an unknown entity set occurred (physics write-back) — the
 * consumer must then treat the whole scene as dirty. */
JCE_API uint32_t  jce_scene_take_dirty_entities(JceScene *s, JceEntity *out,
                                                uint32_t cap, bool *out_overflow);
/* Read-only variant (does not clear) — for consumers earlier in the frame
 * than the taker (the collect membership check runs before the ecull
 * repair, which owns the take). */
JCE_API uint32_t  jce_scene_peek_dirty_entities(const JceScene *s, JceEntity *out,
                                                uint32_t cap, bool *out_overflow);

/* Per-entity MATERIAL generation (lever ③ persistent draw-cmd cache). Mirrors
 * xform_gen for material/mesh content: bumped by the MeshRenderer set path and
 * async texture/model pop-in via jce_scene_invalidate_entity_material. A cache
 * stores the mat_gen it saw and rebuilds the entity's draw command when it
 * differs. Shares the xgen slot table (jce_scene_xgen_active gates both). */
JCE_API uint64_t  jce_scene_entity_material_gen(const JceScene *s, JceEntity e);
JCE_API void      jce_scene_invalidate_entity_material(JceScene *s, JceEntity e);

/* Floating-origin rebase: add `shift` (metres, float[3]) to the LOCAL position
 * of every ROOT entity (one with no parent) that carries a JceTransform, then
 * invalidate the world-matrix cache once.  Children are parent-relative and
 * MUST NOT be shifted — they follow automatically through the hierarchy, which
 * preserves all relative geometry (a parent and child shift together by exactly
 * `shift`).  This is the scene-side primitive the runtime's floating-origin
 * rebase drives (see jce_world_origin.h); it is pure transform mutation and can
 * be exercised headlessly.  NULL scene or an all-zero shift is a no-op (it does
 * not even bump the world epoch, so it cannot churn the cache). */
JCE_API void      jce_scene_apply_world_shift(JceScene *s, const float shift[3]);

/* Component access — Transform. */
JCE_API void           jce_scene_set_transform(JceScene *s, JceEntity e, const JceTransform *t);
JCE_API JceTransform  *jce_scene_get_transform(JceScene *s, JceEntity e);
JCE_API bool           jce_scene_has_transform(const JceScene *s, JceEntity e);
JCE_API void           jce_scene_remove_transform(JceScene *s, JceEntity e);

/* Component access — Pivot. */
JCE_API void               jce_scene_set_pivot(JceScene *s, JceEntity e, const JcePivotComponent *p);
JCE_API JcePivotComponent *jce_scene_get_pivot(JceScene *s, JceEntity e);
JCE_API bool               jce_scene_has_pivot(const JceScene *s, JceEntity e);
JCE_API void               jce_scene_remove_pivot(JceScene *s, JceEntity e);

/* Maya-style pivot editing helpers. These move only the pivot while keeping
 * the rendered model matrix unchanged, so geometry does not jump when the
 * editor enters pivot-edit mode and the user drags the handle. */
JCE_API jce_vec3 jce_scene_get_pivot_world_position(const JceScene *s, JceEntity e);
JCE_API void     jce_scene_set_pivot_world_position_preserve_model(
    JceScene *s, JceEntity e, jce_vec3 world_position);
JCE_API void     jce_scene_set_pivot_local_position_preserve_model(
    JceScene *s, JceEntity e, jce_vec3 local_position);

/* Component access — MeshRenderer. */
JCE_API void               jce_scene_set_mesh_renderer(JceScene *s, JceEntity e, const JceMeshRenderer *mr);
JCE_API JceMeshRenderer   *jce_scene_get_mesh_renderer(JceScene *s, JceEntity e);
JCE_API bool               jce_scene_has_mesh_renderer(const JceScene *s, JceEntity e);
JCE_API void               jce_scene_remove_mesh_renderer(JceScene *s, JceEntity e);
/* Read-only accessor (ecs_get_id) for concurrent worker-thread reads, valid
 * under jce_scene_parallel_read_begin/end (multi-threaded flecs readonly mode). */
JCE_API const JceMeshRenderer *jce_scene_get_mesh_renderer_const(const JceScene *s, JceEntity e);
JCE_API void               jce_scene_parallel_read_begin(JceScene *s);
JCE_API void               jce_scene_parallel_read_end(JceScene *s);

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

/* O(1) scene-wide component counts (flecs table counts) — lets per-frame loops
 * that probe every collected entity for a rare component skip entirely when
 * the scene holds none. */
JCE_API int                              jce_scene_count_skeletal_animators(JceScene *s);
JCE_API int                              jce_scene_count_sprite_animators(JceScene *s);
JCE_API int                              jce_scene_count_point_lights(JceScene *s);
JCE_API int                              jce_scene_count_spot_lights(JceScene *s);
JCE_API int                              jce_scene_count_dir_lights(JceScene *s);
JCE_API int                              jce_scene_count_reflection_probes(JceScene *s);
JCE_API int                              jce_scene_count_light_probe_groups(JceScene *s);
JCE_API int                              jce_scene_count_lod_groups(JceScene *s);


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

/* Component access — MusicTrack (adaptive music director shim). */
JCE_API void                          jce_scene_set_music_track(JceScene *s, JceEntity e, const JceMusicTrackComponent *c);
JCE_API JceMusicTrackComponent       *jce_scene_get_music_track(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_music_track(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_music_track(JceScene *s, JceEntity e);

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

/* Per-entity particle control (gameplay scripting last-mile).
 *
 * Drive the live emitter backing entity `e`'s JceParticleEmitterComponent
 * WITHOUT exposing the renderer particle types: the scene maps the entity to
 * its emitter handle inside the scene-owned JceParticleSystem and forwards to
 * jce_particles_emitter_{burst,start,stop}.  These are the seam scripts reach
 * through jce.particle_burst / jce.particle_set_emitting.
 *
 * Tolerant by design (all return void, no crash):
 *   - No-op when `s` is NULL, the entity has no JceParticleEmitterComponent, the
 *     emitter has not been built yet (the scene builds it on the next
 *     jce_scene_particles_update tick), or the component is GPU-routed (the
 *     CPU emitter handle is absent in that case).
 *
 * jce_scene_particle_burst : fire a one-shot burst of `count` particles (count
 *                            <= 0 is a no-op).
 * jce_scene_particle_set_emitting : start (`on`) continuous emission, or stop
 *                            it (!on) — stopping lets live particles age out. */
JCE_API void jce_scene_particle_burst(JceScene *s, JceEntity e, int count);
JCE_API void jce_scene_particle_set_emitting(JceScene *s, JceEntity e, bool on);

/* jce_scene_particle_set_color : retint the entity emitter's newly-spawned
 * particles (RGB of both color keyframes; alphas/fade preserved). */
JCE_API void jce_scene_particle_set_color(JceScene *s, JceEntity e,
                                          float r, float g, float b);

/* Anchor for resolving component-relative `*.particles.json` paths (the
 * component stores project-relative paths; reads are otherwise CWD-relative).
 * The editor points this at the project's source_assets dir; default_main at
 * <exe>/cooked_assets. NULL/empty clears. Process-global. */
JCE_API void jce_scene_particles_set_asset_root(const char *root);

/* Embedded PAK for the single-exe path: publishes the overlaid game PAK so
 * component-relative `*.particles.json` descriptors load straight from the PAK
 * (the host-fs anchor above misses when no loose cooked tree ships beside the
 * exe).  default_main sets this to the engine PAK; NULL clears. Process-global. */
JCE_API void jce_scene_particles_set_pak(const struct JcePakArchive *pak);

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

/* Component access — Vegetation Scatter (foliage / grass / trees). */
JCE_API void                          jce_scene_set_vegetation_scatter(JceScene *s, JceEntity e, const JceVegetationScatterComponent *c);
JCE_API JceVegetationScatterComponent *jce_scene_get_vegetation_scatter(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_vegetation_scatter(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_vegetation_scatter(JceScene *s, JceEntity e);

/* Component access — Grass Field (GPU-instanced procedural blades + wind). */

/* ── Foliage Cluster (stylized billboard canopy / bush) ────────────────
 * The hand-painted-diorama foliage model: `leaf_count` camera-facing
 * billboard cards sampled on a (squashed) sphere shell of `radius` around
 * the entity, each carrying its sample-point OUTWARD normal.  The dedicated
 * shader shades every card with a 3-tone toon ramp (shadow/mid/highlight,
 * scaled by color_multiplier) keyed on that normal vs the primary light —
 * which is what reads as a shaded leafy BALL instead of flat cards.
 * Unlit otherwise (the authored colors ARE the lighting per look). */
typedef struct {
    int      leaf_count;          /* billboard cards on the shell (<=256)   */
    float    radius;              /* cluster shell radius (world units)     */
    float    squash_y;            /* vertical squash of the shell (1=sphere)*/
    float    leaf_scale;          /* base card size; each card jitters up   */
    uint32_t seed;                /* deterministic sampling                 */
    float    shadow_color[3];     /* toon ramp: shadow -> mid -> highlight  */
    float    mid_color[3];
    float    highlight_color[3];
    float    color_multiplier[3]; /* final multiply (season/tod grade)      */
    char     alpha_tex[256];      /* leaf alpha mask texture                */
    bool     visible;
} JceFoliageClusterComponent;

/* Component access — FoliageCluster. */
JCE_API void                        jce_scene_set_foliage_cluster(JceScene *s, JceEntity e, const JceFoliageClusterComponent *c);
JCE_API JceFoliageClusterComponent *jce_scene_get_foliage_cluster(JceScene *s, JceEntity e);
JCE_API bool                        jce_scene_has_foliage_cluster(const JceScene *s, JceEntity e);
JCE_API void                        jce_scene_remove_foliage_cluster(JceScene *s, JceEntity e);

JCE_API void                    jce_scene_set_grass_field(JceScene *s, JceEntity e, const JceGrassFieldComponent *c);
JCE_API JceGrassFieldComponent *jce_scene_get_grass_field(JceScene *s, JceEntity e);
JCE_API bool                    jce_scene_has_grass_field(const JceScene *s, JceEntity e);
JCE_API void                    jce_scene_remove_grass_field(JceScene *s, JceEntity e);

/* Component access — Water (Gerstner surface). */
JCE_API void                          jce_scene_set_water(JceScene *s, JceEntity e, const JceWaterComponent *c);
JCE_API JceWaterComponent            *jce_scene_get_water(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_water(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_water(JceScene *s, JceEntity e);

/* Component access — Buoyancy (floats a dynamic body on the water surface). */
JCE_API void                          jce_scene_set_buoyancy(JceScene *s, JceEntity e, const JceBuoyancyComponent *c);
JCE_API JceBuoyancyComponent         *jce_scene_get_buoyancy(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_buoyancy(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_buoyancy(JceScene *s, JceEntity e);

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

/* Component access — UI Slider.  get returns a MUTABLE pointer: the canvas
 * writes the dragged value back into `value`. */
JCE_API void                          jce_scene_set_ui_slider(JceScene *s, JceEntity e, const JceUISliderComponent *c);
JCE_API JceUISliderComponent         *jce_scene_get_ui_slider(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_slider(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_slider(JceScene *s, JceEntity e);

/* Component access — UI Toggle.  get returns a MUTABLE pointer: the canvas
 * flips `is_on` on click. */
JCE_API void                          jce_scene_set_ui_toggle(JceScene *s, JceEntity e, const JceUIToggleComponent *c);
JCE_API JceUIToggleComponent         *jce_scene_get_ui_toggle(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_toggle(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_toggle(JceScene *s, JceEntity e);

/* Component access — UI InputField.  get returns a MUTABLE pointer: the
 * canvas writes edits (insert / delete / caret) back into `text`. */
JCE_API void                          jce_scene_set_ui_input_field(JceScene *s, JceEntity e, const JceUIInputFieldComponent *c);
JCE_API JceUIInputFieldComponent     *jce_scene_get_ui_input_field(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_input_field(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_input_field(JceScene *s, JceEntity e);

/* Component access — UI ScrollView.  get returns a MUTABLE pointer: the
 * canvas writes the scrolled offset back into `scroll_position`. */
JCE_API void                          jce_scene_set_ui_scroll_view(JceScene *s, JceEntity e, const JceUIScrollViewComponent *c);
JCE_API JceUIScrollViewComponent     *jce_scene_get_ui_scroll_view(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_scroll_view(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_scroll_view(JceScene *s, JceEntity e);

/* Component access — UI ProgressBar.  Read-only at runtime: the canvas only
 * draws it, never mutates it (gameplay/script writes `value`). */
JCE_API void                          jce_scene_set_ui_progress_bar(JceScene *s, JceEntity e, const JceUIProgressBarComponent *c);
JCE_API JceUIProgressBarComponent    *jce_scene_get_ui_progress_bar(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_progress_bar(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_progress_bar(JceScene *s, JceEntity e);

/* Component access — UI Dropdown.  get returns a MUTABLE pointer: the canvas
 * writes `selected_index` and `expanded` back on click. */
JCE_API void                          jce_scene_set_ui_dropdown(JceScene *s, JceEntity e, const JceUIDropdownComponent *c);
JCE_API JceUIDropdownComponent       *jce_scene_get_ui_dropdown(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ui_dropdown(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ui_dropdown(JceScene *s, JceEntity e);

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

/* RETIRED component (see JceVfxGraphComponent): accessors kept for ABI only. */
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

/* ── Nav-mesh agent (presence-gated, no flag bit) ────────────────── */
JCE_API void                          jce_scene_set_nav_agent(JceScene *s, JceEntity e, const JceNavAgentComponent *c);
JCE_API JceNavAgentComponent         *jce_scene_get_nav_agent(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_nav_agent(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_nav_agent(JceScene *s, JceEntity e);

/* ── Animation-rigging IK constraints (presence-gated, no flag bit) ──
 * No JCE_COMP_FLAG bit — the 64-bit flag space is full; presence-gated
 * like VideoPlayer/NavAgent. */
JCE_API void                          jce_scene_set_ik_constraints(JceScene *s, JceEntity e, const JceIkConstraintComponent *c);
JCE_API JceIkConstraintComponent     *jce_scene_get_ik_constraints(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ik_constraints(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ik_constraints(JceScene *s, JceEntity e);

/* ── Foot IK (presence-gated, no flag bit) ───────────────────────────
 * No JCE_COMP_FLAG bit — the 64-bit flag space is full; presence-gated
 * like IkConstraints/NavAgent. */
JCE_API void                          jce_scene_set_foot_ik(JceScene *s, JceEntity e, const JceFootIkComponent *c);
JCE_API JceFootIkComponent           *jce_scene_get_foot_ik(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_foot_ik(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_foot_ik(JceScene *s, JceEntity e);
JCE_API void                          jce_scene_set_full_body_ik(JceScene *s, JceEntity e, const JceFullBodyIkComponent *c);
JCE_API JceFullBodyIkComponent       *jce_scene_get_full_body_ik(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_full_body_ik(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_full_body_ik(JceScene *s, JceEntity e);

/* ── Sequence player (presence-gated, no flag bit) ───────────────────
 * No JCE_COMP_FLAG bit — the 64-bit flag space is full; presence-gated
 * like VideoPlayer/NavAgent/IkConstraints. */
JCE_API void                          jce_scene_set_sequence_player(JceScene *s, JceEntity e, const JceSequencePlayerComponent *c);
JCE_API JceSequencePlayerComponent   *jce_scene_get_sequence_player(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_sequence_player(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_sequence_player(JceScene *s, JceEntity e);

/* ── Morph weights (presence-gated, no flag bit) ─────────────────────
 * Per-instance static blendshape weights; presence-gated like
 * VideoPlayer/NavAgent/SequencePlayer (the 64-bit flag space is full). */
JCE_API void                          jce_scene_set_morph_weights(JceScene *s, JceEntity e, const JceMorphWeightsComponent *c);
JCE_API JceMorphWeightsComponent     *jce_scene_get_morph_weights(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_morph_weights(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_morph_weights(JceScene *s, JceEntity e);

/* ── Network variable authoring (FEATURE 7.2, presence-gated, no flag) ──
 * Designer-authored mark that an entity carries a replicated typed
 * variable; the runtime net bridge registers the matching typed NetVar at
 * Play.  Presence-gated like MorphWeights (the 64-bit flag space is full). */
JCE_API void                          jce_scene_set_network_variable(JceScene *s, JceEntity e, const JceNetworkVariableComponent *c);
JCE_API JceNetworkVariableComponent  *jce_scene_get_network_variable(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_network_variable(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_network_variable(JceScene *s, JceEntity e);

/* ── Gameplay Ability System authoring (presence-gated, no flag) ──────
 * Designer-authored attribute + ability tables; the runtime inits a live
 * JceGameplayAbilitySystem from them at Play (rt_spawn_gameplay) and exposes
 * it to scripts via jce_runtime_entity_gas.  Presence-gated like
 * MorphWeights/NetworkVariable (the 64-bit flag space is full). */
JCE_API void                               jce_scene_set_gas(JceScene *s, JceEntity e, const JceGameplayAbilitySystemComponent *c);
JCE_API JceGameplayAbilitySystemComponent *jce_scene_get_gas(JceScene *s, JceEntity e);
JCE_API bool                               jce_scene_has_gas(const JceScene *s, JceEntity e);
JCE_API void                               jce_scene_remove_gas(JceScene *s, JceEntity e);

/* ── Ragdoll authoring (presence-gated, no flag) ─────────────────────
 * Designer-authored ragdoll mark + tuning; the runtime builds a live
 * JceRagdoll from it at Play (rt_spawn_gameplay) and publishes its pose
 * through the relay below.  Presence-gated like MorphWeights/GAS (the 64-bit
 * flag space is full). */
JCE_API void                          jce_scene_set_ragdoll(JceScene *s, JceEntity e, const JceRagdollComponent *c);
JCE_API JceRagdollComponent          *jce_scene_get_ragdoll(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_ragdoll(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_ragdoll(JceScene *s, JceEntity e);

/* ── Fracture / destruction authoring (presence-gated, no flag) ──────
 * Designer-authored "this entity shatters" mark + tuning; the runtime swaps
 * the intact body for Voronoi fragment bodies on break (jce_runtime_fracture_
 * entity).  Default disabled -> the runtime fracture path is never entered.
 * Presence-gated like Ragdoll/GAS/MorphWeights (the 64-bit flag space is full). */
JCE_API void                          jce_scene_set_fracture(JceScene *s, JceEntity e, const JceFractureComponent *c);
JCE_API JceFractureComponent         *jce_scene_get_fracture(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_fracture(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_fracture(JceScene *s, JceEntity e);

/* ── Vehicle chassis authoring (presence-gated, no flag) ─────────────
 * Designer-authored "this entity is a raycast-vehicle chassis" mark + tuning;
 * the runtime (rt_try_spawn_vehicle) builds the chassis body + wheels via the
 * engine vehicle API.  Default disabled -> the entity spawns as a normal rigid
 * body.  Presence-gated like Ragdoll/Fracture/GAS (the 64-bit flag space is full). */
JCE_API void                          jce_scene_set_vehicle(JceScene *s, JceEntity e, const JceVehicleComponent *c);
JCE_API JceVehicleComponent          *jce_scene_get_vehicle(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_vehicle(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_vehicle(JceScene *s, JceEntity e);

/* ── Volumetric / pressure soft-body authoring (presence-gated, no flag) ──
 * Designer-authored "this entity is a squishy pressure soft body" mark + tuning;
 * the runtime (rt_try_spawn_softbody) builds the body in the shared soft world.
 * Default disabled -> the entity spawns as a normal rigid body.  Presence-gated
 * like Vehicle/Fracture/Ragdoll (the 64-bit flag space is full). */
JCE_API void                          jce_scene_set_soft_body(JceScene *s, JceEntity e, const JceSoftBodyComponent *c);
JCE_API JceSoftBodyComponent         *jce_scene_get_soft_body(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_soft_body(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_soft_body(JceScene *s, JceEntity e);

/* ── Simulation LOD authoring (presence-gated, no flag) ──────────────
 * Designer-authored distance tiers (near/mid radius + per-tier Hz); the
 * runtime (rt_tick_gameplay) gates this entity's script/nav/AI updates to the
 * active tier's rate, folding the accumulated dt so logic stays time-correct.
 * Presence-gated like SoftBody/Vehicle/Ragdoll (the 64-bit flag space is full). */
JCE_API void                          jce_scene_set_sim_lod(JceScene *s, JceEntity e, const JceSimLodComponent *c);
JCE_API JceSimLodComponent           *jce_scene_get_sim_lod(JceScene *s, JceEntity e);
JCE_API bool                          jce_scene_has_sim_lod(const JceScene *s, JceEntity e);
JCE_API void                          jce_scene_remove_sim_lod(JceScene *s, JceEntity e);

/* ── Ragdoll pose relay (TRANSIENT, NOT serialized) ──────────────────
 * The runtime publishes the ragdoll's resolved per-bone LOCAL pose here each
 * physics step; the scene renderer reads it and evaluates it into the skin
 * palette.  Carries only jce_mat4 (public math), so it crosses no layer
 * boundary in either direction.  Excluded from scene save/load. */
JCE_API void jce_scene_set_ragdoll_pose(JceScene *s, JceEntity e,
                                        const jce_mat4 *locals, uint32_t count);
/* Copies the relay pose into out_locals (which must hold >= JCE_MAX_BONES) and
 * writes the joint count to *out_count.  Returns false if the entity has no
 * relay.  out_count may be NULL. */
JCE_API bool jce_scene_get_ragdoll_pose(const JceScene *s, JceEntity e,
                                        jce_mat4 *out_locals, uint32_t *out_count);
JCE_API bool jce_scene_has_ragdoll_pose(const JceScene *s, JceEntity e);

/* ── Animation state-machine command relay (TRANSIENT, NOT serialized) ─
 * Scripts drive the animation SM via jce.anim_set_float/bool/int/trigger; the
 * runtime pushes those commands here and the scene renderer drains them each
 * frame into the entity's SrAnimInstance->sm_binding.  This decouples the
 * runtime (which has no renderer handle) from the renderer (which owns the
 * anim instance) — the same relay pattern as the ragdoll pose.  Excluded from
 * scene save/load. */
typedef enum {
    JCE_ANIM_PARAM_FLOAT   = 0,
    JCE_ANIM_PARAM_INT     = 1,
    JCE_ANIM_PARAM_BOOL    = 2,
    JCE_ANIM_PARAM_TRIGGER = 3,   /* one-shot; `value` ignored */
} JceAnimParamType;
typedef struct {
    int   type;        /* JceAnimParamType */
    char  name[48];    /* SM parameter name */
    float value;       /* float; int via cast; bool != 0; ignored for trigger */
} JceAnimParamCmd;
#define JCE_ANIM_CMD_RELAY_MAX 16
typedef struct {
    JceAnimParamCmd cmds[JCE_ANIM_CMD_RELAY_MAX];
    uint32_t        count;
} JceAnimCmdRelay;
/* Queue one SM-parameter command for `e` (appended; silently dropped past the
 * per-frame cap). */
JCE_API void     jce_scene_anim_push_param(JceScene *s, JceEntity e,
                                           const JceAnimParamCmd *cmd);
/* Copy out + CLEAR the entity's pending commands (renderer drains each frame).
 * Returns the count written (<= max). */
JCE_API uint32_t jce_scene_anim_take_params(JceScene *s, JceEntity e,
                                            JceAnimParamCmd *out, uint32_t max);

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

/* Component-filtered entity walks (flecs each, O(#matches)) — visit only the
 * entities that HOLD the component instead of probing a full entity list. */
JCE_API void jce_scene_each_point_light(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_spot_light(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_dir_light(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_reflection_probe(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_light_probe_group(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_water(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_camera(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_particle_emitter(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_skeletal_animator(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_skybox(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_video_player(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_volume(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_decal(JceScene *s, JceEntityCallback cb, void *user_data);
JCE_API void jce_scene_each_cloth(JceScene *s, JceEntityCallback cb, void *user_data);

/* O(1) count of entities holding a ConstantForce component (flecs table
 * aggregate) — the per-tick pass bails on 0 without a per-body pre-walk. */
JCE_API int  jce_scene_count_constant_force(JceScene *s);

/* O(1) holder counts (flecs table aggregates) for the per-frame system
 * gates: a scene with none of the component pays a counter read instead of
 * a full-entity probe walk (150k probes/frame on a large static world). */
JCE_API int  jce_scene_count_particle_emitters(JceScene *s);
JCE_API int  jce_scene_count_video_players(JceScene *s);
JCE_API int  jce_scene_count_volumes(JceScene *s);
JCE_API int  jce_scene_count_decals(JceScene *s);
JCE_API int  jce_scene_count_cloth(JceScene *s);

/* Scene-graph queries (Unity Find / OverlapSphere, Godot groups).  Each writes
 * up to `max` matching entities into `out` and returns the count.  Iterate
 * transform-bearing entities.  query_sphere tests transform-origin distance. */
JCE_API int jce_scene_query_by_tag(JceScene *s, const char *tag,
                                   JceEntity *out, int max);
JCE_API int jce_scene_query_by_name(JceScene *s, const char *name,
                                    JceEntity *out, int max);
JCE_API int jce_scene_query_sphere(JceScene *s, jce_vec3 center, float radius,
                                   JceEntity *out, int max);

/* Get the flecs world (for advanced queries). */
JCE_API void *jce_scene_get_world(JceScene *s);

/* Progress the scene (runs flecs systems). */
JCE_API void jce_scene_update(JceScene *s, float dt);

JCE_EXTERN_C_END

#endif /* JCE_SCENE_H */
