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
 * Layer: renderer (L5).  Pure C99.  JSON via jce_json + jce_fs_host_*.
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
} JceRenderSettings;

/* Engine defaults: shadows on (hard+soft), vsync on, everything else 0 so the
 * renderer's own JceSceneRenderConfig defaults stand in. */
JCE_API JceRenderSettings JCE_CALL jce_render_settings_default(void);

/* render_settings.json (schema jce.rendersettings.v1) via jce_fs_host_*.
 * Returns false on I/O or parse error; load fills `out` with defaults first so
 * a partial/old file still yields a valid struct. */
JCE_API bool JCE_CALL jce_render_settings_save_json(const char *vfs_path,
                                                    const JceRenderSettings *s);
JCE_API bool JCE_CALL jce_render_settings_load_json(const char *vfs_path,
                                                    JceRenderSettings *out);

JCE_EXTERN_C_END

#endif /* JCE_RENDER_SETTINGS_H */
