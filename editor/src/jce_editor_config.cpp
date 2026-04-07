/*
 * jce_editor_config.cpp  Editor configuration persistence.
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
}

#include "jce_editor_config.h"

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
}

/* --------------- simple JSON helpers --------------- */

/* Extract the value for a given key from a flat JSON buffer.
   Writes result into out (up to out_size-1 chars). Returns true on success. */
static bool json_get_string(const char *json, const char *key, char *out, size_t out_size) {
    /* Build the search pattern: "key" */
    char pattern[64];
    snprintf(pattern, sizeof(pattern), "\"%s\"", key);

    const char *pos = strstr(json, pattern);
    if (!pos) return false;

    pos += strlen(pattern);

    /* skip whitespace and colon */
    while (*pos == ' ' || *pos == '\t' || *pos == '\n' || *pos == '\r' || *pos == ':') pos++;

    if (*pos != '"') return false;
    pos++; /* skip opening quote */

    size_t i = 0;
    while (*pos && *pos != '"' && i < out_size - 1) {
        if (*pos == '\\' && *(pos + 1)) {
            pos++; /* skip backslash, take next char */
        }
        out[i++] = *pos++;
    }
    out[i] = '\0';
    return true;
}

/* --------------- load --------------- */

bool jce_editor_config_load(JceEditorConfig *cfg) {
    jce_editor_config_defaults(cfg);

    FILE *f = fopen(CONFIG_PATH, "rb");
    if (!f) {
        LOG_WARN(LOG_TAG, "Config file not found: %s", CONFIG_PATH);
        return false;
    }

    fseek(f, 0, SEEK_END);
    long len = ftell(f);
    fseek(f, 0, SEEK_SET);

    if (len <= 0 || len > 64 * 1024) {
        fclose(f);
        LOG_ERROR(LOG_TAG, "Config file invalid size: %ld", len);
        return false;
    }

    char *buf = (char *)malloc((size_t)len + 1);
    if (!buf) {
        fclose(f);
        return false;
    }

    size_t rd = fread(buf, 1, (size_t)len, f);
    fclose(f);
    buf[rd] = '\0';

    /* Parse known keys */
    char tmp[512];

    if (json_get_string(buf, "language", tmp, sizeof(tmp)))
        strncpy(cfg->language, tmp, sizeof(cfg->language) - 1);

    if (json_get_string(buf, "font_size", tmp, sizeof(tmp)))
        cfg->font_size = atoi(tmp);

    if (json_get_string(buf, "theme", tmp, sizeof(tmp)))
        strncpy(cfg->theme, tmp, sizeof(cfg->theme) - 1);

    if (json_get_string(buf, "renderer", tmp, sizeof(tmp)))
        strncpy(cfg->renderer, tmp, sizeof(cfg->renderer) - 1);

    if (json_get_string(buf, "last_project", tmp, sizeof(tmp)))
        strncpy(cfg->last_project, tmp, sizeof(cfg->last_project) - 1);

    /* recent_0 .. recent_9 */
    cfg->recent_count = 0;
    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "recent_%d", i);
        if (json_get_string(buf, key, tmp, sizeof(tmp)) && tmp[0] != '\0') {
            strncpy(cfg->recent_projects[i], tmp, sizeof(cfg->recent_projects[i]) - 1);
            cfg->recent_count = i + 1;
        } else {
            cfg->recent_projects[i][0] = '\0';
        }
    }

    free(buf);
    LOG_INFO(LOG_TAG, "Config loaded: lang=%s theme=%s font=%d",
              cfg->language, cfg->theme, cfg->font_size);
    return true;
}

/* --------------- save --------------- */

bool jce_editor_config_save(const JceEditorConfig *cfg) {
    /* Ensure .jce directory exists */
    MKDIR(CONFIG_DIR);

    FILE *f = fopen(CONFIG_PATH, "w");
    if (!f) {
        LOG_ERROR(LOG_TAG, "Failed to write config: %s", CONFIG_PATH);
        return false;
    }

    fprintf(f, "{\n");
    fprintf(f, "    \"language\": \"%s\",\n", cfg->language);
    fprintf(f, "    \"font_size\": \"%d\",\n", cfg->font_size);
    fprintf(f, "    \"theme\": \"%s\",\n", cfg->theme);
    fprintf(f, "    \"renderer\": \"%s\",\n", cfg->renderer);
    fprintf(f, "    \"last_project\": \"%s\",\n", cfg->last_project);

    for (int i = 0; i < 10; i++) {
        const char *val = (i < cfg->recent_count) ? cfg->recent_projects[i] : "";
        const char *comma = (i < 9) ? "," : "";
        fprintf(f, "    \"recent_%d\": \"%s\"%s\n", i, val, comma);
    }

    fprintf(f, "}\n");
    fclose(f);

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
