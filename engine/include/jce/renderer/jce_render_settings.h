/*
 * jce_render_settings.h — project-wide render/quality settings carrier.
 *
 * Emitted by the editor build from Project Settings > Quality into the cooked
 * tree as render_settings.json, then loaded by the standalone runtime (drop-in
 * main) so a BUILT game honors the authored quality level.  Per-scene
 * JceSceneRenderingSettings still OVERRIDE these inside the renderer; this is
 * the project-wide DEFAULT folded into JceSceneRenderConfig.
 *
 * SCOPE: only the fields the runtime can apply post-window-creation
 * (shadow tier + lod bias) are carried here.  MSAA / HDR / sRGB / vsync are
 * window/swapchain-creation settings with no runtime apply path in the drop-in
 * main; vsync is kept as an informational hint for a future window-init pass.
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
    float lod_bias;         /* global texture mip bias hint */
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
