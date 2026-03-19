/*
 * jce_config.h  Engine configuration with INI file loading.
 *
 * All settings have sensible defaults. If jce.ini exists next to the
 * executable, it overrides individual values.
 */

#ifndef JCE_CONFIG_H
#define JCE_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

/* -- Renderer backend ---------------------------------------------- */

typedef enum JceRendererBackend {
    JCE_BACKEND_AUTO = 0,
    JCE_BACKEND_D3D11,
    JCE_BACKEND_D3D12,
    JCE_BACKEND_VULKAN,
    JCE_BACKEND_OPENGL,
    JCE_BACKEND_OPENGLES,
    JCE_BACKEND_METAL
} JceRendererBackend;

/* -- Configuration ------------------------------------------------- */

typedef struct JceConfig {
    /* Window */
    int         window_width;           /* default: 640 */
    int         window_height;          /* default: 480 */
    char        window_title[128];      /* default: "JCE" */
    bool        fullscreen;             /* default: false */
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
JceConfig jce_config_defaults(void);

/* Load config from an INI file, overriding only the keys found.
   Returns true if the file was opened successfully.
   If the file doesn't exist, cfg keeps its current values. */
bool jce_config_load(JceConfig *cfg, const char *path);

#endif /* JCE_CONFIG_H */
