/*
 * jce_config.h  Engine configuration with INI file loading.
 *
 * All settings have sensible defaults. If jce.ini exists next to the
 * executable, it overrides individual values.
 */

#ifndef JCE_CONFIG_H
#define JCE_CONFIG_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_renderer_caps.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* -- Configuration ------------------------------------------------- */

typedef struct JceConfig {
    /* Window */
    int         window_width;           /* default: 640 */
    int         window_height;          /* default: 480 */
    char        window_title[128];      /* default: "JCE" */
    bool        fullscreen;             /* default: false */
    bool        maximized;              /* default: false */
    bool        resizable;              /* default: true */

    /* Renderer */
    JceRendererBackend renderer_backend;/* default: AUTO */
    bool        vsync;                  /* default: true */
    bool        debug_text;             /* default: true */
    uint32_t    clear_color;            /* default: 0x000000FF (RGBA) */

    /* Audio */
    float       master_volume;          /* default: 1.0 */
    float       music_volume;           /* default: 0.8 */
    float       sfx_volume;             /* default: 1.0 */

    /* Logging */
    int         log_level;              /* default: JCE_LOG_LEVEL_INFO (2) */
    bool        log_colors;             /* default: true */

    /* App */
    char        locale[32];             /* default: "" = auto-detect from the
                                         * host OS.  Lowercase "lang[_country]"
                                         * tag, e.g. "zh_cn".
                                         * INI: [app] locale = zh_cn */

    /* Performance / compatibility (settings S5) — layer 4 (USER CONFIG) of
     * the five-layer settings system documented in jce_render_pipeline.h.
     * BOOT-ONLY: these size bgfx pools / thread counts before the render-
     * pipeline asset is mounted, so they live in jce.ini (loaded pre-init)
     * rather than the .rp.json.  Changing them needs a restart.  User-editable
     * and consumed identically by the editor and shipped games; the
     * JCE_LOW_MEM env var still takes precedence over machine_class.
     *
     *   [performance]
     *   machine_class = auto | low | full   ; pools/encoder/trim/undo tier
     *   job_workers   = 0                    ; 0=auto(cores-1) else 1..16 */
    int         machine_class;          /* JceMachineClass: 0 auto / 1 low /
                                         * 2 full.  Overrides the RAM+core auto-
                                         * detect for bgfx transient pools,
                                         * encoder count, allocator trim cadence,
                                         * editor undo budget.
                                         * INI: [performance] machine_class = low */
    int         job_workers;            /* 0 = auto (cores-1, clamped 1..8);
                                         * >0 pins the worker-thread count.
                                         * INI: [performance] job_workers = 4 */

    /* Graphics (settings S7 follow-up: in-game graphics persistence) — the
     * knobs the in-game settings screen's graphics tab edits.  Written as an
     * ini [graphics] section on Apply and layered back onto the live render
     * pipeline at boot, right after the .rp.json / tier-preset resolution
     * (player choices are layer 4, USER CONFIG — they outrank the asset).
     * gfx_valid stays false while no [graphics] section was read, so old
     * jce.ini files (and configs never touched by the screen) are a strict
     * no-op at boot.
     *
     *   [graphics]
     *   quality        = auto | low | medium | high | ultra | custom
     *   shadows        = true                 ; CSM master toggle
     *   ssao           = true
     *   bloom          = true
     *   fog            = false                ; volumetric fog
     *   shadow_quality = 1                    ; PCF filter tier 0..2
     *   msaa           = 1                    ; 1 / 2 / 4 / 8 samples */
    bool        gfx_valid;              /* true once any graphics.* key parsed */
    int         gfx_quality;            /* -1 auto / 0 low / 1 medium / 2 high /
                                         * 3 ultra / 4 custom (keep pipeline) */
    bool        gfx_shadows;            /* default: true */
    bool        gfx_ssao;               /* default: true */
    bool        gfx_bloom;              /* default: true */
    bool        gfx_fog;                /* default: false */
    int         gfx_shadow_quality;     /* 0..2, default: 1 */
    int         gfx_msaa;               /* 1/2/4/8, default: 1 */
} JceConfig;

typedef enum JceMachineClass {
    JCE_MACHINE_CLASS_AUTO = 0,
    JCE_MACHINE_CLASS_LOW  = 1,   /* small pools / trim-aggressive (512MB) */
    JCE_MACHINE_CLASS_FULL = 2,   /* full pools (developer / high-end box) */
} JceMachineClass;

/* Fill cfg with default values. */
JCE_API JceConfig jce_config_defaults(void);

/* Load config from an INI file, overriding only the keys found.
   Returns true if the file was opened successfully.
   If the file doesn't exist, cfg keeps its current values. */
JCE_API bool jce_config_load(JceConfig *cfg, const char *path);

/* Write cfg to an INI file (settings S7: what the in-game settings screen
   persists — window/renderer/audio/locale/performance, plus [graphics] when
   gfx_valid is set).  Returns false on a write error. */
JCE_API bool jce_config_save(const JceConfig *cfg, const char *path);

/* Process-global accessors for the boot-only performance knobs.  jce_engine
 * publishes the loaded values right after jce_config_load so the renderer /
 * jobs layers (which never receive the JceConfig) can read them.  Return the
 * defaults (auto / 0) until published. */
JCE_API JceMachineClass jce_config_machine_class(void);
JCE_API int             jce_config_job_workers(void);
JCE_API void            jce_config_publish_perf(int machine_class, int job_workers);

JCE_EXTERN_C_END

#endif /* JCE_CONFIG_H */
