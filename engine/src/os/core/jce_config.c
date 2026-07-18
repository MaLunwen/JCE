/*
 * jce_config.c  INI configuration loader.
 *
 * Simple line-oriented parser:
 *   - [section] headers set the current section prefix
 *   - key = value pairs are matched as "section.key"
 *   - # and ; start comments
 *   - Blank lines are ignored
 */

#include <jce/os/core/jce_config.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <ctype.h>
#include <SDL3/SDL.h>
#include <stdio.h>
#include <string.h>

#define LOG_TAG "jce_config"

/* -- Defaults ------------------------------------------------------ */

JceConfig jce_config_defaults(void)
{
    JceConfig cfg;
    memset(&cfg, 0, sizeof(cfg));

    cfg.window_width      = 640;
    cfg.window_height     = 480;
    SDL_strlcpy(cfg.window_title, "JCE", sizeof(cfg.window_title));
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

    cfg.machine_class     = JCE_MACHINE_CLASS_AUTO;
    cfg.job_workers       = 0;   /* auto */

    /* Graphics (settings S7 follow-up) — inert until a [graphics] section
     * flips gfx_valid.  "custom" quality keeps the boot-resolved pipeline
     * untouched, so a hand-written partial section only layers the keys it
     * actually names on top of sensible mid-tier values. */
    cfg.gfx_valid          = false;
    cfg.gfx_quality        = 4;      /* custom */
    cfg.gfx_shadows        = true;
    cfg.gfx_ssao           = true;
    cfg.gfx_bloom          = true;
    cfg.gfx_fog            = false;
    cfg.gfx_shadow_quality = 1;
    cfg.gfx_msaa           = 1;

    return cfg;
}

/* ── Boot-only perf knobs: process-global publish (settings S5) ─────── */

static int s_perf_machine_class = JCE_MACHINE_CLASS_AUTO;
static int s_perf_job_workers   = 0;

void jce_config_publish_perf(int machine_class, int job_workers)
{
    if (machine_class < 0 || machine_class > JCE_MACHINE_CLASS_FULL)
        machine_class = JCE_MACHINE_CLASS_AUTO;
    if (job_workers < 0) job_workers = 0;
    s_perf_machine_class = machine_class;
    s_perf_job_workers   = job_workers;
}

JceMachineClass jce_config_machine_class(void)
{
    return (JceMachineClass)s_perf_machine_class;
}

int jce_config_job_workers(void)
{
    return s_perf_job_workers;
}

static int parse_machine_class(const char *value)
{
    if (strcmp(value, "low")  == 0) return JCE_MACHINE_CLASS_LOW;
    if (strcmp(value, "full") == 0) return JCE_MACHINE_CLASS_FULL;
    return JCE_MACHINE_CLASS_AUTO;   /* "auto" or anything else */
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
    return (uint32_t)SDL_strtoul(value, NULL, 16);
}

/* Graphics quality preset name <-> index (settings S7 follow-up).
 * -1 auto / 0 low / 1 medium / 2 high / 3 ultra / 4 custom. */
static int parse_gfx_quality(const char *value)
{
    if (strcmp(value, "auto")   == 0) return -1;
    if (strcmp(value, "low")    == 0) return 0;
    if (strcmp(value, "medium") == 0) return 1;
    if (strcmp(value, "high")   == 0) return 2;
    if (strcmp(value, "ultra")  == 0) return 3;
    return 4;   /* "custom" or anything else */
}

static const char *gfx_quality_to_str(int q)
{
    switch (q) {
    case -1: return "auto";
    case 0:  return "low";
    case 1:  return "medium";
    case 2:  return "high";
    case 3:  return "ultra";
    default: return "custom";
    }
}

/* -- Apply a section.key = value to config ------------------------- */

