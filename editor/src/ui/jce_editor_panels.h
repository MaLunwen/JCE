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
#include <stddef.h>

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
    JCE_PANEL_PROFILER,
    JCE_PANEL_PARTICLE_EDITOR,
    JCE_PANEL_MATERIAL_GRAPH,
    JCE_PANEL_IMPORT_PRESETS,
    JCE_PANEL_LIGHTMAP_BAKE,
    JCE_PANEL_LIGHTING,
    JCE_PANEL_AUDIO_MIXER,
    JCE_PANEL_INPUT_MANAGER,
    JCE_PANEL_CURVE_EDITOR,
    JCE_PANEL_ANIMATION_EDITOR,
    JCE_PANEL_ANIMATOR_SM,
    JCE_PANEL_SEQUENCER,
    JCE_PANEL_NAVMESH,
    JCE_PANEL_TERRAIN,
    JCE_PANEL_PREFERENCES,
    JCE_PANEL_PACKAGE_MANAGER,
    JCE_PANEL_FRAME_DEBUGGER,
    JCE_PANEL_SPRITE_EDITOR,
    JCE_PANEL_TILE_PALETTE,
    JCE_PANEL_VFX_GRAPH,
    JCE_PANEL_TEST_RUNNER,
    JCE_PANEL_BUILD_PROFILES,
    JCE_PANEL_TOOLBAR,
    JCE_PANEL_STATUS_BAR,
    JCE_PANEL_MEMORY_PROFILER,
    JCE_PANEL_PHYSICS_DEBUGGER,
    JCE_PANEL_LIGHT_EXPLORER,
    JCE_PANEL_REFLECTION_PROBES,
    JCE_PANEL_SHADER_GRAPH,
    JCE_PANEL_SEARCH,
    JCE_PANEL_VERSION_CONTROL,
    JCE_PANEL_TIME_OF_DAY,
    JCE_PANEL_VCAM_MANAGER,
    JCE_PANEL_REVERB_ZONES,
    JCE_PANEL_SAVE_BROWSER,
    JCE_PANEL_VSCRIPT_GRAPH,
    JCE_PANEL_COUNT
} JceEditorPanel;

/* Lifecycle. */
void  jce_editor_panels_init(void);
void  jce_editor_panels_shutdown(void);

/* Visibility toggle (returns pointer for ImGui::MenuItem binding). */
bool *jce_editor_panel_visible_ptr(JceEditorPanel panel);

/* Persist current visibility mask to editor-config if changed since last
   call. Cheap (no-op when stable). Call once per frame from the main UI
   loop after panel toggles are processed. */
void  jce_editor_panels_persist_visibility(void);

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
void  jce_editor_panel_profiler(void);
void  jce_editor_panel_particle_editor(void);
void  jce_editor_panel_material_graph(void);
void  jce_editor_panel_import_presets(void);
void  jce_editor_panel_lightmap_bake(void);
void  jce_editor_panel_lighting(void);
void  jce_editor_panel_audio_mixer(void);
void  jce_editor_panel_input_manager(void);
void  jce_editor_panel_curve_editor(void);
void  jce_editor_panel_animation_editor(void);
void  jce_editor_panel_animator_sm(void);
void  jce_editor_panel_sequencer(void);
void  jce_editor_panel_navmesh(void);
void  jce_editor_panel_terrain(void);
void  jce_editor_panel_preferences(void);
void  jce_editor_panel_package_manager(void);
void  jce_editor_panel_frame_debugger(void);
void  jce_editor_panel_sprite_editor(void);
void  jce_editor_panel_tile_palette(void);
void  jce_editor_panel_vfx_graph(void);
void  jce_editor_panel_test_runner(void);
void  jce_editor_panel_build_profiles(void);
void  jce_editor_panel_toolbar(void);
void  jce_editor_panel_status_bar(void);

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
void  jce_editor_panel_lighting_content(void);
void  jce_editor_panel_audio_mixer_content(void);
void  jce_editor_panel_input_manager_content(void);
void  jce_editor_panel_package_manager_content(void);
void  jce_editor_panel_frame_debugger_content(void);
void  jce_editor_panel_sprite_editor_content(void);
void  jce_editor_panel_tile_palette_content(void);
void  jce_editor_panel_vfx_graph_content(void);
void  jce_editor_panel_test_runner_content(void);
void  jce_editor_panel_build_profiles_content(void);
void  jce_editor_panel_profiler_content(void);
void  jce_editor_panel_memory_profiler_content(void);
void  jce_editor_panel_physics_debugger_content(void);
void  jce_editor_panel_light_explorer_content(void);
void  jce_editor_panel_reflection_probes_content(void);
void  jce_editor_panel_shader_graph_content(void);
void  jce_editor_panel_search_content(void);
void  jce_editor_panel_version_control_content(void);
void  jce_editor_panel_time_of_day_content(void);
void  jce_editor_panel_vcam_manager_content(void);
void  jce_editor_panel_reverb_zones_content(void);
void  jce_editor_panel_save_browser_content(void);
void  jce_editor_panel_vscript_graph_content(void);

