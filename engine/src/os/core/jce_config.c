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
#include <jce/os/core/jce_thread.h>   /* settings S5: job_workers sizes the pool */

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
    cfg.renderer_backend_resolved = JCE_BACKEND_AUTO;
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

    /* Settings S5: [performance] job_workers sizes the process-wide frame
     * fork/join pool and is also the upper bound read by structured async
     * executors. Push it rather than let the frame pool pull it: that pool is
     * built lazily and set_workers() is a no-op once it exists. This runs
     * before bgfx init and any frame or background work. Zero leaves the
     * cores-1 policy in charge; a pin is clamped to 1..16. */
    if (job_workers > 0) {
        int w = job_workers > 16 ? 16 : job_workers;
        LOG_INFO(LOG_TAG, "shared job pool: %d worker(s) "
                          "(pinned via [performance] job_workers)", w);
        jce_thread_pool_shared_set_workers(w);
    } else {
        LOG_INFO(LOG_TAG, "shared job pool: auto (cores-1, capped at 8)");
    }
}

JceMachineClass jce_config_machine_class(void)
{
    return (JceMachineClass)s_perf_machine_class;
}

int jce_config_job_workers(void)
{
    return s_perf_job_workers;
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

/* ── enum <-> ini token, one table per enum ────────────────────────
 *
 * Eight hand-written functions used to do this: four `if (strcmp(...))`
 * chains and four `switch` statements, one pair per enum.  They are the
 * kind of duplication that stays correct until it does not -- log_level
 * had a parser and NO formatter, which is part of why jce_config_save
 * silently dropped logging.level from every user file it rewrote.
 *
 * A table cannot have one direction and not the other. */
typedef struct { int value; const char *token; } JceEnumToken;

/* The FIRST row is the fallback in both directions: it is what an
 * unrecognised token parses to, and what an unlisted value formats as. */
static const JceEnumToken k_machine_class[] = {
    { JCE_MACHINE_CLASS_AUTO, "auto" },
    { JCE_MACHINE_CLASS_LOW,  "low"  },
    { JCE_MACHINE_CLASS_FULL, "full" },
};

static const JceEnumToken k_backend[] = {
    { JCE_BACKEND_AUTO,     "auto"     },
    { JCE_BACKEND_D3D11,    "d3d11"    },
    { JCE_BACKEND_D3D12,    "d3d12"    },
    { JCE_BACKEND_VULKAN,   "vulkan"   },
    { JCE_BACKEND_OPENGL,   "opengl"   },
    { JCE_BACKEND_OPENGLES, "opengles" },
    { JCE_BACKEND_METAL,    "metal"    },
};

static const JceEnumToken k_log_level[] = {
    { JCE_LOG_LEVEL_INFO,    "info"    },
    { JCE_LOG_LEVEL_TRACE,   "trace"   },
    { JCE_LOG_LEVEL_DEBUG,   "debug"   },
    { JCE_LOG_LEVEL_SUCCESS, "success" },
    { JCE_LOG_LEVEL_WARN,    "warn"    },
    { JCE_LOG_LEVEL_ERROR,   "error"   },
    { JCE_LOG_LEVEL_OFF,     "off"     },
};

/* gfx quality is a bare int, not an enum: -1 auto, 0..3 tiers, 4 custom.
 * "custom" is the fallback, so it leads. */
static const JceEnumToken k_gfx_quality[] = {
    {  4, "custom" },
    { -1, "auto"   },
    {  0, "low"    },
    {  1, "medium" },
    {  2, "high"   },
    {  3, "ultra"  },
};

#define JCE_ENUM_TOKEN_COUNT(tbl) (sizeof(tbl) / sizeof((tbl)[0]))

static int enum_from_token(const JceEnumToken *tbl, size_t n,
                           const char *token)
{
    size_t i;
    if (token) {
        for (i = 0; i < n; ++i) {
            if (strcmp(token, tbl[i].token) == 0)
                return tbl[i].value;
        }
    }
    return tbl[0].value;
}

static const char *token_from_enum(const JceEnumToken *tbl, size_t n,
                                   int value)
{
    size_t i;
    for (i = 0; i < n; ++i) {
        if (tbl[i].value == value)
            return tbl[i].token;
    }
    return tbl[0].token;
}

#define JCE_PARSE_ENUM(tbl, token) \
    enum_from_token((tbl), JCE_ENUM_TOKEN_COUNT(tbl), (token))
#define JCE_FORMAT_ENUM(tbl, value) \
    token_from_enum((tbl), JCE_ENUM_TOKEN_COUNT(tbl), (value))

static int parse_machine_class(const char *v)
{ return JCE_PARSE_ENUM(k_machine_class, v); }
static const char *machine_class_to_str(int mc)
{ return JCE_FORMAT_ENUM(k_machine_class, mc); }

static JceRendererBackend parse_backend(const char *v)
{ return (JceRendererBackend)JCE_PARSE_ENUM(k_backend, v); }
static const char *backend_to_str(JceRendererBackend b)
{ return JCE_FORMAT_ENUM(k_backend, (int)b); }

static int parse_log_level(const char *v)
{ return JCE_PARSE_ENUM(k_log_level, v); }
static const char *log_level_to_str(int level)
{ return JCE_FORMAT_ENUM(k_log_level, level); }

static int parse_gfx_quality(const char *v)
{ return JCE_PARSE_ENUM(k_gfx_quality, v); }
static const char *gfx_quality_to_str(int q)
{ return JCE_FORMAT_ENUM(k_gfx_quality, q); }

static bool parse_bool(const char *value)
{
    return (strcmp(value, "true") == 0 ||
            strcmp(value, "1")    == 0 ||
            strcmp(value, "yes")  == 0);
}

/* Inverse of parse_log_level, so jce_config_save can write back every key
 * jce_config_load understands.  It had no inverse, which is part of why the
 * key was silently dropped on every save. */
static uint32_t parse_hex(const char *value)
{
    return (uint32_t)SDL_strtoul(value, NULL, 16);
}

/* Graphics quality preset name <-> index (settings S7 follow-up).
 * -1 auto / 0 low / 1 medium / 2 high / 3 ultra / 4 custom. */
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
    else if (strcmp(full, "renderer.backend_resolved") == 0) cfg->renderer_backend_resolved = parse_backend(value);
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

bool jce_config_save(const JceConfig *cfg, const char *path)
{
    if (!cfg || !path) return false;

    /* EVERY KEY jce_config_load() PARSES MUST BE WRITTEN BACK HERE.
     *
     * This is a whole-file truncating write, not a merge, so a key the loader
     * understands but this template omits is DELETED from the user's file the
     * first time anything saves.  Six were: window.title, window.resizable,
     * renderer.debug_text, renderer.clear_color, logging.level and
     * logging.colors -- and the first AUTO launch saves, because that is where
     * the resolved backend is remembered.  So a user who had set a log level
     * or a window title lost it on the first run of a build that remembers its
     * backend, silently.
     *
     * check_config_key_roundtrip.py holds the two sides equal from now on. */
    char buf[2048];
    int n = snprintf(buf, sizeof(buf),
        "# JCE engine config (written by the in-game settings screen).\n"
        "[window]\n"
        "width = %d\n"
        "height = %d\n"
        "fullscreen = %s\n"
        "resizable = %s\n"
        "title = %s\n"
        "\n[renderer]\n"
        "backend = %s\n"
        "backend_resolved = %s\n"
        "vsync = %s\n"
        "debug_text = %s\n"
        "clear_color = %08X\n"
        "\n[audio]\n"
        "master_volume = %.3f\n"
        "music_volume = %.3f\n"
        "sfx_volume = %.3f\n"
        "\n[app]\n"
        "locale = %s\n"
        "\n[logging]\n"
        "level = %s\n"
        "colors = %s\n"
        "\n[performance]\n"
        "machine_class = %s\n"
        "job_workers = %d\n",
        cfg->window_width, cfg->window_height,
        cfg->fullscreen ? "true" : "false",
        cfg->resizable ? "true" : "false",
        cfg->window_title[0] ? cfg->window_title : "JCE",
        backend_to_str(cfg->renderer_backend),
        backend_to_str(cfg->renderer_backend_resolved),
        cfg->vsync ? "true" : "false",
        cfg->debug_text ? "true" : "false",
        (unsigned)cfg->clear_color,
        (double)cfg->master_volume, (double)cfg->music_volume,
        (double)cfg->sfx_volume,
        cfg->locale[0] ? cfg->locale : "",
        log_level_to_str(cfg->log_level),
        cfg->log_colors ? "true" : "false",
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
