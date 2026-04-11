/*
 * jce_editor_panels.h  Individual editor panel declarations.
 *
 * Matches reference Java editor panels: Hierarchy, Inspector, Console,
 * SceneView, GameView, Timeline, AssetBrowser, FileViewer, Preferences, About.
 */

#ifndef JCE_EDITOR_PANELS_H
#define JCE_EDITOR_PANELS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Panel identifiers. */
typedef enum {
    JCE_PANEL_HIERARCHY = 0,
    JCE_PANEL_INSPECTOR,
    JCE_PANEL_CONSOLE,
    JCE_PANEL_SCENE_VIEW,
    JCE_PANEL_GAME_VIEW,
    JCE_PANEL_TIMELINE,
    JCE_PANEL_ASSETS,
    JCE_PANEL_FILE_VIEWER,
    JCE_PANEL_POSTFX,
    JCE_PANEL_PREFERENCES,
    JCE_PANEL_COUNT
} JceEditorPanel;

/* Lifecycle. */
void  jce_editor_panels_init(void);
void  jce_editor_panels_shutdown(void);

/* Visibility toggle (returns pointer for ImGui::MenuItem binding). */
bool *jce_editor_panel_visible_ptr(JceEditorPanel panel);

/* Draw individual panels as standalone windows (only if visible). */
void  jce_editor_panel_hierarchy(void);
void  jce_editor_panel_inspector(void);
void  jce_editor_panel_console(void);
void  jce_editor_panel_scene_view(void);
void  jce_editor_panel_game_view(void);
void  jce_editor_panel_timeline(void);
void  jce_editor_panel_assets(void);
void  jce_editor_panel_file_viewer(void);
void  jce_editor_panel_postfx(void);
void  jce_editor_panel_preferences(void);

/* Draw panel content only (no Begin/End — for embedding in layout tabs). */
void  jce_editor_panel_hierarchy_content(void);
void  jce_editor_panel_inspector_content(void);
void  jce_editor_panel_console_content(void);
void  jce_editor_panel_scene_view_content(void);
void  jce_editor_panel_game_view_content(void);
void  jce_editor_panel_timeline_content(void);
void  jce_editor_panel_assets_content(void);
void  jce_editor_panel_file_viewer_content(void);
void  jce_editor_panel_postfx_content(void);

/* About dialog (modal). */
void  jce_editor_about_dialog(bool *p_open);

/* Settings dialog (modal — will replace Preferences). */
void  jce_editor_settings_dialog(bool *p_open);

/* File viewer: open a file for preview. */
void  jce_file_viewer_open(const char *path);

/* Asset browser: set the project root directory. */
void  jce_editor_assets_set_project(const char *path);
/* Asset browser: returns true while the delete confirmation dialog is open. */
bool  jce_editor_assets_delete_dialog_open(void);

/* Inspector sync: hierarchy calls this when selection changes. */
void  jce_editor_inspector_request_sync(void);

/* Inspector delete request: opens the same confirmation dialog used by Inspector panel. */
void  jce_editor_inspector_request_delete_confirm(uint32_t entity_id);

/* Inspector delete request (multi-select). */
void  jce_editor_inspector_request_delete_confirm_many(const uint32_t *entity_ids,
                                                       int entity_count);

/* Inspector delete dialog: open-state query + top-level draw call. */
bool  jce_editor_inspector_delete_dialog_open(void);

/* Inspector delete dialog: call each frame from top-level layout. */
void  jce_editor_inspector_delete_dialog(void);

/* Preference getters (for scene view / gizmo integration). */
bool  jce_editor_prefs_show_gizmos(void);
float jce_editor_prefs_gizmo_scale(void);

/* Console log API (with log levels matching reference). */
typedef enum {
    JCE_CONSOLE_INFO = 0,
    JCE_CONSOLE_WARNING,
    JCE_CONSOLE_ERROR,
    JCE_CONSOLE_DEBUG,
} JceConsoleLevel;

void  jce_editor_console_log(const char *fmt, ...);
void  jce_editor_console_log_level(JceConsoleLevel level, const char *fmt, ...);
void  jce_editor_console_clear(void);

/* Console iteration API (for console panel to read entries). */
typedef struct {
    const char     *text;
    const char     *timestamp;
    JceConsoleLevel level;
} JceConsoleEntry;

int   jce_editor_console_entry_count(void);
bool  jce_editor_console_entry_get(int display_idx, JceConsoleEntry *out);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PANELS_H */
