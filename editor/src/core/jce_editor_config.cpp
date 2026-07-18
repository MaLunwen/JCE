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
#include <jce/os/platform/jce_host_paths.h>
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

/* Editor config (.jce) lives in the user's HOME directory: "~/.jce/<name>".
 * Industry-standard per-user config location — stable across builds/installs
 * and independent of the process CWD and the executable's location.  (The old
 * CWD-relative ".jce" only landed in $HOME by accident on a Finder double-
 * click; this makes it deterministic.)  $HOME via SDL (jce_host_get_user_
 * folder); no platform #ifdef. */
bool jce_editor_dotjce_path(const char *name, char *out, size_t cap) {
    char home[1024];
    bool ok = jce_host_get_user_folder(JCE_USER_FOLDER_HOME, home, sizeof(home));
    if (ok) {
        size_t hl = strlen(home);
        while (hl > 0 && (home[hl - 1] == '/' || home[hl - 1] == '\\'))
            home[--hl] = '\0';
        if (name && *name) snprintf(out, cap, "%s/.jce/%s", home, name);
        else               snprintf(out, cap, "%s/.jce", home);
    } else {
        if (name && *name) snprintf(out, cap, ".jce/%s", name);
        else               snprintf(out, cap, ".jce");
    }
    return ok;
}

static const char *config_dir(void) {
    static char d[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path(NULL, d, sizeof(d)); init = true; }
    return d;
}
/* Legacy single-file store (pre-split).  Still READ on load as the
 * lowest-precedence source so existing configs migrate forward; never
 * written again after the split. */
static const char *config_path(void) {
    static char p[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path("editor-config.json", p, sizeof(p)); init = true; }
    return p;
}
/* Industry-standard split (Unity EditorPrefs vs Library/, Unreal Config
 * vs Saved/): per-user PREFERENCES vs machine-local SESSION/last-state.
 * Both live beside the legacy file under the same ~/.jce anchor. */
static const char *prefs_path(void) {
    static char p[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path("editor-preferences.json", p, sizeof(p)); init = true; }
    return p;
}
static const char *session_path(void) {
    static char p[1024]; static bool init = false;
    if (!init) { jce_editor_dotjce_path("editor-session.json", p, sizeof(p)); init = true; }
    return p;
}
#define CONFIG_PATH   config_path()
#define PREFS_PATH    prefs_path()
#define SESSION_PATH  session_path()
#define CONFIG_DIR    config_dir()

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
    /* Editor default backend.  On Windows, AUTO resolves to D3D12 first
     * (best frame throughput) but D3D12's driver cold-start adds ~0.6-1.0s
     * to editor launch; an EDITOR favours iteration latency, so default to
     * D3D11 (device+swapchain create is far cheaper) — matching Unity's
     * Windows editor default.  D3D12/Vulkan remain one click away in
     * Preferences, and the runtime/game/dist backend chain is untouched
     * (jce_renderer_caps.c stays D3D12-first).  Only affects FRESH configs;
     * an existing saved `renderer` value is preserved on load. */
#if JCE_PLATFORM_WINDOWS
    strncpy(cfg->renderer, "D3D11", sizeof(cfg->renderer) - 1);
#else
    strncpy(cfg->renderer, "Auto", sizeof(cfg->renderer) - 1);
#endif
    cfg->last_project[0] = '\0';
    cfg->last_scene_path[0] = '\0';
    cfg->recent_count = 0;
    cfg->recent_scene_count = 0;
    cfg->autosave_interval = 0;   /* off by default (user preference) */
    cfg->startup_mode      = 0;   /* JCE_EDITOR_STARTUP_LAST */
    cfg->recent_max        = 10;
    cfg->view_mode = 0;    /* JCE_VIEW_SHADED */
    cfg->show_grid = true;
    cfg->gizmo_snap_translate = 0.5f;
    cfg->gizmo_snap_rotate    = 15.0f;
    cfg->gizmo_snap_scale     = 0.25f;
    cfg->show_gizmos          = true;
    cfg->gizmo_scale          = 1.0f;
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
    cfg->external_script_editor[0] = '\0';
    cfg->external_image_editor[0]  = '\0';
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

static bool ui_state_key_valid(const char *key)
{
    if (!key || key[0] == '\0')
        return false;
    return strlen(key) < JCE_EDITOR_UI_STATE_KEY_MAX;
}