static void apply(JceConfig *cfg, const char *section,
                  const char *key, const char *value)
{
    char full[128];
    snprintf(full, sizeof(full), "%s.%s", section, key);

    /* Window */
    if      (strcmp(full, "window.width")  == 0) cfg->window_width  = SDL_atoi(value);
    else if (strcmp(full, "window.height") == 0) cfg->window_height = SDL_atoi(value);
    else if (strcmp(full, "window.title")  == 0) {
        SDL_strlcpy(cfg->window_title, value, sizeof(cfg->window_title));
    }
    else if (strcmp(full, "window.fullscreen") == 0) cfg->fullscreen = parse_bool(value);
    else if (strcmp(full, "window.resizable")  == 0) cfg->resizable  = parse_bool(value);

    /* Renderer */
    else if (strcmp(full, "renderer.backend")    == 0) cfg->renderer_backend = parse_backend(value);
    else if (strcmp(full, "renderer.vsync")      == 0) cfg->vsync       = parse_bool(value);
    else if (strcmp(full, "renderer.debug_text") == 0) cfg->debug_text  = parse_bool(value);
    else if (strcmp(full, "renderer.clear_color")== 0) cfg->clear_color = parse_hex(value);

    /* Audio */
    else if (strcmp(full, "audio.master_volume") == 0) cfg->master_volume = (float)SDL_atof(value);
    else if (strcmp(full, "audio.music_volume")  == 0) cfg->music_volume  = (float)SDL_atof(value);
    else if (strcmp(full, "audio.sfx_volume")    == 0) cfg->sfx_volume    = (float)SDL_atof(value);

    /* Logging */
    else if (strcmp(full, "logging.level")  == 0) cfg->log_level  = parse_log_level(value);
    else if (strcmp(full, "logging.colors") == 0) cfg->log_colors = parse_bool(value);

    /* App */
    else if (strcmp(full, "app.locale") == 0) {
        SDL_strlcpy(cfg->locale, value, sizeof(cfg->locale));
    }

    /* Performance (settings S5) */
    else if (strcmp(full, "performance.machine_class") == 0)
        cfg->machine_class = parse_machine_class(value);
    else if (strcmp(full, "performance.job_workers") == 0)
        cfg->job_workers = SDL_atoi(value);

    /* Graphics (settings S7 follow-up: in-game graphics tab).  Any key in
     * the section marks the whole block valid so boot layers it onto the
     * render pipeline; absent section = gfx_valid stays false = no-op. */
    else if (strcmp(full, "graphics.quality") == 0) {
        cfg->gfx_quality = parse_gfx_quality(value);
        cfg->gfx_valid   = true;
    }
    else if (strcmp(full, "graphics.shadows") == 0) {
        cfg->gfx_shadows = parse_bool(value);
        cfg->gfx_valid   = true;
    }
    else if (strcmp(full, "graphics.ssao") == 0) {
        cfg->gfx_ssao  = parse_bool(value);
        cfg->gfx_valid = true;
    }
    else if (strcmp(full, "graphics.bloom") == 0) {
        cfg->gfx_bloom = parse_bool(value);
        cfg->gfx_valid = true;
    }
    else if (strcmp(full, "graphics.fog") == 0) {
        cfg->gfx_fog   = parse_bool(value);
        cfg->gfx_valid = true;
    }
    else if (strcmp(full, "graphics.shadow_quality") == 0) {
        int sq = SDL_atoi(value);
        if (sq < 0) sq = 0;
        if (sq > 2) sq = 2;
        cfg->gfx_shadow_quality = sq;
        cfg->gfx_valid          = true;
    }
    else if (strcmp(full, "graphics.msaa") == 0) {
        int ms = SDL_atoi(value);
        if (ms != 1 && ms != 2 && ms != 4 && ms != 8) ms = 1;
        cfg->gfx_msaa  = ms;
        cfg->gfx_valid = true;
    }

    else {
        LOG_WARN(LOG_TAG, "unknown config key: %s", full);
    }
}

/* -- INI file loader ----------------------------------------------- */

