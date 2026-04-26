/*
 * jce_editor_config.h  Editor configuration persistence.
 */

#ifndef JCE_EDITOR_CONFIG_H
#define JCE_EDITOR_CONFIG_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct {
    char language[8];          /* "en" or "zh_cn" */
    int  font_size;            /* 12-32 */
    char theme[16];            /* "Dark", "Light", "Blue" */
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

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_CONFIG_H */
