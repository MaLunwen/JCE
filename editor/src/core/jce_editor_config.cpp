/*
 * jce_editor_config.cpp  Editor configuration persistence.
 *
 * Uses the engine JSON facade for JSON parsing and generation.
 */

#include <stdio.h>
#include <stdlib.h>
#include <string.h>

extern "C" {
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_filesystem.h>
#include <jce/os/core/jce_json.h>
#include <jce/os/core/jce_log.h>
}

/* Cross-platform mkdir-equivalent. Creates the directory if it does not
 * already exist. */
static void ensure_directory(const char *path) {
    jce_fs_host_create_directory(path);
}

#include "jce_editor_alloc.h"
#include "jce_editor_config.h"
#include "io/jce_editor_file_util.h"

#define LOG_TAG       "editor_config"
#define CONFIG_PATH   ".jce/editor-config.json"
#define CONFIG_DIR    ".jce"

/* Cached input preference flags. */
bool jce_editor_pref_invert_scroll_zoom = false;
bool jce_editor_pref_invert_drag_y      = false;
bool jce_editor_pref_touchpad_h_invert  = true;

/* --------------- defaults --------------- */

void jce_editor_config_defaults(JceEditorConfig *cfg) {
    memset(cfg, 0, sizeof(*cfg));
    strncpy(cfg->language, "en", sizeof(cfg->language) - 1);
    cfg->font_size = 24;
    cfg->ui_scale  = 1.0f;
    strncpy(cfg->theme, "Dark", sizeof(cfg->theme) - 1);
    strncpy(cfg->renderer, "OpenGL", sizeof(cfg->renderer) - 1);
    cfg->last_project[0] = '\0';
    cfg->last_scene_path[0] = '\0';
    cfg->recent_count = 0;
    cfg->recent_scene_count = 0;
    cfg->view_mode = 0;    /* JCE_VIEW_SHADED */
    cfg->show_grid = true;
    cfg->asset_browser_view_mode = 0; /* ASSET_BROWSER_VIEW_GRID */
    cfg->asset_favorite_count = 0;
    /* asset_favorites left zero-initialised by the memset above. */
    cfg->run_mode = 0; /* Editor Simulation */
    /* Default points to the canonical CMake-preset output.  Forward slashes
     * work on every host (Windows accepts them in CreateProcess paths).
     * jce_run_manager performs smart resolution at spawn-time: tries
     * configured path → with platform exe suffix → walks parent dirs →
     * tries other known build/desktop/<arch>/[release/] candidates. */
#if JCE_PLATFORM_WINDOWS
    strncpy(cfg->game_executable_path,
            "build/desktop/windows-x64/release/caged_kingdom.exe",
            sizeof(cfg->game_executable_path) - 1);
    strncpy(cfg->game_working_directory, "build/desktop/windows-x64/release",
            sizeof(cfg->game_working_directory) - 1);
#elif JCE_PLATFORM_MACOS
    strncpy(cfg->game_executable_path,
            "build/desktop/macos-arm64/CagedKingdom",
            sizeof(cfg->game_executable_path) - 1);
    strncpy(cfg->game_working_directory, "build/desktop/macos-arm64",
            sizeof(cfg->game_working_directory) - 1);
#elif JCE_PLATFORM_LINUX
    strncpy(cfg->game_executable_path,
            "build/desktop/linux-x64/CagedKingdom",
            sizeof(cfg->game_executable_path) - 1);
    strncpy(cfg->game_working_directory, "build/desktop/linux-x64",
            sizeof(cfg->game_working_directory) - 1);
#else
    strncpy(cfg->game_executable_path, "build/host/release/caged_kingdom",
            sizeof(cfg->game_executable_path) - 1);
    strncpy(cfg->game_working_directory, "build/host/release",
            sizeof(cfg->game_working_directory) - 1);
#endif
    strncpy(cfg->game_target_name, "CagedKingdom", sizeof(cfg->game_target_name) - 1);
    strncpy(cfg->build_configure_preset, "host-release",
            sizeof(cfg->build_configure_preset) - 1);
    strncpy(cfg->build_preset, "build-host-release", sizeof(cfg->build_preset) - 1);
    strncpy(cfg->build_output_path, "build/host/release",
            sizeof(cfg->build_output_path) - 1);
    cfg->font_en_path[0] = '\0';
    cfg->font_zh_path[0] = '\0';
    cfg->invert_scroll_zoom = false;
    cfg->invert_drag_y      = false;
    cfg->touchpad_h_invert  = true;
    cfg->auto_repack_on_save = false;
    cfg->run_dev_mode = true;
    cfg->panels_visible_mask = JCE_EDITOR_PANELS_MASK_UNSET;
    cfg->panels_visible_mask_hi = 0u;
    strncpy(cfg->workspace_id, "default", sizeof(cfg->workspace_id) - 1);
    cfg->build_project_root[0] = '\0';
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
    cfg->ui_scale  = (float)jce_json_get_number(root, "ui_scale", cfg->ui_scale);
    if (cfg->ui_scale < 0.5f) cfg->ui_scale = 0.5f;
    if (cfg->ui_scale > 3.0f) cfg->ui_scale = 3.0f;
    cjson_read_str(root, "theme",    cfg->theme,    sizeof(cfg->theme));
    cjson_read_str(root, "renderer", cfg->renderer, sizeof(cfg->renderer));
    cjson_read_str(root, "last_project", cfg->last_project, sizeof(cfg->last_project));
    cjson_read_str(root, "last_scene_path",
                   cfg->last_scene_path, sizeof(cfg->last_scene_path));

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
    cfg->invert_scroll_zoom = jce_json_get_bool(root, "invert_scroll_zoom",
                                                cfg->invert_scroll_zoom);
    cfg->invert_drag_y      = jce_json_get_bool(root, "invert_drag_y",
                                                cfg->invert_drag_y);
    cfg->touchpad_h_invert  = jce_json_get_bool(root, "touchpad_h_invert",
                                                cfg->touchpad_h_invert);
    cfg->auto_repack_on_save = jce_json_get_bool(root, "auto_repack_on_save",
                                                 cfg->auto_repack_on_save);
    cfg->run_dev_mode = jce_json_get_bool(root, "run_dev_mode",
                                          cfg->run_dev_mode);
    cfg->panels_visible_mask = (uint32_t)jce_json_get_int(
        root, "panels_visible_mask", (int)cfg->panels_visible_mask);
    cfg->panels_visible_mask_hi = (uint32_t)jce_json_get_int(
        root, "panels_visible_mask_hi", (int)cfg->panels_visible_mask_hi);
    cjson_read_str(root, "workspace_id",
                   cfg->workspace_id, sizeof(cfg->workspace_id));
    cjson_read_str(root, "build_project_root",
                   cfg->build_project_root, sizeof(cfg->build_project_root));
    jce_editor_pref_invert_scroll_zoom = cfg->invert_scroll_zoom;
    jce_editor_pref_invert_drag_y      = cfg->invert_drag_y;
    jce_editor_pref_touchpad_h_invert  = cfg->touchpad_h_invert;

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

    /* recent_scene_0 .. recent_scene_9 */
    cfg->recent_scene_count = 0;
    for (int i = 0; i < 10; i++) {
        char key[24];
        snprintf(key, sizeof(key), "recent_scene_%d", i);
        const char *s = jce_json_get_string(root, key, NULL);
        if (s && s[0]) {
            strncpy(cfg->recent_scene_paths[i], s,
                    sizeof(cfg->recent_scene_paths[i]) - 1);
            cfg->recent_scene_paths[i][sizeof(cfg->recent_scene_paths[i]) - 1] = '\0';
            cfg->recent_scene_count = i + 1;
        } else {
            cfg->recent_scene_paths[i][0] = '\0';
        }
    }

    /* asset_favorite_0 .. asset_favorite_11  (asset browser Locations) */
    cfg->asset_favorite_count = 0;
    {
        int cap = (int)(sizeof(cfg->asset_favorites) /
                        sizeof(cfg->asset_favorites[0]));
        for (int i = 0; i < cap; i++) {
            char key[24];
            snprintf(key, sizeof(key), "asset_favorite_%d", i);
            const char *s = jce_json_get_string(root, key, NULL);
            if (s && s[0]) {
                strncpy(cfg->asset_favorites[i], s,
                        sizeof(cfg->asset_favorites[i]) - 1);
                cfg->asset_favorites[i]
                                    [sizeof(cfg->asset_favorites[i]) - 1] = '\0';
                cfg->asset_favorite_count = i + 1;
            } else {
                cfg->asset_favorites[i][0] = '\0';
            }
        }
    }

    jce_json_free(root);
    /* Suppress repetitive logging: jce_editor_config_load() is called from
       ~30 sites during startup (panels, dialogs, state init, etc.) and each
       call would otherwise spam an identical line. Log only when the
       observable summary (lang/theme/font) actually changes vs the previous
       successful load. The first call always logs. */
    static char  s_last_lang[8]  = {0};
    static char  s_last_theme[16] = {0};
    static int   s_last_font     = -1;
    if (s_last_font != cfg->font_size ||
        strncmp(s_last_lang, cfg->language, sizeof(s_last_lang)) != 0 ||
        strncmp(s_last_theme, cfg->theme, sizeof(s_last_theme)) != 0) {
        LOG_INFO(LOG_TAG, "Config loaded: lang=%s theme=%s font=%d",
                  cfg->language, cfg->theme, cfg->font_size);
        strncpy(s_last_lang, cfg->language, sizeof(s_last_lang) - 1);
        s_last_lang[sizeof(s_last_lang) - 1] = '\0';
        strncpy(s_last_theme, cfg->theme, sizeof(s_last_theme) - 1);
        s_last_theme[sizeof(s_last_theme) - 1] = '\0';
        s_last_font = cfg->font_size;
    }
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
    jce_json_set_number(root, "ui_scale",     cfg->ui_scale);
    jce_json_set_string(root, "theme",        cfg->theme);
    jce_json_set_string(root, "renderer",     cfg->renderer);
    jce_json_set_string(root, "last_project", cfg->last_project);
    jce_json_set_string(root, "last_scene_path", cfg->last_scene_path);

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
    jce_json_set_bool(root, "invert_scroll_zoom", cfg->invert_scroll_zoom);
    jce_json_set_bool(root, "invert_drag_y",      cfg->invert_drag_y);
    jce_json_set_bool(root, "touchpad_h_invert",  cfg->touchpad_h_invert);
    jce_json_set_bool(root, "auto_repack_on_save", cfg->auto_repack_on_save);
    jce_json_set_bool(root, "run_dev_mode",        cfg->run_dev_mode);
    jce_json_set_int (root, "panels_visible_mask", (int)cfg->panels_visible_mask);
    jce_json_set_int (root, "panels_visible_mask_hi", (int)cfg->panels_visible_mask_hi);
    jce_json_set_string(root, "workspace_id", cfg->workspace_id);
    jce_json_set_string(root, "build_project_root", cfg->build_project_root);
    jce_editor_pref_invert_scroll_zoom = cfg->invert_scroll_zoom;
    jce_editor_pref_invert_drag_y      = cfg->invert_drag_y;
    jce_editor_pref_touchpad_h_invert  = cfg->touchpad_h_invert;

    for (int i = 0; i < 10; i++) {
        char key[16];
        snprintf(key, sizeof(key), "recent_%d", i);
        const char *val = (i < cfg->recent_count) ? cfg->recent_projects[i] : "";
        jce_json_set_string(root, key, val);
    }
    for (int i = 0; i < 10; i++) {
        char key[24];
        snprintf(key, sizeof(key), "recent_scene_%d", i);
        const char *val = (i < cfg->recent_scene_count) ? cfg->recent_scene_paths[i] : "";
        jce_json_set_string(root, key, val);
    }

    {
        int cap = (int)(sizeof(cfg->asset_favorites) /
                        sizeof(cfg->asset_favorites[0]));
        for (int i = 0; i < cap; i++) {
            char key[24];
            snprintf(key, sizeof(key), "asset_favorite_%d", i);
            const char *val = (i < cfg->asset_favorite_count) ?
                              cfg->asset_favorites[i] : "";
            jce_json_set_string(root, key, val);
        }
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

void jce_editor_config_add_recent_scene(JceEditorConfig *cfg, const char *path) {
    if (!cfg || !path || path[0] == '\0') return;

    /* Filter: only accept .scene.json files (post A1-A6 unification). */
    size_t plen = strlen(path);
    const char *suffix = ".scene.json";
    size_t slen = strlen(suffix);
    if (plen < slen) return;
    /* Case-insensitive ASCII suffix compare. */
    bool suffix_ok = true;
    for (size_t i = 0; i < slen; i++) {
        char a = path[plen - slen + i];
        char b = suffix[i];
        if (a >= 'A' && a <= 'Z') a = (char)(a - 'A' + 'a');
        if (a != b) { suffix_ok = false; break; }
    }
    if (!suffix_ok) return;

    /* Normalize backslashes to forward slashes so dedupe works regardless
     * of whether the path came from Win32 APIs or our IO layer. */
    char norm[512];
    size_t n = plen < sizeof(norm) - 1 ? plen : sizeof(norm) - 1;
    for (size_t i = 0; i < n; i++) {
        char c = path[i];
        norm[i] = (c == '\\') ? '/' : c;
    }
    norm[n] = '\0';

    int dup_idx = -1;
    for (int i = 0; i < cfg->recent_scene_count; i++) {
        if (strcmp(cfg->recent_scene_paths[i], norm) == 0) {
            dup_idx = i;
            break;
        }
    }

    if (dup_idx >= 0) {
        for (int i = dup_idx; i > 0; i--) {
            strncpy(cfg->recent_scene_paths[i], cfg->recent_scene_paths[i - 1],
                    sizeof(cfg->recent_scene_paths[i]) - 1);
            cfg->recent_scene_paths[i][sizeof(cfg->recent_scene_paths[i]) - 1] = '\0';
        }
    } else {
        int count = cfg->recent_scene_count < 10 ? cfg->recent_scene_count : 9;
        for (int i = count; i > 0; i--) {
            strncpy(cfg->recent_scene_paths[i], cfg->recent_scene_paths[i - 1],
                    sizeof(cfg->recent_scene_paths[i]) - 1);
            cfg->recent_scene_paths[i][sizeof(cfg->recent_scene_paths[i]) - 1] = '\0';
        }
        if (cfg->recent_scene_count < 10)
            cfg->recent_scene_count++;
    }

    strncpy(cfg->recent_scene_paths[0], norm, sizeof(cfg->recent_scene_paths[0]) - 1);
    cfg->recent_scene_paths[0][sizeof(cfg->recent_scene_paths[0]) - 1] = '\0';
}

/* --------------- favourites --------------- */

/* Normalise an arbitrary host path to the canonical form we store on
   disk: forward slashes, no trailing separator (except for drive roots
   like "C:/" which collapse to "C:/").  Idempotent. */
static void favorite_normalize(const char *in, char *out, size_t cap)
{
    if (cap == 0) return;
    out[0] = '\0';
    if (!in || !in[0]) return;
    size_t len = strlen(in);
    if (len + 1 > cap) len = cap - 1;
    for (size_t i = 0; i < len; i++)
        out[i] = (in[i] == '\\') ? '/' : in[i];
    out[len] = '\0';
    while (len > 1 && out[len - 1] == '/' &&
           !(len == 3 && out[1] == ':')) {
        out[--len] = '\0';
    }
}

bool jce_editor_config_add_favorite(JceEditorConfig *cfg, const char *path)
{
    if (!cfg || !path || !path[0]) return false;
    char norm[512];
    favorite_normalize(path, norm, sizeof(norm));
    if (!norm[0]) return false;

    for (int i = 0; i < cfg->asset_favorite_count; i++) {
        if (strcmp(cfg->asset_favorites[i], norm) == 0)
            return false;
    }
    int cap = (int)(sizeof(cfg->asset_favorites) /
                    sizeof(cfg->asset_favorites[0]));
    if (cfg->asset_favorite_count >= cap) return false;
    strncpy(cfg->asset_favorites[cfg->asset_favorite_count], norm,
            sizeof(cfg->asset_favorites[0]) - 1);
    cfg->asset_favorites[cfg->asset_favorite_count]
                       [sizeof(cfg->asset_favorites[0]) - 1] = '\0';
    cfg->asset_favorite_count++;
    return true;
}

bool jce_editor_config_remove_favorite(JceEditorConfig *cfg, const char *path)
{
    if (!cfg || !path || !path[0]) return false;
    char norm[512];
    favorite_normalize(path, norm, sizeof(norm));
    if (!norm[0]) return false;

    for (int i = 0; i < cfg->asset_favorite_count; i++) {
        if (strcmp(cfg->asset_favorites[i], norm) == 0) {
            for (int j = i + 1; j < cfg->asset_favorite_count; j++) {
                strncpy(cfg->asset_favorites[j - 1], cfg->asset_favorites[j],
                        sizeof(cfg->asset_favorites[0]) - 1);
                cfg->asset_favorites[j - 1]
                                    [sizeof(cfg->asset_favorites[0]) - 1] = '\0';
            }
            cfg->asset_favorite_count--;
            cfg->asset_favorites[cfg->asset_favorite_count][0] = '\0';
            return true;
        }
    }
    return false;
}
