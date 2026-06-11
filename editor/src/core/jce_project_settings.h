/*
 * jce_project_settings.h  Project-scoped settings persisted to
 * `.jce/project-settings.json`.
 *
 * Distinct from `jce_editor_config.{h,cpp}` (per-user editor preferences:
 * language, fonts, recent projects, build presets). Project settings
 * mirror Unity's ProjectSettings/* assets — they travel with the project
 * and define runtime behaviour: physics gravity, fixed timestep, audio
 * master mix, tags & layers, quality levels, etc.
 *
 * Editor pages live in `dialogs/jce_dialog_project_settings.cpp`; runtime
 * subsystems consume the loaded snapshot through the public getters
 * declared at the bottom of this header.
 */

#ifndef JCE_PROJECT_SETTINGS_H
#define JCE_PROJECT_SETTINGS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_PS_MAX_TAGS              64
#define JCE_PS_MAX_SORTING_LAYERS    32
#define JCE_PS_LAYER_COUNT           32       /* fixed, Unity-compatible */
#define JCE_PS_MAX_QUALITY_LEVELS    8
#define JCE_PS_MAX_PRESET_BINDINGS   64
#define JCE_PS_NAME_LEN              64
#define JCE_PS_PATH_LEN              512

/* ── Audio ─────────────────────────────────────────────────────────── */
typedef struct {
    float master_volume;          /* 0..1 */
    float doppler_factor;         /* default 1.0 */
    int   sample_rate;            /* 44100 / 48000 */
    bool  pause_on_focus_loss;
    bool  disable_audio;
} JceProjectAudio;

/* ── Editor ────────────────────────────────────────────────────────── */
typedef struct {
    bool  auto_save_enabled;
    int   auto_save_interval_sec;
    int   default_behavior_mode;  /* 0 = 3D, 1 = 2D */
    int   version_control_mode;   /* 0 = Hidden Meta, 1 = Visible Meta */
    char  external_script_editor[JCE_PS_PATH_LEN];
    char  external_image_editor [JCE_PS_PATH_LEN];
} JceProjectEditor;

/* ── Graphics ──────────────────────────────────────────────────────── */
typedef struct {
    int   color_space;            /* 0 = Gamma, 1 = Linear */
    bool  hdr;
    bool  srgb_write;
    int   default_msaa;           /* 0 / 2 / 4 / 8 */
    int   anisotropic_textures;   /* 0 = Disabled, 1 = PerTexture, 2 = ForcedOn */
    char  always_included_shaders[1024]; /* newline-delimited paths */
} JceProjectGraphics;

/* ── Input ─────────────────────────────────────────────────────────── */
typedef struct {
    bool  treat_keyboard_as_dpad;
    float dead_zone;              /* 0..1 */
    float gravity;                /* analog snap-back rate */
    float sensitivity;
    bool  enable_gamepad;
} JceProjectInput;

/* ── Physics 3D ────────────────────────────────────────────────────── */
typedef struct {
    float gravity[3];             /* default (0, -9.81, 0) */
    float default_contact_offset;
    int   default_solver_iterations;
    int   default_solver_velocity_iterations;
    float bounce_threshold;
    float sleep_threshold;
    bool  queries_hit_triggers;
    bool  queries_hit_backfaces;
    bool  auto_simulation;
    /* Layer collision matrix: bit (i,j) -> layer i collides with layer j.
     * Stored as 32 rows of uint32_t bit masks (symmetric kept by UI). */
    uint32_t layer_collision_matrix[JCE_PS_LAYER_COUNT];
} JceProjectPhysics;

/* ── Physics 2D ────────────────────────────────────────────────────── */
typedef struct {
    float gravity[2];             /* default (0, -9.81) */
    int   velocity_iterations;
    int   position_iterations;
    bool  queries_hit_triggers;
    bool  auto_sync_transforms;
    bool  auto_simulation;
    uint32_t layer_collision_matrix[JCE_PS_LAYER_COUNT];
} JceProjectPhysics2D;

/* ── Player ────────────────────────────────────────────────────────── */
typedef struct {
    char  company_name[JCE_PS_NAME_LEN];
    char  product_name[JCE_PS_NAME_LEN];
    char  version[32];
    char  default_icon_path [JCE_PS_PATH_LEN];
    char  default_cursor_path[JCE_PS_PATH_LEN];
    float splash_bg_color[3];
    bool  show_splash;
    bool  run_in_background;
    bool  fullscreen_default;
    int   default_screen_width;
    int   default_screen_height;
} JceProjectPlayer;

/* ── Preset Manager ────────────────────────────────────────────────── */
typedef struct {
    char component_type[JCE_PS_NAME_LEN];
    char preset_path   [JCE_PS_PATH_LEN];
    char filter        [JCE_PS_NAME_LEN];     /* optional name regex */
} JceProjectPresetBinding;

typedef struct {
    int                         count;
    JceProjectPresetBinding     bindings[JCE_PS_MAX_PRESET_BINDINGS];
} JceProjectPresetManager;