static int ui_int_state_find(const JceEditorConfig *cfg, const char *key)
{
    if (!cfg || !ui_state_key_valid(key))
        return -1;
    for (int i = 0; i < cfg->ui_int_state_count; i++) {
        if (strcmp(cfg->ui_int_states[i].key, key) == 0)
            return i;
    }
    return -1;
}

static void load_ui_float_str_state(const JceJson *root);   /* fwd */

static void load_ui_int_state(JceEditorConfig *cfg, const JceJson *root)
{
    const JceJson *ui = jce_json_get(root, "ui_state_int");
    if (!cfg || !jce_json_is_object(ui))
        return;

    cfg->ui_int_state_count = 0;
    for (JceJson *it = jce_json_first_child(ui); it;
         it = jce_json_next_sibling(it)) {
        if (cfg->ui_int_state_count >= JCE_EDITOR_UI_INT_STATE_MAX)
            break;

        const char *key = jce_json_member_key(it);
        if (!ui_state_key_valid(key) || !jce_json_is_number(it))
            continue;

        JceEditorUiIntState *slot =
            &cfg->ui_int_states[cfg->ui_int_state_count++];
        strncpy(slot->key, key, sizeof(slot->key) - 1);
        slot->key[sizeof(slot->key) - 1] = '\0';
        slot->value = (int)jce_json_number_value(it, 0.0);
    }
}

/* ---------- categorized key I/O (preferences vs session) ----------
 *
 * editor-config.json was one flat file mixing two industry-standard
 * categories: per-user PREFERENCES (theme/fonts/input/gizmo display — like
 * Unity's EditorPrefs / Unreal's EditorPreferences) and SESSION/last-state
 * (recent lists, window-visibility mask, last scene, build/run machine
 * paths — like Unity's Library/ or Unreal's Saved/).  These now persist to
 * two files (editor-preferences.json + editor-session.json) so the
 * categories are visible and independently resettable, while keeping ONE
 * in-memory JceEditorConfig and the same load/save API for the ~30 call
 * sites.  apply_*_keys read one category from a JSON root (missing keys
 * keep the current/default value); write_*_keys emit one category. */

#define JCE_EDITOR_CONFIG_SCHEMA 1

/* Read + parse a config file into a JceJson tree (caller frees via
 * jce_json_free).  Returns NULL when the file is absent or unparseable. */
static JceJson *read_config_json(const char *path) {
    size_t len = 0;
    char *buf = (char *)ed_read_file(path, &len);
    if (!buf) return NULL;
    JceJson *root = jce_json_parse(buf, len);
    ED_FREE(buf);
    return root;
}

static void apply_pref_keys(JceEditorConfig *cfg, const JceJson *root) {
    cjson_read_str(root, "language", cfg->language, sizeof(cfg->language));
    cfg->font_size = cjson_read_int(root, "font_size", cfg->font_size);
    cfg->ui_scale  = (float)jce_json_get_number(root, "ui_scale", cfg->ui_scale);
    if (cfg->ui_scale < 0.5f) cfg->ui_scale = 0.5f;
    if (cfg->ui_scale > 3.0f) cfg->ui_scale = 3.0f;
    cjson_read_str(root, "theme",    cfg->theme,    sizeof(cfg->theme));
    cjson_read_str(root, "renderer", cfg->renderer, sizeof(cfg->renderer));
    cfg->gizmo_snap_translate = (float)jce_json_get_number(
        root, "gizmo_snap_translate", cfg->gizmo_snap_translate);
    cfg->gizmo_snap_rotate = (float)jce_json_get_number(
        root, "gizmo_snap_rotate", cfg->gizmo_snap_rotate);
    cfg->gizmo_snap_scale = (float)jce_json_get_number(
        root, "gizmo_snap_scale", cfg->gizmo_snap_scale);
    {
        const JceJson *sg = jce_json_get(root, "show_gizmos");
        if (sg)
            cfg->show_gizmos = jce_json_get_bool(root, "show_gizmos",
                                                 cfg->show_gizmos);
    }
    cfg->gizmo_scale = (float)jce_json_get_number(
        root, "gizmo_scale", cfg->gizmo_scale);
    cfg->asset_browser_view_mode = cjson_read_int(root,
                                                  "asset_browser_view_mode",
                                                  cfg->asset_browser_view_mode);
    cjson_read_str(root, "font_en_path",
                   cfg->font_en_path, sizeof(cfg->font_en_path));
    cjson_read_str(root, "font_zh_path",
                   cfg->font_zh_path, sizeof(cfg->font_zh_path));
    cjson_read_str(root, "external_script_editor",
                   cfg->external_script_editor, sizeof(cfg->external_script_editor));
    cjson_read_str(root, "external_image_editor",
                   cfg->external_image_editor, sizeof(cfg->external_image_editor));
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
    /* General prefs (merged from the retired prefs.json store). */
    cfg->autosave_interval = cjson_read_int(root, "autosave_interval",
                                            cfg->autosave_interval);
    cfg->startup_mode = cjson_read_int(root, "startup_mode", cfg->startup_mode);
    cfg->recent_max   = cjson_read_int(root, "recent_max",   cfg->recent_max);
    if (cfg->recent_max < 1)  cfg->recent_max = 1;
    if (cfg->recent_max > 20) cfg->recent_max = 20;
}

