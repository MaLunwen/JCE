/*
 * jce_editor_config.cpp  Editor configuration persistence.
 *
 * Uses the engine JSON facade for JSON parsing and generation.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#include <SDL3/SDL_filesystem.h>

extern "C" {
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_json.h>
}

/* Cross-platform mkdir-equivalent. SDL_CreateDirectory creates the directory
 * if it does not already exist, returning true if the directory exists after
 * the call. */
static void ensure_directory(const char *path) {
    SDL_CreateDirectory(path);
}

#include "jce_editor_config.h"
#include "jce_editor_alloc.h"
#include "jce_editor_file_util.h"

#define LOG_TAG       "editor_config"
#define CONFIG_PATH   ".jce/editor-config.json"
#define CONFIG_DIR    ".jce"

/* --------------- defaults --------------- */

void jce_editor_config_defaults(JceEditorConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->language, "en", sizeof(cfg->language) - 1);
    cfg->font_size = 24;
    strncpy(cfg->theme, "Dark", sizeof(cfg->theme) - 1);
    strncpy(cfg->renderer, "OpenGL", sizeof(cfg->renderer) - 1);
    cfg->last_project[0] = '\0';
    cfg->recent_count = 0;
    cfg->view_mode = 0;    /* JCE_VIEW_SHADED */
    cfg->show_grid = true;
    cfg->asset_browser_view_mode = 0; /* ASSET_BROWSER_VIEW_GRID */
    cfg->run_mode = 0; /* Editor Simulation */
    /* Forward slashes work on all hosts including Windows, so the defaults
     * stay platform-neutral. The .exe suffix is intentionally omitted; the
     * executable resolver in jce_run_manager will validate existence and the
     * Build dialog (Phase 2) will overwrite this value to the freshly produced
     * binary path including any host-specific suffix. */
    strncpy(cfg->game_executable_path, "build/host/release/caged_kingdom",
            sizeof(cfg->game_executable_path) - 1);
    strncpy(cfg->game_working_directory, "build/host/release",
            sizeof(cfg->game_working_directory) - 1);
    strncpy(cfg->game_target_name, "CagedKingdom", sizeof(cfg->game_target_name) - 1);
    strncpy(cfg->build_configure_preset, "host-release",
            sizeof(cfg->build_configure_preset) - 1);
    strncpy(cfg->build_preset, "build-host-release", sizeof(cfg->build_preset) - 1);
    strncpy(cfg->build_output_path, "build/host/release",
            sizeof(cfg->build_output_path) - 1);
    cfg->font_en_path[0] = '\0';
    cfg->font_zh_path[0] = '\0';
}

/* --------------- helpers --------------- */