/* About dialog (modal). */
void  jce_editor_about_dialog(bool *p_open);

/* File viewer: open a file for preview. */
void  jce_file_viewer_open(const char *path);

/* Asset browser: set the project root directory. */
void  jce_editor_assets_set_project(const char *path);
/* Asset browser: returns the current project root (never NULL; may be ""). */
const char *jce_editor_assets_get_project(void);
/* Asset browser: returns true while the delete confirmation dialog is open. */
bool  jce_editor_assets_delete_dialog_open(void);

/* Path normalization helpers (#4 relative paths).
 *
 * Convert an absolute or arbitrary path to a path relative to the current
 * project root when possible.  If the input is already relative or lies
 * outside the project root, the input is copied verbatim (truncated to
 * out_size).  Forward slashes are used in the relative output for cross-
 * platform stability of serialized scenes.
 */
void jce_editor_path_to_relative(char *out, size_t out_size,
                                 const char *abs_or_rel_path);
/* Same as above but uses a caller-supplied base directory.  Useful when
 * the natural anchor is the scene file's directory rather than project root.
 */
void jce_editor_path_to_relative_to(char *out, size_t out_size,
                                    const char *abs_or_rel_path,
                                    const char *base_dir);

/* Inspector sync: hierarchy calls this when selection changes. */
void  jce_editor_inspector_request_sync(void);

/* Reload PBR properties for all entities referencing this material file. */
void  jce_editor_inspector_reload_material(const char *material_path);

/* Inspector delete request: opens the same confirmation dialog used by Inspector panel. */
void  jce_editor_inspector_request_delete_confirm(uint32_t entity_id);

/* Inspector delete request (multi-select). */
void  jce_editor_inspector_request_delete_confirm_many(const uint32_t *entity_ids,
                                                       int entity_count);

/* Inspector delete dialog: open-state query + top-level draw call. */
bool  jce_editor_inspector_delete_dialog_open(void);

/* Inspector delete dialog: call each frame from top-level layout. */
void  jce_editor_inspector_delete_dialog(void);

/* Returns pointer + size of an entity's component struct for the given flag,
 * or NULL if the entity doesn't have it. Used by component clipboard,
 * preset I/O, and multi-edit broadcast. Pointer is owned by the scene. */
void *jce_inspector_comp_blob(struct JceScene *scene, uint64_t e,
                              uint64_t flag, size_t *out_size);

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

/* Renderer-backend dropdown contents.  Populated from
   jce_renderer_caps_list_backends() the first time it is queried.
   Returns the count of entries; *out_names (if non-NULL) receives a
   pointer to a const array of UI strings of that length. */
int   jce_editor_renderer_backends(const char *const **out_names);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PANELS_H */