static void apply_session_keys(JceEditorConfig *cfg, const JceJson *root) {
    cjson_read_str(root, "last_project", cfg->last_project, sizeof(cfg->last_project));
    cjson_read_str(root, "last_scene_path",
                   cfg->last_scene_path, sizeof(cfg->last_scene_path));

    /* Scene view render settings (transient last-state). */
    cfg->view_mode = cjson_read_int(root, "view_mode", cfg->view_mode);
    {
        const JceJson *g = jce_json_get(root, "show_grid");
        if (jce_json_is_bool(g))
            cfg->show_grid = jce_json_get_bool(root, "show_grid", cfg->show_grid);
    }

    /* Build/run profile: machine-local (absolute exe/output paths, per-dev
     * iteration toggles) — kept user-local, NOT version-controlled. */
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

    cfg->panels_visible_mask = (uint32_t)jce_json_get_int(
        root, "panels_visible_mask", (int)cfg->panels_visible_mask);
    cfg->panels_visible_mask_hi = (uint32_t)jce_json_get_int(
        root, "panels_visible_mask_hi", (int)cfg->panels_visible_mask_hi);
    cjson_read_str(root, "workspace_id",
                   cfg->workspace_id, sizeof(cfg->workspace_id));
    cjson_read_str(root, "build_project_root",
                   cfg->build_project_root, sizeof(cfg->build_project_root));

    /* recent_0 .. recent_N */
    cfg->recent_count = 0;
    {
        int rcap = (int)(sizeof(cfg->recent_projects) /
                         sizeof(cfg->recent_projects[0]));
        for (int i = 0; i < rcap; i++) {
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
    }

    /* recent_scene_0 .. recent_scene_N */
    cfg->recent_scene_count = 0;
    {
        int rcap = (int)(sizeof(cfg->recent_scene_paths) /
                         sizeof(cfg->recent_scene_paths[0]));
        for (int i = 0; i < rcap; i++) {
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

    load_ui_int_state(cfg, root);
    load_ui_float_str_state(root);
}

/* --------------- load --------------- */

/* ---------- singleton ----------
 *
 * ONE in-memory JceEditorConfig backs the whole editor.  load() hands out
 * copies of it; save() replaces it and marks it dirty.  Disk I/O happens
 * exactly twice per quiet-period (both category files) instead of twice per
 * call across ~30 load-modify-save sites — killing the per-frame reads
 * (File menu, favourites sidebar), the write amplification (snap-increment
 * keystrokes, slider drags), and shrinking the torn-write window to the
 * debounced flush. */
static JceEditorConfig s_live;
static bool  s_live_valid = false;
static bool  s_live_found = false;   /* any source file existed at first load */
static bool  s_dirty      = false;
static float s_quiet      = 0.0f;    /* seconds since the last save() */
static bool  s_had_legacy = false;   /* editor-config.json present at load */
static bool  s_had_old_prefs = false;/* prefs.json (retired store) present  */
static bool  s_schema_newer  = false;/* file written by a NEWER editor      */

static uint64_t s_generation = 0;

static void ui_state_mark_dirty(void) {
    s_dirty = true;
    s_quiet = 0.0f;
    ++s_generation;
}

uint64_t jce_editor_config_generation(void) { return s_generation; }

/* Migrate the retired ~/.jce/prefs.json (Preferences-panel store) into the
 * canonical config: autosave/startup/recent_max become struct fields;
 * per-kind toolchain path overrides become "toolchain.<kind>" string KV. */
static void merge_old_prefs_json(JceEditorConfig *cfg)
{
    char path[1024];
    jce_editor_dotjce_path("prefs.json", path, sizeof(path));
    JceJson *root = read_config_json(path);
    if (!root) return;
    s_had_old_prefs = true;
    /* autosave deliberately NOT migrated: the old panel default was 5min-ON,
     * so nearly every prefs.json carries "2" without the user ever choosing
     * it.  Policy is autosave OFF by default; users re-enable it in
     * Preferences > General. */
    cfg->startup_mode      = cjson_read_int(root, "startup",  cfg->startup_mode);
    cfg->recent_max        = cjson_read_int(root, "recent_max", cfg->recent_max);
    if (cfg->recent_max < 1)  cfg->recent_max = 1;
    if (cfg->recent_max > 20) cfg->recent_max = 20;
    const JceJson *tc = jce_json_get(root, "toolchain_overrides");
    if (jce_json_is_object(tc)) {
        for (JceJson *it = jce_json_first_child(tc); it;
             it = jce_json_next_sibling(it)) {
            const char *kind = jce_json_member_key(it);
            if (!kind || !jce_json_is_string(it)) continue;
            char key[JCE_EDITOR_UI_STATE_KEY_MAX];
            snprintf(key, sizeof(key), "toolchain.%s", kind);
            /* Existing KV wins: it means a post-migration edit. */
            char cur[8];
            if (!jce_editor_config_get_ui_str(key, cur, sizeof(cur)))
                jce_editor_config_set_ui_str(key, jce_json_string_value(it, ""));
        }
    }
    jce_json_free(root);
    LOG_INFO(LOG_TAG, "merged retired prefs.json into editor-preferences");
}

static bool config_load_from_disk(JceEditorConfig *cfg) {
    jce_editor_config_defaults(cfg);

    /* Precedence: new category files win; the legacy single file fills any
     * category whose new file is absent (one-time forward migration with no
     * data loss).  Each category is applied from exactly ONE source. */
    JceJson *pj = read_config_json(PREFS_PATH);
    JceJson *sj = read_config_json(SESSION_PATH);
    JceJson *lj = read_config_json(CONFIG_PATH);
    s_had_legacy = (lj != NULL);

    /* Forward-compat guard: a config written by a NEWER editor keeps its
     * unknown keys only until our next flush rewrites the file.  Detect it,
     * warn once, and keep a one-time backup beside the file. */
    int schema = 0;
    if (pj) schema = cjson_read_int(pj, "_schema", 0);
    if (sj) { int s2 = cjson_read_int(sj, "_schema", 0); if (s2 > schema) schema = s2; }
    if (schema > JCE_EDITOR_CONFIG_SCHEMA) {
        s_schema_newer = true;
        LOG_WARN(LOG_TAG, "config _schema %d is newer than this editor (%d) — "
                 "unknown keys will be dropped on next save", schema,
                 JCE_EDITOR_CONFIG_SCHEMA);
    }

    if (pj)      apply_pref_keys(cfg, pj);
    else if (lj) apply_pref_keys(cfg, lj);

    if (sj)      apply_session_keys(cfg, sj);
    else if (lj) apply_session_keys(cfg, lj);

    /* Retired fourth store (Preferences panel's prefs.json): its keys now
     * live in editor-preferences.json.  Merge only while the new file does
     * not carry them yet (i.e. prefs.json still exists). */
    merge_old_prefs_json(cfg);

    /* Cached input-pref globals (read every frame by viewport handlers). */
    jce_editor_pref_invert_scroll_zoom = cfg->invert_scroll_zoom;
    jce_editor_pref_invert_drag_y      = cfg->invert_drag_y;
    jce_editor_pref_touchpad_h_invert  = cfg->touchpad_h_invert;

    const bool found = (pj || sj || lj || s_had_old_prefs);
    if (pj) jce_json_free(pj);
    if (sj) jce_json_free(sj);
    if (lj) jce_json_free(lj);

    if (!found) {
        LOG_WARN(LOG_TAG, "No editor config found (prefs/session/legacy); using defaults");
        return false;
    }

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

bool jce_editor_config_load(JceEditorConfig *cfg) {
    if (!cfg) return false;
    if (!s_live_valid) {
        s_live_found = config_load_from_disk(&s_live);
        s_live_valid = true;
        /* The retired stores merged above must reach disk in the new
         * format even if the user never changes a setting this session. */
        if (s_had_legacy || s_had_old_prefs) ui_state_mark_dirty();
    }
    *cfg = s_live;
    return s_live_found;
}

/* --------------- save --------------- */

static void write_pref_keys(JceJson *root, const JceEditorConfig *cfg) {
    jce_json_set_string(root, "language",     cfg->language);
    jce_json_set_int   (root, "font_size",    cfg->font_size);
    jce_json_set_number(root, "ui_scale",     cfg->ui_scale);
    jce_json_set_string(root, "theme",        cfg->theme);
    jce_json_set_string(root, "renderer",     cfg->renderer);
    jce_json_set_number(root, "gizmo_snap_translate", cfg->gizmo_snap_translate);
    jce_json_set_number(root, "gizmo_snap_rotate",    cfg->gizmo_snap_rotate);
    jce_json_set_number(root, "gizmo_snap_scale",     cfg->gizmo_snap_scale);
    jce_json_set_bool  (root, "show_gizmos",          cfg->show_gizmos);
    jce_json_set_number(root, "gizmo_scale",          cfg->gizmo_scale);
    jce_json_set_int (root, "asset_browser_view_mode",
                      cfg->asset_browser_view_mode);
    jce_json_set_string(root, "font_en_path", cfg->font_en_path);
    jce_json_set_string(root, "font_zh_path", cfg->font_zh_path);
    jce_json_set_string(root, "external_script_editor", cfg->external_script_editor);
    jce_json_set_string(root, "external_image_editor",  cfg->external_image_editor);
    jce_json_set_bool(root, "invert_scroll_zoom", cfg->invert_scroll_zoom);
    jce_json_set_bool(root, "invert_drag_y",      cfg->invert_drag_y);
    jce_json_set_bool(root, "touchpad_h_invert",  cfg->touchpad_h_invert);
    jce_json_set_bool(root, "auto_repack_on_save", cfg->auto_repack_on_save);
    jce_json_set_bool(root, "run_dev_mode",        cfg->run_dev_mode);
    jce_json_set_int (root, "autosave_interval",   cfg->autosave_interval);
    jce_json_set_int (root, "startup_mode",        cfg->startup_mode);
    jce_json_set_int (root, "recent_max",          cfg->recent_max);
}

/* ---------- module-level generic float / string UI state ----------
 *
 * These live on the config singleton rather than inside JceEditorConfig:
 * the struct is copied by ~30 load/save call sites, and embedding dynamic
 * tables in it would turn every copy into an ownership question.  Loaded
 * from / written to editor-session.json beside ui_state_int. */

typedef struct { char key[JCE_EDITOR_UI_STATE_KEY_MAX]; double value; } UiFloatKV;
typedef struct { char key[JCE_EDITOR_UI_STATE_KEY_MAX]; char value[512]; } UiStrKV;

static UiFloatKV *s_ui_floats = NULL;
static int s_ui_float_count = 0, s_ui_float_cap = 0;
static UiStrKV *s_ui_strs = NULL;
static int s_ui_str_count = 0, s_ui_str_cap = 0;

static void ui_state_mark_dirty(void);   /* fwd (defined with the singleton) */

static int ui_float_find(const char *key) {
    for (int i = 0; i < s_ui_float_count; i++)
        if (strcmp(s_ui_floats[i].key, key) == 0) return i;
    return -1;
}
static int ui_str_find(const char *key) {
    for (int i = 0; i < s_ui_str_count; i++)
        if (strcmp(s_ui_strs[i].key, key) == 0) return i;
    return -1;
}

static void load_ui_float_str_state(const JceJson *root)
{
    const JceJson *uf = jce_json_get(root, "ui_state_float");
    if (jce_json_is_object(uf)) {
        for (JceJson *it = jce_json_first_child(uf); it;
             it = jce_json_next_sibling(it)) {
            const char *key = jce_json_member_key(it);
            if (!ui_state_key_valid(key) || !jce_json_is_number(it)) continue;
            jce_editor_config_set_ui_float(key,
                (float)jce_json_number_value(it, 0.0));
        }
    }
    const JceJson *us = jce_json_get(root, "ui_state_str");
    if (jce_json_is_object(us)) {
        for (JceJson *it = jce_json_first_child(us); it;
             it = jce_json_next_sibling(it)) {
            const char *key = jce_json_member_key(it);
            if (!ui_state_key_valid(key) || !jce_json_is_string(it)) continue;
            jce_editor_config_set_ui_str(key, jce_json_string_value(it, ""));
        }
    }
}

static void write_ui_float_str_state(JceJson *root)
{
    if (s_ui_float_count > 0) {
        JceJson *uf = jce_json_object();
        if (uf) {
            for (int i = 0; i < s_ui_float_count; i++)
                jce_json_set_number(uf, s_ui_floats[i].key, s_ui_floats[i].value);
            jce_json_set_child(root, "ui_state_float", uf);
        }
    }
    if (s_ui_str_count > 0) {
        JceJson *us = jce_json_object();
        if (us) {
            for (int i = 0; i < s_ui_str_count; i++)
                jce_json_set_string(us, s_ui_strs[i].key, s_ui_strs[i].value);
            jce_json_set_child(root, "ui_state_str", us);
        }
    }
}

static void write_session_keys(JceJson *root, const JceEditorConfig *cfg) {
    jce_json_set_string(root, "last_project", cfg->last_project);
    jce_json_set_string(root, "last_scene_path", cfg->last_scene_path);

    /* Scene view render settings. */
    jce_json_set_int (root, "view_mode",  cfg->view_mode);
    jce_json_set_bool(root, "show_grid",  cfg->show_grid);

    /* Build/run profile (machine-local). */
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

    jce_json_set_int (root, "panels_visible_mask", (int)cfg->panels_visible_mask);
    jce_json_set_int (root, "panels_visible_mask_hi", (int)cfg->panels_visible_mask_hi);
    jce_json_set_string(root, "workspace_id", cfg->workspace_id);
    jce_json_set_string(root, "build_project_root", cfg->build_project_root);

    {
        int rcap = (int)(sizeof(cfg->recent_projects) /
                         sizeof(cfg->recent_projects[0]));
        for (int i = 0; i < rcap; i++) {
            char key[16];
            snprintf(key, sizeof(key), "recent_%d", i);
            const char *val = (i < cfg->recent_count) ? cfg->recent_projects[i] : "";
            jce_json_set_string(root, key, val);
        }
    }
    {
        int rcap = (int)(sizeof(cfg->recent_scene_paths) /
                         sizeof(cfg->recent_scene_paths[0]));
        for (int i = 0; i < rcap; i++) {
            char key[24];
            snprintf(key, sizeof(key), "recent_scene_%d", i);
            const char *val = (i < cfg->recent_scene_count) ? cfg->recent_scene_paths[i] : "";
            jce_json_set_string(root, key, val);
        }
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
    {
        JceJson *ui = jce_json_object();
        if (ui) {
            for (int i = 0; i < cfg->ui_int_state_count; i++) {
                const JceEditorUiIntState *slot = &cfg->ui_int_states[i];
                if (ui_state_key_valid(slot->key))
                    jce_json_set_int(ui, slot->key, slot->value);
            }
            jce_json_set_child(root, "ui_state_int", ui);
        }
    }
}

bool jce_editor_config_save(const JceEditorConfig *cfg) {
    if (!cfg) return false;
    /* Adopt the caller's copy as the new live state; the actual disk write
     * is coalesced into the debounced flush below. */
    if (!s_live_valid) {
        /* First touch is a save (possible during early boot): seed the
         * singleton from disk first so unrelated categories are not lost. */
        s_live_found = config_load_from_disk(&s_live);
        s_live_valid = true;
    }
    s_live = *cfg;
    s_live_found = true;

    /* Keep the cached input-pref globals in sync (read every frame). */
    jce_editor_pref_invert_scroll_zoom = cfg->invert_scroll_zoom;
    jce_editor_pref_invert_drag_y      = cfg->invert_drag_y;
    jce_editor_pref_touchpad_h_invert  = cfg->touchpad_h_invert;

    ui_state_mark_dirty();
    return true;
}

/* Write both category files (atomic via the engine JSON writer), then —
 * exactly once, after the first successful flush — retire the superseded
 * stores by renaming them *.migrated: a hand-edit to a dead file can then
 * never be silently inert, and deleting the new files can never resurrect
 * years-old values. */
static bool config_flush_to_disk(void) {
    ensure_directory(CONFIG_DIR);
    bool ok = true;

    /* Preferences (per-user). */
    {
        JceJson *root = jce_json_object();
        if (!root) return false;
        jce_json_set_int(root, "_schema", JCE_EDITOR_CONFIG_SCHEMA);
        write_pref_keys(root, &s_live);
        if (!ed_write_json_to_file(PREFS_PATH, root)) {
            LOG_ERROR(LOG_TAG, "Failed to write preferences: %s", PREFS_PATH);
            ok = false;
        }
    }

    /* Session / last-state (machine-local). */
    {
        JceJson *root = jce_json_object();
        if (!root) return false;
        jce_json_set_int(root, "_schema", JCE_EDITOR_CONFIG_SCHEMA);
        write_session_keys(root, &s_live);
        write_ui_float_str_state(root);
        if (!ed_write_json_to_file(SESSION_PATH, root)) {
            LOG_ERROR(LOG_TAG, "Failed to write session: %s", SESSION_PATH);
            ok = false;
        }
    }

    if (ok) {
        LOG_INFO(LOG_TAG, "Config saved (%s + %s)", PREFS_PATH, SESSION_PATH);
        /* Retire superseded stores (skip when a NEWER editor owns them —
         * downgrade sessions must not eat the newer editor's files). */
        if (!s_schema_newer) {
            if (s_had_legacy) {
                char dst[1024];
                snprintf(dst, sizeof(dst), "%s.migrated", CONFIG_PATH);
                jce_fs_host_remove_file(dst);
                if (jce_fs_host_rename(CONFIG_PATH, dst))
                    LOG_INFO(LOG_TAG, "legacy editor-config.json retired -> %s", dst);
                s_had_legacy = false;
            }
            if (s_had_old_prefs) {
                char src[1024], dst[1024];
                jce_editor_dotjce_path("prefs.json", src, sizeof(src));
                snprintf(dst, sizeof(dst), "%s.migrated", src);
                jce_fs_host_remove_file(dst);
                if (jce_fs_host_rename(src, dst))
                    LOG_INFO(LOG_TAG, "retired prefs.json -> %s", dst);
                s_had_old_prefs = false;
            }
        }
    }
    return ok;
}

void jce_editor_config_flush_tick(float dt_sec) {
    if (!s_dirty) return;
    s_quiet += (dt_sec > 0.0f ? dt_sec : 0.0f);
    if (s_quiet < 0.5f) return;
    s_dirty = false;
    s_quiet = 0.0f;
    config_flush_to_disk();
}

void jce_editor_config_flush_now(void) {
    if (!s_dirty) return;
    s_dirty = false;
    s_quiet = 0.0f;
    config_flush_to_disk();
}

void jce_editor_config_ensure_dir(void) {
    ensure_directory(CONFIG_DIR);
}

/* --------------- add recent --------------- */

/* Max remembered recent entries (Preferences > General > recent_max).
 * Defaults to the array bound (10); the preferences panel pushes the user's
 * value so the persisted recent_max actually bounds the lists instead of
 * being inert.  Clamped to [1,10] (the array capacity). */
static int s_recent_cap = 10;

void jce_editor_config_set_recent_cap(int n) {
    if (n < 1)  n = 1;
    if (n > 20) n = 20;   /* == array capacity; 11..20 used to silently act as 10 */
    s_recent_cap = n;
}

void jce_editor_config_add_recent(JceEditorConfig *cfg, const char *path) {
    if (!path || path[0] == '\0') return;
    const int cap = s_recent_cap;

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
        /* Shift everything down, drop last if at the cap */
        int count = cfg->recent_count < cap ? cfg->recent_count : (cap - 1);
        for (int i = count; i > 0; i--) {
            strncpy(cfg->recent_projects[i], cfg->recent_projects[i - 1],
                    sizeof(cfg->recent_projects[i]) - 1);
            cfg->recent_projects[i][sizeof(cfg->recent_projects[i]) - 1] = '\0';
        }
        if (cfg->recent_count < cap)
            cfg->recent_count++;
    }

    /* Place the new path at the front */
    strncpy(cfg->recent_projects[0], path, sizeof(cfg->recent_projects[0]) - 1);
    cfg->recent_projects[0][sizeof(cfg->recent_projects[0]) - 1] = '\0';
}

void jce_editor_config_add_recent_scene(JceEditorConfig *cfg, const char *path) {
    if (!cfg || !path || path[0] == '\0') return;
    const int cap = s_recent_cap;

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
        int count = cfg->recent_scene_count < cap ? cfg->recent_scene_count : (cap - 1);
        for (int i = count; i > 0; i--) {
            strncpy(cfg->recent_scene_paths[i], cfg->recent_scene_paths[i - 1],
                    sizeof(cfg->recent_scene_paths[i]) - 1);
            cfg->recent_scene_paths[i][sizeof(cfg->recent_scene_paths[i]) - 1] = '\0';
        }
        if (cfg->recent_scene_count < cap)
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

bool jce_editor_config_get_ui_int(const JceEditorConfig *cfg,
                                  const char *key,
                                  int *out_value)
{
    int idx = ui_int_state_find(cfg, key);
    if (idx < 0)
        return false;
    if (out_value)
        *out_value = cfg->ui_int_states[idx].value;
    return true;
}

int jce_editor_config_get_ui_int_or(const JceEditorConfig *cfg,
                                    const char *key,
                                    int fallback)
{
    int value = fallback;
    if (jce_editor_config_get_ui_int(cfg, key, &value))
        return value;
    return fallback;
}

bool jce_editor_config_set_ui_int(JceEditorConfig *cfg,
                                  const char *key,
                                  int value)
{
    if (!cfg || !ui_state_key_valid(key))
        return false;

    int idx = ui_int_state_find(cfg, key);
    if (idx >= 0) {
        cfg->ui_int_states[idx].value = value;
        return true;
    }

    if (cfg->ui_int_state_count >= JCE_EDITOR_UI_INT_STATE_MAX) {
        /* Never drop silently (fcluster lesson): callers void the result. */
        static bool warned = false;
        if (!warned) {
            warned = true;
            LOG_WARN(LOG_TAG, "ui_state_int table full (%d slots) — '%s' and "
                     "later keys will NOT persist", JCE_EDITOR_UI_INT_STATE_MAX,
                     key);
        }
        return false;
    }

    JceEditorUiIntState *slot =
        &cfg->ui_int_states[cfg->ui_int_state_count++];
    strncpy(slot->key, key, sizeof(slot->key) - 1);
    slot->key[sizeof(slot->key) - 1] = '\0';
    slot->value = value;
    return true;
}

/* --------------- generic float / string UI state (module-level) ------- */

bool jce_editor_config_get_ui_float(const char *key, float *out_value)
{
    if (!ui_state_key_valid(key)) return false;
    int idx = ui_float_find(key);
    if (idx < 0) return false;
    if (out_value) *out_value = (float)s_ui_floats[idx].value;
    return true;
}

float jce_editor_config_get_ui_float_or(const char *key, float fallback)
{
    float v = fallback;
    return jce_editor_config_get_ui_float(key, &v) ? v : fallback;
}

void jce_editor_config_set_ui_float(const char *key, float value)
{
    if (!ui_state_key_valid(key)) return;
    int idx = ui_float_find(key);
    if (idx < 0) {
        if (s_ui_float_count >= s_ui_float_cap) {
            int ncap = s_ui_float_cap ? s_ui_float_cap * 2 : 32;
            UiFloatKV *nt = (UiFloatKV *)ED_REALLOC(
                s_ui_floats, (size_t)ncap * sizeof *nt);
            if (!nt) return;
            s_ui_floats = nt; s_ui_float_cap = ncap;
        }
        idx = s_ui_float_count++;
        strncpy(s_ui_floats[idx].key, key, sizeof(s_ui_floats[idx].key) - 1);
        s_ui_floats[idx].key[sizeof(s_ui_floats[idx].key) - 1] = '\0';
        s_ui_floats[idx].value = value;
        ui_state_mark_dirty();
        return;
    }
    if (s_ui_floats[idx].value != (double)value) {
        s_ui_floats[idx].value = value;
        ui_state_mark_dirty();
    }
}

bool jce_editor_config_get_ui_str(const char *key, char *out, size_t cap)
{
    if (!ui_state_key_valid(key) || !out || cap == 0) return false;
    int idx = ui_str_find(key);
    if (idx < 0) return false;
    strncpy(out, s_ui_strs[idx].value, cap - 1);
    out[cap - 1] = '\0';
    return true;
}

void jce_editor_config_set_ui_str(const char *key, const char *value)
{
    if (!ui_state_key_valid(key)) return;
    if (!value) value = "";
    int idx = ui_str_find(key);
    if (idx < 0) {
        if (s_ui_str_count >= s_ui_str_cap) {
            int ncap = s_ui_str_cap ? s_ui_str_cap * 2 : 16;
            UiStrKV *nt = (UiStrKV *)ED_REALLOC(
                s_ui_strs, (size_t)ncap * sizeof *nt);
            if (!nt) return;
            s_ui_strs = nt; s_ui_str_cap = ncap;
        }
        idx = s_ui_str_count++;
        strncpy(s_ui_strs[idx].key, key, sizeof(s_ui_strs[idx].key) - 1);
        s_ui_strs[idx].key[sizeof(s_ui_strs[idx].key) - 1] = '\0';
        s_ui_strs[idx].value[0] = '\0';
    }
    if (strncmp(s_ui_strs[idx].value, value, sizeof(s_ui_strs[idx].value)) != 0) {
        strncpy(s_ui_strs[idx].value, value, sizeof(s_ui_strs[idx].value) - 1);
        s_ui_strs[idx].value[sizeof(s_ui_strs[idx].value) - 1] = '\0';
        ui_state_mark_dirty();
    }
}