static void cjson_read_str(const JceJson *root, const char *key,
                           char *out, size_t out_size) {
    const char *s = jce_json_get_string(root, key, NULL);
    if (s) {
        strncpy(out, s, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static int cjson_read_int(const JceJson *root, const char *key, int fallback) {
    const JceJson *item = jce_json_get(root, key);
    if (jce_json_is_number(item))
        return jce_json_get_int(root, key, fallback);
    if (jce_json_is_string(item)) {
        const char *s = jce_json_get_string(root, key, NULL);
        if (s) return atoi(s);
    }
    return fallback;
}

/* --------------- load --------------- */

bool jce_editor_config_load(JceEditorConfig *cfg) {
    jce_editor_config_defaults(cfg);

    size_t len = 0;
    char *buf = (char *)ed_read_file(CONFIG_PATH, &len);
    if (!buf) {
        LOG_WARN(LOG_TAG, "Config file not found: %s", CONFIG_PATH);
        return false;
    }

    JceJson *root = jce_json_parse(buf, len);
    ED_FREE(buf);
    if (!root) {
        LOG_ERROR(LOG_TAG, "Config JSON parse error");
        return false;
    }

    cjson_read_str(root, "language", cfg->language, sizeof(cfg->language));
    cfg->font_size = cjson_read_int(root, "font_size", cfg->font_size);
    cjson_read_str(root, "theme",    cfg->theme,    sizeof(cfg->theme));
    cjson_read_str(root, "renderer", cfg->renderer, sizeof(cfg->renderer));
    cjson_read_str(root, "last_project", cfg->last_project, sizeof(cfg->last_project));

    /* Scene view render settings. */
    cfg->view_mode = cjson_read_int(root, "view_mode", cfg->view_mode);
    {
        const JceJson *g = jce_json_get(root, "show_grid");
        if (jce_json_is_bool(g))
            cfg->show_grid = jce_json_get_bool(root, "show_grid", cfg->show_grid);
    }
    cfg->asset_browser_view_mode = cjson_read_int(root,
                                                  "asset_browser_view_mode",
                                                  cfg->asset_browser_view_mode);

    cfg->run_mode = cjson_read_int(root, "run_mode", cfg->run_mode);
    cjson_read_str(root, "game_executable_path",
                   cfg->game_executable_path, sizeof(cfg->game_executable_path));
    cjson_read_str(root, "game_working_directory",
                   cfg->game_working_directory, sizeof(cfg->game_working_directory));
    cjson_read_str(root, "game_target_name",
                   cfg->game_target_name, sizeof(cfg->game_target_name));
    cjson_read_str(root, "build_configure_preset",
                   cfg->build_configure_preset, sizeof(cfg->build_configure_preset));
    cjson_read_str(root, "build_preset",
                   cfg->build_preset, sizeof(cfg->build_preset));
    cjson_read_str(root, "build_output_path",
                   cfg->build_output_path, sizeof(cfg->build_output_path));
    cjson_read_str(root, "font_en_path",
                   cfg->font_en_path, sizeof(cfg->font_en_path));
    cjson_read_str(root, "font_zh_path",
                   cfg->font_zh_path, sizeof(cfg->font_zh_path));

    /* recent_0 .. recent_9 */
    cfg->recent_count = 0;
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "recent_%d", i);
        const char *s = jce_json_get_string(root, key, NULL);
        if (s && s[0]) {
            strncpy(cfg->recent_projects[i], s,
                    sizeof(cfg->recent_projects[i]) - 1);
            cfg->recent_projects[i][sizeof(cfg->recent_projects[i]) - 1] = '\0';
            cfg->recent_count = i + 1;
        } else {
            cfg->recent_projects[i][0] = '\0';
        }
    }

    jce_json_free(root);
    LOG_INFO(LOG_TAG, "Config loaded: lang=%s theme=%s font=%d",
              cfg->language, cfg->theme, cfg->font_size);
    return true;
}

/* --------------- save --------------- */

bool jce_editor_config_save(const JceEditorConfig *cfg) {
    /* Ensure .jce directory exists */
    ensure_directory(CONFIG_DIR);

    JceJson *root = jce_json_object();
    if (!root) return false;

    jce_json_set_string(root, "language",     cfg->language);
    jce_json_set_int   (root, "font_size",    cfg->font_size);
    jce_json_set_string(root, "theme",        cfg->theme);
    jce_json_set_string(root, "renderer",     cfg->renderer);
    jce_json_set_string(root, "last_project", cfg->last_project);

    /* Scene view render settings. */
    jce_json_set_int (root, "view_mode",  cfg->view_mode);
    jce_json_set_bool(root, "show_grid",  cfg->show_grid);
    jce_json_set_int (root, "asset_browser_view_mode",
                      cfg->asset_browser_view_mode);
    jce_json_set_int (root, "run_mode", cfg->run_mode);
    jce_json_set_string(root, "game_executable_path",
                        cfg->game_executable_path);
    jce_json_set_string(root, "game_working_directory",
                        cfg->game_working_directory);
    jce_json_set_string(root, "game_target_name", cfg->game_target_name);
    jce_json_set_string(root, "build_configure_preset",
                        cfg->build_configure_preset);
    jce_json_set_string(root, "build_preset", cfg->build_preset);
    jce_json_set_string(root, "build_output_path", cfg->build_output_path);
    jce_json_set_string(root, "font_en_path", cfg->font_en_path);
    jce_json_set_string(root, "font_zh_path", cfg->font_zh_path);

    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "recent_%d", i);
        const char *val = (i < cfg->recent_count) ? cfg->recent_projects[i] : "";
        jce_json_set_string(root, key, val);
    }

    if (!ed_write_json_to_file(CONFIG_PATH, root)) {
        LOG_ERROR(LOG_TAG, "Failed to write config: %s", CONFIG_PATH);
        return false;
    }

    LOG_INFO(LOG_TAG, "Config saved to %s", CONFIG_PATH);
    return true;
}

void jce_editor_config_ensure_dir(void) {
    ensure_directory(CONFIG_DIR);
}

/* --------------- add recent --------------- */

void jce_editor_config_add_recent(JceEditorConfig *cfg, const char *path) {
    if (!path || path[0] == '\0') return;

    /* Remove duplicate if it already exists */
    int dup_idx = -1;
    for (int i = 0; i < cfg->recent_count; i++) {
        if (strcmp(cfg->recent_projects[i], path) == 0) {
            dup_idx = i;
            break;
        }
    }

    if (dup_idx >= 0) {
        /* Shift entries between 0..dup_idx-1 down by one to make room at front */
        for (int i = dup_idx; i > 0; i--) {
            strncpy(cfg->recent_projects[i], cfg->recent_projects[i - 1],
                    sizeof(cfg->recent_projects[i]) - 1);
            cfg->recent_projects[i][sizeof(cfg->recent_projects[i]) - 1] = '\0';
        }
    } else {
        /* Shift everything down, drop last if full */
        int count = cfg->recent_count < 10 ? cfg->recent_count : 9;
        for (int i = count; i > 0; i--) {
            strncpy(cfg->recent_projects[i], cfg->recent_projects[i - 1],
                    sizeof(cfg->recent_projects[i]) - 1);
            cfg->recent_projects[i][sizeof(cfg->recent_projects[i]) - 1] = '\0';
        }
        if (cfg->recent_count < 10)
            cfg->recent_count++;
    }

    /* Place the new path at the front */
    strncpy(cfg->recent_projects[0], path, sizeof(cfg->recent_projects[0]) - 1);
    cfg->recent_projects[0][sizeof(cfg->recent_projects[0]) - 1] = '\0';
}