bool jce_config_load(JceConfig *cfg, const char *path)
{
    if (!cfg || !path) return false;

    uint64_t total = 0;
    char *buf = (char *)jce_fs_host_read_all(path, &total);
    if (!buf) return false;

    LOG_INFO(LOG_TAG, "loading %s", path);

    if (total == 0) { JCE_FREE(buf); return false; }
    /* host_read_all guarantees a trailing NUL sentinel byte. */

    /* Parse lines from the in-memory buffer. */
    char section[64] = "";
    char *cursor = buf;

    while (*cursor) {
        /* Extract one line. */
        char *eol = cursor;
        while (*eol && *eol != '\n' && *eol != '\r') eol++;

        char saved = *eol;
        *eol = '\0';

        char *s = trim(cursor);

        if (*s != '\0' && *s != '#' && *s != ';') {
            if (*s == '[') {
                char *end = strchr(s, ']');
                if (end) {
                    *end = '\0';
                    SDL_strlcpy(section, s + 1, sizeof(section));
                }
            } else {
                char *eq = strchr(s, '=');
                if (eq) {
                    *eq = '\0';
                    const char *key   = trim(s);
                    const char *value = trim(eq + 1);
                    if (*key != '\0' && section[0] != '\0')
                        apply(cfg, section, key, value);
                }
            }
        }

        /* Advance past the line ending. */
        *eol = saved;
        if (*eol == '\r') eol++;
        if (*eol == '\n') eol++;
        cursor = eol;
    }

    JCE_FREE(buf);
    return true;
}

/* -- INI file saver (settings S7: in-game persistence) ------------- */

static const char *backend_to_str(JceRendererBackend b)
{
    switch (b) {
    case JCE_BACKEND_D3D11:    return "d3d11";
    case JCE_BACKEND_D3D12:    return "d3d12";
    case JCE_BACKEND_VULKAN:   return "vulkan";
    case JCE_BACKEND_OPENGL:   return "opengl";
    case JCE_BACKEND_OPENGLES: return "opengles";
    case JCE_BACKEND_METAL:    return "metal";
    default:                   return "auto";
    }
}

static const char *machine_class_to_str(int mc)
{
    switch (mc) {
    case JCE_MACHINE_CLASS_LOW:  return "low";
    case JCE_MACHINE_CLASS_FULL: return "full";
    default:                     return "auto";
    }
}

bool jce_config_save(const JceConfig *cfg, const char *path)
{
    if (!cfg || !path) return false;

    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "# JCE engine config (written by the in-game settings screen).\n"
        "[window]\n"
        "width = %d\n"
        "height = %d\n"
        "fullscreen = %s\n"
        "\n[renderer]\n"
        "backend = %s\n"
        "vsync = %s\n"
        "\n[audio]\n"
        "master_volume = %.3f\n"
        "music_volume = %.3f\n"
        "sfx_volume = %.3f\n"
        "\n[app]\n"
        "locale = %s\n"
        "\n[performance]\n"
        "machine_class = %s\n"
        "job_workers = %d\n",
        cfg->window_width, cfg->window_height,
        cfg->fullscreen ? "true" : "false",
        backend_to_str(cfg->renderer_backend),
        cfg->vsync ? "true" : "false",
        (double)cfg->master_volume, (double)cfg->music_volume,
        (double)cfg->sfx_volume,
        cfg->locale[0] ? cfg->locale : "",
        machine_class_to_str(cfg->machine_class),
        cfg->job_workers);
    if (n <= 0 || n >= (int)sizeof(buf)) return false;

    /* Graphics (settings S7 follow-up): only emit the section once the
     * in-game screen has filled it — never pin defaults on top of a
     * project's .rp.json for a config that never touched graphics. */
    if (cfg->gfx_valid) {
        int g = snprintf(buf + n, sizeof(buf) - (size_t)n,
            "\n[graphics]\n"
            "quality = %s\n"
            "shadows = %s\n"
            "ssao = %s\n"
            "bloom = %s\n"
            "fog = %s\n"
            "shadow_quality = %d\n"
            "msaa = %d\n",
            gfx_quality_to_str(cfg->gfx_quality),
            cfg->gfx_shadows ? "true" : "false",
            cfg->gfx_ssao    ? "true" : "false",
            cfg->gfx_bloom   ? "true" : "false",
            cfg->gfx_fog     ? "true" : "false",
            cfg->gfx_shadow_quality,
            cfg->gfx_msaa);
        if (g <= 0 || g >= (int)(sizeof(buf) - (size_t)n)) return false;
        n += g;
    }

    if (!jce_fs_host_write_all(path, buf, (size_t)n)) {
        LOG_WARN(LOG_TAG, "failed to write config: %s", path);
        return false;
    }
    LOG_INFO(LOG_TAG, "saved %s", path);
    return true;
}
