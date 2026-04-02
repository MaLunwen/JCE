/*
 * jce_config.c  INI configuration loader.
 *
 * Simple line-oriented parser:
 *   - [section] headers set the current section prefix
 *   - key = value pairs are matched as "section.key"
 *   - # and ; start comments
 *   - Blank lines are ignored
 */

#include <jce/app/jce_config.h>
#include <jce/core/jce_log.h>

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <ctype.h>

#define LOG_TAG "jce_config"

/* -- Defaults ------------------------------------------------------ */

JceConfig jce_config_defaults(void)
{
    JceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.window_width      = 640;
    cfg.window_height     = 480;
    strncpy(cfg.window_title, "JCE", sizeof(cfg.window_title) - 1);
    cfg.fullscreen        = false;
    cfg.resizable         = true;

    cfg.renderer_backend  = JCE_BACKEND_AUTO;
    cfg.vsync             = true;
    cfg.debug_text        = true;
    cfg.clear_color       = 0x000000FF;

    cfg.master_volume     = 1.0f;
    cfg.music_volume      = 0.8f;
    cfg.sfx_volume        = 1.0f;

    cfg.log_level         = JCE_LOG_LEVEL_INFO;
    cfg.log_colors        = true;

    return cfg;
}

/* -- Parse helpers ------------------------------------------------- */

/* Trim leading and trailing whitespace in-place, return pointer. */
static char *trim(char *s)
{
    while (*s && isspace((unsigned char)*s)) s++;
    char *end = s + strlen(s);
    while (end > s && isspace((unsigned char)*(end - 1))) end--;
    *end = '\0';
    return s;
}

static bool parse_bool(const char *value)
{
    return (strcmp(value, "true") == 0 ||
            strcmp(value, "1")    == 0 ||
            strcmp(value, "yes")  == 0);
}

static JceRendererBackend parse_backend(const char *value)
{
    if (strcmp(value, "d3d11")    == 0) return JCE_BACKEND_D3D11;
    if (strcmp(value, "d3d12")    == 0) return JCE_BACKEND_D3D12;
    if (strcmp(value, "vulkan")   == 0) return JCE_BACKEND_VULKAN;
    if (strcmp(value, "opengl")   == 0) return JCE_BACKEND_OPENGL;
    if (strcmp(value, "opengles") == 0) return JCE_BACKEND_OPENGLES;
    if (strcmp(value, "metal")   == 0) return JCE_BACKEND_METAL;
    return JCE_BACKEND_AUTO;
}

static int parse_log_level(const char *value)
{
    if (strcmp(value, "trace")   == 0) return JCE_LOG_LEVEL_TRACE;
    if (strcmp(value, "debug")   == 0) return JCE_LOG_LEVEL_DEBUG;
    if (strcmp(value, "info")    == 0) return JCE_LOG_LEVEL_INFO;
    if (strcmp(value, "success") == 0) return JCE_LOG_LEVEL_SUCCESS;
    if (strcmp(value, "warn")    == 0) return JCE_LOG_LEVEL_WARN;
    if (strcmp(value, "error")   == 0) return JCE_LOG_LEVEL_ERROR;
    if (strcmp(value, "off")     == 0) return JCE_LOG_LEVEL_OFF;
    return JCE_LOG_LEVEL_INFO;
}

static uint32_t parse_hex(const char *value)
{
    return (uint32_t)strtoul(value, NULL, 16);
}

/* -- Apply a section.key = value to config ------------------------- */

static void apply(JceConfig *cfg, const char *section,
                  const char *key, const char *value)
{
    char full[128];
    snprintf(full, sizeof(full), "%s.%s", section, key);

    /* Window */
    if      (strcmp(full, "window.width")  == 0) cfg->window_width  = atoi(value);
    else if (strcmp(full, "window.height") == 0) cfg->window_height = atoi(value);
    else if (strcmp(full, "window.title")  == 0) {
        strncpy(cfg->window_title, value, sizeof(cfg->window_title) - 1);
        cfg->window_title[sizeof(cfg->window_title) - 1] = '\0';
    }
    else if (strcmp(full, "window.fullscreen") == 0) cfg->fullscreen = parse_bool(value);
    else if (strcmp(full, "window.resizable")  == 0) cfg->resizable  = parse_bool(value);

    /* Renderer */
    else if (strcmp(full, "renderer.backend")    == 0) cfg->renderer_backend = parse_backend(value);
    else if (strcmp(full, "renderer.vsync")      == 0) cfg->vsync       = parse_bool(value);
    else if (strcmp(full, "renderer.debug_text") == 0) cfg->debug_text  = parse_bool(value);
    else if (strcmp(full, "renderer.clear_color")== 0) cfg->clear_color = parse_hex(value);

    /* Audio */
    else if (strcmp(full, "audio.master_volume") == 0) cfg->master_volume = (float)atof(value);
    else if (strcmp(full, "audio.music_volume")  == 0) cfg->music_volume  = (float)atof(value);
    else if (strcmp(full, "audio.sfx_volume")    == 0) cfg->sfx_volume    = (float)atof(value);

    /* Logging */
    else if (strcmp(full, "logging.level")  == 0) cfg->log_level  = parse_log_level(value);
    else if (strcmp(full, "logging.colors") == 0) cfg->log_colors = parse_bool(value);

    else {
        LOG_WARN(LOG_TAG, "unknown config key: %s", full);
    }
}

/* -- INI file loader ----------------------------------------------- */

bool jce_config_load(JceConfig *cfg, const char *path)
{
    if (!cfg || !path) return false;

    FILE *fp = fopen(path, "r");
    if (!fp) return false;

    LOG_INFO(LOG_TAG, "loading %s", path);

    char line[512];
    char section[64] = "";

    while (fgets(line, sizeof(line), fp)) {
        char *s = trim(line);

        /* Skip empty lines and comments. */
        if (*s == '\0' || *s == '#' || *s == ';') continue;

        /* Section header: [name] */
        if (*s == '[') {
            char *end = strchr(s, ']');
            if (end) {
                *end = '\0';
                strncpy(section, s + 1, sizeof(section) - 1);
                section[sizeof(section) - 1] = '\0';
            }
            continue;
        }

        /* Key = value */
        char *eq = strchr(s, '=');
        if (!eq) continue;

        *eq = '\0';
        const char *key   = trim(s);
        const char *value = trim(eq + 1);

        if (*key != '\0' && section[0] != '\0') {
            apply(cfg, section, key, value);
        }
    }

    fclose(fp);
    return true;
}
