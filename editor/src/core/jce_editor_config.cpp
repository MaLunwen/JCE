/*
 * jce_editor_config.cpp  Editor configuration persistence.
 *
 * Uses cJSON (engine dependency) for JSON parsing and generation.
 */

#include <stdio.h>
#include <string.h>
#include <stdlib.h>

#ifdef _WIN32
#include <direct.h>
#define MKDIR(p) _mkdir(p)
#else
#include <sys/stat.h>
#define MKDIR(p) mkdir(p, 0755)
#endif

extern "C" {
#include <jce/core/jce_log.h>
#include <cjson/cJSON.h>
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
    cfg->font_size = 14;
    strncpy(cfg->theme, "Dark", sizeof(cfg->theme) - 1);
    strncpy(cfg->renderer, "OpenGL", sizeof(cfg->renderer) - 1);
    cfg->last_project[0] = '\0';
    cfg->recent_count = 0;
    cfg->view_mode = 0;    /* JCE_VIEW_SHADED */
    cfg->show_grid = true;
    cfg->asset_browser_view_mode = 0; /* ASSET_BROWSER_VIEW_GRID */
}

/* --------------- helpers --------------- */

static void cjson_read_str(const cJSON *root, const char *key,
                           char *out, size_t out_size) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsString(item) && item->valuestring) {
        strncpy(out, item->valuestring, out_size - 1);
        out[out_size - 1] = '\0';
    }
}

static int cjson_read_int(const cJSON *root, const char *key, int fallback) {
    const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
    if (cJSON_IsNumber(item)) return item->valueint;
    if (cJSON_IsString(item) && item->valuestring) return atoi(item->valuestring);
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

    cJSON *root = cJSON_Parse(buf);
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
        const cJSON *g = cJSON_GetObjectItemCaseSensitive(root, "show_grid");
        if (cJSON_IsBool(g))
            cfg->show_grid = cJSON_IsTrue(g);
    }
    cfg->asset_browser_view_mode = cjson_read_int(root,
                                                  "asset_browser_view_mode",
                                                  cfg->asset_browser_view_mode);

    /* recent_0 .. recent_9 */
    cfg->recent_count = 0;
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "recent_%d", i);
        const cJSON *item = cJSON_GetObjectItemCaseSensitive(root, key);
        if (cJSON_IsString(item) && item->valuestring && item->valuestring[0]) {
            strncpy(cfg->recent_projects[i], item->valuestring,
                    sizeof(cfg->recent_projects[i]) - 1);
            cfg->recent_projects[i][sizeof(cfg->recent_projects[i]) - 1] = '\0';
            cfg->recent_count = i + 1;
        } else {
            cfg->recent_projects[i][0] = '\0';
        }
    }

    cJSON_Delete(root);
    LOG_INFO(LOG_TAG, "Config loaded: lang=%s theme=%s font=%d",
              cfg->language, cfg->theme, cfg->font_size);
    return true;
}

/* --------------- save --------------- */

bool jce_editor_config_save(const JceEditorConfig *cfg) {
    /* Ensure .jce directory exists */
    MKDIR(CONFIG_DIR);

    cJSON *root = cJSON_CreateObject();
    if (!root) return false;

    cJSON_AddStringToObject(root, "language",     cfg->language);
    cJSON_AddNumberToObject(root, "font_size",    cfg->font_size);
    cJSON_AddStringToObject(root, "theme",        cfg->theme);
    cJSON_AddStringToObject(root, "renderer",     cfg->renderer);
    cJSON_AddStringToObject(root, "last_project", cfg->last_project);

    /* Scene view render settings. */
    cJSON_AddNumberToObject(root, "view_mode",  cfg->view_mode);
    cJSON_AddBoolToObject(root, "show_grid",    cfg->show_grid);
    cJSON_AddNumberToObject(root,
                            "asset_browser_view_mode",
                            cfg->asset_browser_view_mode);

    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "recent_%d", i);
        const char *val = (i < cfg->recent_count) ? cfg->recent_projects[i] : "";
        cJSON_AddStringToObject(root, key, val);
    }

    if (!ed_write_json_to_file(CONFIG_PATH, root)) {
        LOG_ERROR(LOG_TAG, "Failed to write config: %s", CONFIG_PATH);
        return false;
    }

    LOG_INFO(LOG_TAG, "Config saved to %s", CONFIG_PATH);
    return true;
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
