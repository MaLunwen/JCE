/*
 * jce_editor_config.h  Editor configuration persistence.
 */

#ifndef JCE_EDITOR_CONFIG_H
#define JCE_EDITOR_CONFIG_H

#include <stdbool.h>
#include <stdint.h>
#include <stddef.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_EDITOR_PANELS_MASK_UNSET  0xFFFFFFFFu
#define JCE_EDITOR_UI_INT_STATE_MAX   64
#define JCE_EDITOR_UI_STATE_KEY_MAX   64

typedef struct {
    char key[JCE_EDITOR_UI_STATE_KEY_MAX];
    int  value;
} JceEditorUiIntState;

typedef struct {
    char language[8];          /* "en" or "zh_cn" */
    int  font_size;            /* 12-32 */
    float ui_scale;            /* DPI scale 0.5..3.0 (default 1.0) */
    char theme[16];            /* "Dark", "Light", "Blue" (Blue maps to SSMS engine theme) */
    char renderer[16];         /* "OpenGL", "Vulkan" */
    char last_project[512];
    char last_scene_path[512];
    char recent_projects[10][512];
    int  recent_count;
    char recent_scene_paths[10][512];
    int  recent_scene_count;

    /* Scene view render settings (persisted across sessions). */
    int  view_mode;            /* JceSceneViewMode enum (0=Shaded,1=Wireframe,2=Textured) */
    bool show_grid;

    /* Gizmo Ctrl-snap increments (persisted across sessions).  Defaults
       0.5 units / 15 degrees / 0.25 ratio. */
    float gizmo_snap_translate;
    float gizmo_snap_rotate;
    float gizmo_snap_scale;

    /* Asset Browser settings (persisted across sessions). */
    int  asset_browser_view_mode; /* AssetBrowserViewMode enum (0=Grid,1=Details) */

    /* External game run/build settings. */
    int  run_mode;             /* 0=Editor Simulation, 1=External Game */
    char game_executable_path[512];
    char game_working_directory[512];
    char game_target_name[128];
    char build_configure_preset[128];
    char build_preset[128];
    char build_output_path[512];

    /* When true, saving a scene file kicks an incremental
       `cmake --build … --target PackGameAssets` so external ck.exe sees
       the change without a manual CLI round-trip.  Default false to
       avoid surprising users on slow machines.  Launching ck always
       repacks unconditionally (see jce_run_manager). */
    bool auto_repack_on_save;

    /* When true, the editor passes `--dev <project_assets_dir>` to the
       spawned game so it mounts loose source files on top of the PAK.
       Material .mat.json edits then hot-reload in the running game
       without rebuilding the PAK.  Default true: this is the common
       "designer iteration" workflow. Disable for clean-room launches
       that should mirror a shipped build. */
    bool run_dev_mode;

    /* Font overrides (empty -> default lookup: try system Ink Free /
       KaiTi by name, fall back to ImGui built-in proggy). */
    char font_en_path[512];
    char font_zh_path[512];

    /* Input preferences. */
    bool invert_scroll_zoom;   /* MouseWheel: false=natural (up=zoom in) */
    bool invert_drag_y;        /* MouseDrag Y axis (orbit/pan/zoom) */
    bool touchpad_h_invert;    /* Touchpad horizontal: true = browser-style
                                  (RtoL swipe reveals right content). Mouse
                                  wheel is unaffected — fractional wheel.x
                                  is treated as touchpad. Default true. */

    /* Window panel visibility persistence. Bit i corresponds to
       JceEditorPanel value i. Sentinel JCE_EDITOR_PANELS_MASK_UNSET
       means "never saved" — defaults from jce_editor_panels_init()
       apply. Updated whenever the user toggles a Window menu item.
       `panels_visible_mask` covers panels 0..31; `panels_visible_mask_hi`
       covers panels 32..63 (added when panel count exceeded 32). */
    uint32_t panels_visible_mask;
    uint32_t panels_visible_mask_hi;

    /* Maya-style Workspace persistence. Stable id string of the active
       workspace ("default", "modeling", "rigging", "animation", "fx",
       "rendering", "uvEditing", "sculpting"). Loaded at startup by
       jce_workspace_init() and rewritten whenever the user switches
       workspace via the menu-bar dropdown or Ctrl+F1..F7. */
    char workspace_id[32];

    /* Project root for the Build Profiles panel.  Empty = use editor's
     * cwd.  Picked via the panel's "Browse…" button (a file picker for
     * CMakePresets.json — its parent directory is stored here).
     * Used as the working_directory for spawned cmake / conan tools. */
    char build_project_root[512];

    /* Asset Browser "Locations" favourites — user-pinned folders shown
     * in the left sidebar above the project tree.  Persisted so the
     * user's curated quick-access list survives editor restarts. */
    char asset_favorites[12][512];
    int  asset_favorite_count;

    /* Generic editor-UI state for stable integer values such as active
       workbench tabs, selected modes, and other per-user panel chrome.
       Keys are stable ASCII identifiers like "panel.animation.current_tab". */
    JceEditorUiIntState ui_int_states[JCE_EDITOR_UI_INT_STATE_MAX];
    int                 ui_int_state_count;
} JceEditorConfig;

/* Load config from .jce/editor-config.json. Returns false if not found. */
bool jce_editor_config_load(JceEditorConfig *cfg);

/* Save config to .jce/editor-config.json. */
bool jce_editor_config_save(const JceEditorConfig *cfg);

/* Ensure the .jce config directory exists (idempotent). */
void jce_editor_config_ensure_dir(void);

/* Build "<exe_dir>/.jce/<name>" (or "<exe_dir>/.jce" when name is NULL/empty)
 * into `out`.  Anchored to the EXECUTABLE directory (the "launch directory",
 * via jce_fs_host_get_base_path), NOT the process CWD: a Finder/`open`
 * double-click runs with CWD=$HOME, so CWD-relative ".jce" dropped editor
 * files into the home directory.  Returns false if the base path can't be
 * resolved (out then falls back to a CWD-relative ".jce/<name>"). */
bool jce_editor_dotjce_path(const char *name, char *out, size_t cap);

/* Set defaults. */
void jce_editor_config_defaults(JceEditorConfig *cfg);

/* Add a path to recent projects (front of list, deduped, max 10). */
void jce_editor_config_add_recent(JceEditorConfig *cfg, const char *path);

/* Add a path to recent scenes (front of list, deduped, max 10). */
void jce_editor_config_add_recent_scene(JceEditorConfig *cfg, const char *path);

/* Asset Browser favourites helpers — dedupe-aware, max 12.  Idempotent. */
bool jce_editor_config_add_favorite(JceEditorConfig *cfg, const char *path);
bool jce_editor_config_remove_favorite(JceEditorConfig *cfg, const char *path);

/* Generic UI integer state helpers.  Return false for NULL/empty/too-long
   keys or when the fixed storage table is full. */
bool jce_editor_config_get_ui_int(const JceEditorConfig *cfg,
                                  const char *key,
                                  int *out_value);
int  jce_editor_config_get_ui_int_or(const JceEditorConfig *cfg,
                                     const char *key,
                                     int fallback);
bool jce_editor_config_set_ui_int(JceEditorConfig *cfg,
                                  const char *key,
                                  int value);

/* Cached input preference flags — kept in sync by load/save.
   Read directly by scene/particle viewport input handlers (avoids
   re-loading the JSON every frame). */
extern bool jce_editor_pref_invert_scroll_zoom;
extern bool jce_editor_pref_invert_drag_y;
extern bool jce_editor_pref_touchpad_h_invert;

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_CONFIG_H */
