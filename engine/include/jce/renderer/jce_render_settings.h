/*
 * jce_render_settings.h — project-wide render/quality settings carrier.
 *
 * Emitted by the editor build from Project Settings > Quality into the cooked
 * tree as render_settings.json, then loaded by the standalone runtime (drop-in
 * main) so a BUILT game honors the authored quality level.  Per-scene
 * JceSceneRenderingSettings still OVERRIDE these inside the renderer; this is
 * the project-wide DEFAULT folded into JceSceneRenderConfig.
 *
 * SCOPE: the fields the runtime applies after window creation.  That used to
 * be shadow tier and lod bias only, and this paragraph still said MSAA and
 * vsync had "no runtime apply path in the drop-in main" and that vsync was
 * "an informational hint for a future window-init pass" -- both are applied
 * on every launch (jce_default_main.inc.h calls jce_renderer_set_vsync and
 * jce_renderer_set_msaa through a GPU reset, the renderer already existing at
 * that point).  HDR is still not carried: it is handled by the pipeline's HDR
 * offscreen target and tonemap, not a backbuffer flag.
 *
 * Look Profile project defaults (plan 02): carried in the nested "look" JSON
 * object (schema jce.rendersettings.v2).  Folded into a new scene's
 * JceSceneRenderingSettings by jce_editor_scene_rendering_settings_from_project.
 *
 * Layer: renderer (L5).  Pure C99.  JSON via jce_json + jce_fs_host_*.
 * Schema: jce.rendersettings.v2 (v1 = no look block; defaults-first load).
 */
#ifndef JCE_RENDER_SETTINGS_H
#define JCE_RENDER_SETTINGS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct {
    int   shadow_quality;   /* 0 = off, 1 = hard, 2 = hard+soft */
    int   shadow_map_size;  /* pixels 512/1024/2048/4096 (0 = renderer default) */
    int   shadow_cascades;  /* 1/2/4 (0 = renderer default) */
    float shadow_distance;  /* world units (0 = renderer default) */
    float lod_bias;         /* mesh-LOD distance multiplier, Unity-style:
                             * 1.0 = neutral, >1 keeps detail further out.
                             * Applied via jce_lod_set_global_bias().  NOT a
                             * texture mip bias -- it was consumed as one for
                             * a long time, which cost every shipped game a
                             * mip level at the default. */
    int   vsync;            /* 0 = off, >=1 = on (applied via the swapchain reset) */
    int   msaa;             /* 0/1 = off, else 2/4/8/16 (swapchain MSAA) */

    /* ── Look Profile project defaults (stylized slice plan 02) ─────────
     * Folded into a new scene's JceSceneRenderingSettings by the editor
     * (jce_editor_scene_rendering_settings_from_project).  Per-scene
     * settings still OVERRIDE these.  All NEUTRAL so an old project file
     * (schema v1, keys absent) loads byte-identical. */
    float wrap_factor;
    bool  ambient_hemisphere;
    float ambient_ground_color[3];
    float rim_color[3];
    float rim_power;
    float rim_intensity;
    int   tonemap_op;              /* JceSceneTonemapOp */
    char  lut_path[256];
    float lut_strength;
    bool  toon_character;
    float bloom_knee;

    /* ── Grass (GPU-instanced procedural blades, plan 07) ───────────────
     * Project-level gate: 0 = off (default, safe for all GPUs); 1 = draw
     * GrassField components.  Read by sr_draw_entities Task-6 dispatch.
     * Absent key in old files loads as 0 (defaults-first). */
    int   grass_enabled;    /* 0 = off (default); 1 = draw GrassField components */

    /* ── Texture quality (Project Settings > Quality / > Graphics) ──────
     *
     * Both of these already had working engine consumers --
     * jce_texture_set_quality_mip_bias and jce_texture_set_aniso_override --
     * and BOTH consumers had editor-only callers.  So a designer set Texture
     * Quality to Quarter to fit a low-end target, saw the viewport honour it,
     * built the game, and shipped Full: the authored value reached the
     * viewport and stopped there.  Carried here because this struct is the
     * thing the build exports and the runtime applies.
     *
     * texture_quality is the LEVEL's field (0 = Full, 1 = Half, 2 = Quarter,
     * 3 = Eighth -> a base mip drop that stacks with the streaming bias).
     * anisotropic is the project-wide Graphics field, not a per-level one:
     * <0 keeps the GPU-tier default (aniso on HIGH+ only), 0 forces it off,
     * >0 forces it on.
     *
     * Defaults are the no-op values, so a render_settings.json written before
     * these keys existed loads to exactly the behaviour it had. */
    int   texture_quality;  /* 0 = Full (default) .. 3 = Eighth */
    int   anisotropic;      /* <0 = tier default (the default here), 0 = off, >0 = on */

    /* ── Target framerate (Project Settings > Quality) ──────────────────
     *
     * Unity's Application.targetFrameRate.  The field has been authorable per
     * quality level since the Quality tab existed and the ENGINE HAD NO FRAME
     * LIMITER AT ALL -- there was nothing to hand it to, so a shipped game on
     * a menu screen ran the loop as fast as it could, thousands of frames a
     * second, burning battery and spinning fans for frames nobody sees.
     *
     * <= 0 is uncapped, which is both the default here and Unity's -1, so a
     * project that never set it is unchanged.  Applied through
     * jce_engine_set_target_fps. */
    int   target_framerate; /* <= 0 = uncapped (default) */

    /* Per-pixel light budget (Unity's QualitySettings.pixelLightCount).
     * Authored per quality level since the tab existed, with nothing to hand
     * it to until jce_lighting_set_pixel_light_count.  <= 0 = no budget
     * (default), which reproduces the old light selection exactly. */
    int   pixel_light_count;

    /* Soft particles (Unity's QualitySettings.softParticlesEnabled).
     * Authored per quality level since the tab existed, with no depth fade in
     * either particle path to hand it to.  0 = off (default), which is the
     * old behaviour exactly.  Applied through
     * jce_particles_set_soft_fade_distance.  APPEND-ONLY. */
    int   soft_particles;
} JceRenderSettings;

/* Engine defaults: shadows on (hard+soft), vsync on, everything else 0 so the
 * renderer's own JceSceneRenderConfig defaults stand in. */
JCE_API JceRenderSettings JCE_CALL jce_render_settings_default(void);

/* render_settings.json (schema jce.rendersettings.v2; v1 compat) via jce_fs_host_*.
 * Returns false on I/O or parse error; load fills `out` with defaults first so
 * a partial/old file (v1: no "look" block) still yields a valid struct. */
JCE_API bool JCE_CALL jce_render_settings_save_json(const char *vfs_path,
                                                    const JceRenderSettings *s);
JCE_API bool JCE_CALL jce_render_settings_load_json(const char *vfs_path,
                                                    JceRenderSettings *out);
/* Parse from an in-memory JSON buffer (single-exe: bytes decompressed from the
 * embedded PAK).  `len` may be 0 to strlen(json).  Fills `out` defaults-first. */
JCE_API bool JCE_CALL jce_render_settings_load_json_mem(const char *json,
                                                        size_t len,
                                                        JceRenderSettings *out);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_SETTINGS_H */
