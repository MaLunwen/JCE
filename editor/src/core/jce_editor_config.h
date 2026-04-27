/*
 * jce_editor_config.h  Editor configuration persistence.
 */

#ifndef JCE_EDITOR_CONFIG_H
#define JCE_EDITOR_CONFIG_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_EDITOR_PANELS_MASK_UNSET  0xFFFFFFFFu

typedef struct {
    char language[8];          /* "en" or "zh_cn" */
    int  font_size;            /* 12-32 */
    float ui_scale;            /* DPI scale 0.5..3.0 (default 1.0) */
    char theme[16];            /* "Dark", "Light", "Blue" (Blue maps to SSMS engine theme) */
    char renderer[16];         /* "OpenGL", "Vulkan" */
    char last_project[512];
    char recent_projects[10][512];
    int  recent_count;

    /* Scene view render settings (persisted across sessions). */
    int  view_mode;            /* JceSceneViewMode enum (0=Shaded,1=Wireframe,2=Textured) */
    bool show_grid;

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
       apply. Updated whenever the user toggles a Window menu item. */
    uint32_t panels_visible_mask;
} JceEditorConfig;

/* Load config from .jce/editor-config.json. Returns false if not found. */
bool jce_editor_config_load(JceEditorConfig *cfg);

/* Save config to .jce/editor-config.json. */
bool jce_editor_config_save(const JceEditorConfig *cfg);

/* Ensure the .jce config directory exists (idempotent). */
void jce_editor_config_ensure_dir(void);

/* Set defaults. */
void jce_editor_config_defaults(JceEditorConfig *cfg);

/* Add a path to recent projects (front of list, deduped, max 10). */
void jce_editor_config_add_recent(JceEditorConfig *cfg, const char *path);

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
