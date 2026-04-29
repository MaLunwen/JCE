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
} JceConfig;

/* Fill cfg with default values. */
JCE_API JceConfig jce_config_defaults(void);

/* Load config from an INI file, overriding only the keys found.
   Returns true if the file was opened successfully.
   If the file doesn't exist, cfg keeps its current values. */
JCE_API bool jce_config_load(JceConfig *cfg, const char *path);

JCE_EXTERN_C_END

#endif /* JCE_CONFIG_H */