/* ── Quality ───────────────────────────────────────────────────────── */
typedef struct {
    char  name[JCE_PS_NAME_LEN];
    int   pixel_light_count;
    int   texture_quality;        /* 0 = Full, 1 = Half, 2 = Quarter, 3 = Eighth */
    int   anisotropic;            /* same enum as Graphics */
    int   anti_aliasing;          /* 0/2/4/8 */
    bool  soft_particles;
    bool  realtime_reflection_probes;
    int   shadow_quality;         /* 0 = Disable, 1 = Hard, 2 = HardSoft */
    int   shadow_resolution;      /* 0..3 (Low/Med/High/VeryHigh) */
    float shadow_distance;
    int   shadow_cascades;        /* 1 / 2 / 4 */
    int   vsync_count;            /* 0 = Off, 1 = Every VBlank, 2 = Every 2nd */
    int   target_framerate;       /* -1 = uncapped */
    float lod_bias;
} JceProjectQualityLevel;

typedef struct {
    int                       count;
    int                       current_level;     /* index into levels */
    JceProjectQualityLevel    levels[JCE_PS_MAX_QUALITY_LEVELS];
} JceProjectQuality;

/* ── Tags & Layers ─────────────────────────────────────────────────── */
typedef struct {
    int   tag_count;
    char  tags         [JCE_PS_MAX_TAGS][JCE_PS_NAME_LEN];
    int   sorting_layer_count;
    char  sorting_layers[JCE_PS_MAX_SORTING_LAYERS][JCE_PS_NAME_LEN];
    char  layers       [JCE_PS_LAYER_COUNT][JCE_PS_NAME_LEN]; /* slot 0..7 are
                                                                 builtin and
                                                                 typically read-only */
} JceProjectTagsAndLayers;

/* ── Time ──────────────────────────────────────────────────────────── */
typedef struct {
    float fixed_timestep;         /* default 0.02 */
    float max_allowed_timestep;   /* default 0.3333 */
    float time_scale;             /* default 1.0 */
    int   maximum_particle_timestep_ms;
} JceProjectTime;

/* ── Rendering (PostFX + Fog + Ambient) ────────────────────────────── */
typedef struct {
    /* PostFX — mirrors JcePostFXParams + per-effect enabled flags */
    bool  postfx_enabled[6];      /* indexed by JcePostFXType order */
    float exposure;               /* tonemap: default 1.0 */
    float gamma;                  /* tonemap: default 2.2 */
    float bloom_threshold;        /* bloom:   default 1.0 */
    float bloom_intensity;        /* bloom:   default 0.5 */
    float fxaa_span_max;          /* fxaa:    default 8.0 */
    float vignette_intensity;     /* vignette: default 0.3 */
    float vignette_smoothness;    /* vignette: default 2.0 */
    float chromatic_strength;     /* chromatic: default 0.005 */

    /* Volumetric fog */
    bool  fog_enabled;
    float fog_color[3];           /* default: 0.7, 0.75, 0.85 */
    float fog_density;            /* default: 0.02 */
    float fog_height_falloff;     /* default: 0.05 */
    float fog_height_origin;      /* default: 0.0 */

    /* Ambient light */
    float ambient_color[3];       /* default: 0.1, 0.1, 0.12 */
    float ambient_intensity;      /* default: 1.0 */
} JceProjectRendering;

/* ── Packaging ─────────────────────────────────────────────────────── */
typedef struct {
    /* Encrypt the embedded asset PAK (and suppress loose plaintext
     * staging) for project builds.  Deters casual extraction only: the
     * key ships inside the game binary and there is no MAC.  The key
     * itself lives in <project>/.jce/pak_key.hex (git-ignored). */
    bool  encrypt_assets;
    /* Also encrypt debug-variant builds (default off so debug builds
     * keep the loose, inspectable asset tree). */
    bool  encrypt_debug_builds;
} JceProjectPackaging;

/* ── Aggregate ─────────────────────────────────────────────────────── */
typedef struct {
    JceProjectAudio          audio;
    JceProjectEditor         editor;
    JceProjectGraphics       graphics;
    JceProjectInput          input;
    JceProjectPhysics        physics;
    JceProjectPhysics2D      physics2d;
    JceProjectPlayer         player;
    JceProjectPresetManager  presets;
    JceProjectQuality        quality;
    JceProjectTagsAndLayers  tags_layers;
    JceProjectTime           time;
    JceProjectRendering      rendering;
    JceProjectPackaging      packaging;
} JceProjectSettings;

/* ── Lifecycle ─────────────────────────────────────────────────────── */

void jce_project_settings_defaults(JceProjectSettings *s);
bool jce_project_settings_load    (JceProjectSettings *out);
bool jce_project_settings_save    (const JceProjectSettings *s);

/* Writes-through to the live engine subsystems (those that have public
 * setters). UI fields without a runtime hook are persisted only.
 * Safe to call repeatedly. */
void jce_project_settings_apply   (const JceProjectSettings *s);

/* Process-wide cached snapshot — populated by load/apply. Read-only
 * accessor for runtime subsystems that want the current values without
 * hitting disk. Returns NULL until the first load. */
const JceProjectSettings *jce_project_settings_current(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PROJECT_SETTINGS_H */
