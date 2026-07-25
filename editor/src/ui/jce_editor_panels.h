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
    /* DEPRECATED (P5-A.4): merged into JCE_PANEL_LIGHTING_SETTINGS.
     * Slot kept to preserve the integer index — the visibility bitmap is
     * serialized to disk by ordinal, so removing this would silently
     * shift every later panel's persisted toggle. New code must not
     * reference this; use JCE_PANEL_LIGHTING_SETTINGS instead. */
    JCE_PANEL_LIGHTING_DEPRECATED,
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
    JCE_PANEL_BUNDLE_BROWSER,
    JCE_PANEL_PHYSICS_LAYERS,
    JCE_PANEL_TAGS_LAYERS,
    JCE_PANEL_LIGHTING_SETTINGS,
    JCE_PANEL_BUILD_REPORT,
    JCE_PANEL_SYSTEMS,
    JCE_PANEL_PROJECT_SETTINGS,
    JCE_PANEL_USER_PREFERENCES,
    JCE_PANEL_LAN_DISCOVERY,
    JCE_PANEL_NETWORK_STATS,
    JCE_PANEL_ANIMATION_RIGGING,
    JCE_PANEL_RENDER_PIPELINE,
    JCE_PANEL_PROFILE_ANALYZER,
    JCE_PANEL_BT_VISUALIZER,
    /* APPEND ONLY before JCE_PANEL_COUNT — the visibility bitmap is
     * serialized to disk by ordinal (see jce_editor_panels_init). */
    JCE_PANEL_WORLD_STREAMING,
    JCE_PANEL_USER_GUIDE,
    JCE_PANEL_COUNT
} JceEditorPanel;

/* Lifecycle. */
void  jce_editor_panels_init(void);
void  jce_editor_panels_shutdown(void);

/* Visibility toggle (returns pointer for ImGui::MenuItem binding). */
bool *jce_editor_panel_visible_ptr(JceEditorPanel panel);

/* Queue a dock-tab focus/bring-to-front request for a stable ImGui ID
 * such as "###console". Applied at frame boundary so menu popups can
 * remain open while Window menu entries focus panels. */
void  jce_editor_panel_request_focus(const char *stable_window_id);

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
/* True while the Game View is actively driving game input (the user clicked in
 * to control the FPS fly-cam / WASD free-fly / player, OR the mouse is
 * captured) — stays true during a transient LeftAlt free-look release.  The
 * editor shortcut gate consults this (gated by Play state) so editor hotkeys
 * (Ctrl+S / Ctrl+A / …) never mis-fire while the user is playing in the Game
 * View.  Play-consistent: broader than the strict mouse-captured check. */
bool  jce_editor_game_view_is_input_active(void);
void  jce_editor_panel_timeline(void);
void  jce_editor_panel_assets(void);
void  jce_editor_panel_file_viewer(void);
void  jce_editor_panel_postfx(void);
void  jce_editor_panel_particle_editor(void);
void  jce_editor_panel_material_graph(void);
/* Open .matgraph.json (or auto-derive one from a .mat.json) in the
 * Material Graph panel.  Routes used by the Asset Browser double-click,
 * the file viewer (.matgraph.json route), and the Inspector's
 * "Open in Material Graph" button.  No-op on NULL / empty path. */
void  jce_editor_open_material_graph(const char *path);
void  jce_editor_panel_import_presets(void);
void  jce_editor_panel_lightmap_bake(void);
void  jce_editor_panel_audio_mixer(void);
void  jce_editor_panel_input_manager(void);
void  jce_editor_panel_curve_editor(void);
void  jce_editor_panel_animation_editor(void);
void  jce_panel_animation_editor_request_tab(int idx);
/* Load a (project-relative) .seq.json into the Sequencer panel — used by the
 * inspector's "Open in Sequencer" so selecting an entity binds the panel to its
 * SequencePlayer's sequence. */
void  jce_panel_sequencer_load_path(const char *path);
/* Open a .anim_sm.json in the State Machine tab / a .particles.json in the
 * particle editor tab — the inspector path fields' double-click preview. */
void  jce_panel_animator_sm_open_path(const char *path);
void  jce_panel_particle_editor_open_path(const char *path);
/* 0=Editor 1=StateMachine 2=Curves 3=Sequencer 4=Timeline 5=Rigging */
int   jce_panel_animation_editor_current_tab(void);
void  jce_editor_panel_animator_sm(void);
void  jce_editor_panel_sequencer(void);
/* Restore any component values the Sequencer panel's live preview touched
 * (idempotent / cheap when nothing is previewed).  Called before scene
 * save/load and when the Sequencer tab loses visibility. */
void  jce_panel_sequencer_preview_flush(void);
void  jce_editor_panel_navmesh(void);
/* World-streaming authoring: scene-level streamer config + chunk table +
 * live preview toggle (spawns chunk entities into the hierarchy). */
void  jce_editor_panel_world_streaming(void);
/* Read-only Behavior Tree visualizer (live Play status / edit-mode
 * structure preview via a lenient panel-owned JceBtContext). */
void  jce_editor_panel_bt_visualizer(void);
void  jce_editor_panel_bt_visualizer_content(void);
void  jce_editor_panel_terrain(void);
/* jce_editor_panel_preferences (legacy dockable Preferences) retired;
 * JCE_PANEL_USER_PREFERENCES (modal) is the single Preferences surface.
 * The enum ordinal JCE_PANEL_PREFERENCES stays frozen for the serialized
 * visibility bitmap. */
void  jce_editor_panel_user_preferences(void);
void  jce_editor_panel_package_manager(void);
void  jce_editor_panel_frame_debugger(void);
void  jce_editor_panel_vfx_graph(void);
void  jce_editor_panel_animation_rigging(void);
void  jce_editor_panel_render_pipeline_content(void);
void  jce_editor_panel_render_pipeline_tick(void);
void  jce_editor_panel_test_runner(void);
void  jce_editor_panel_build_profiles(void);
void  jce_editor_panel_toolbar(void);
/* Inline variant: draw the toolbar's contents at the current cursor
   position (no Begin/End, no SetNextWindowPos). Used by the DockSpace
   host to render the toolbar between the menu bar and the DockSpace,
   so the Window > Toolbar visibility toggle actually re-flows the
   layout instead of pinning a separate window underneath the host. */
void  jce_editor_panel_toolbar_inline(void);
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
/* Per-frame tick: detect panel open/close transitions and apply
 * preview enable/disable to the live engine pipeline. Must be called
 * every frame regardless of panel visibility. */
void  jce_editor_panel_postfx_tick(void);
void  jce_editor_panel_audio_mixer_content(void);
void  jce_editor_audio_mixer_focus_mixer_tab(void);
void  jce_editor_audio_mixer_focus_reverb_tab(void);
int   jce_editor_audio_mixer_current_tab(void);
void  jce_editor_panel_input_manager_content(void);
/* Resolve an Input Manager action's KEY bindings to ImGuiKey codes for
 * play-in-editor (live panel state — unsaved rebinds included). Returns
 * the number of keys written; 0 = action missing / no keyboard binds. */
int   jce_editor_input_action_keys(const char *name, int *out_imgui_keys, int max);
/* Top 6: synthesize the authored action map into a LIVE engine JceInputActions
 * from current ImGui key state each call, so editor-Play scripts read the same
 * data-driven actions (jce.is_action_down / get_axis) a shipped game does.
 * Keyboard + key-composite binds only (mouse/gamepad resolve in the shipped
 * game).  Returns a borrowed pointer owned by the Input Manager panel. */
struct JceInputActions;
const struct JceInputActions *jce_editor_input_actions_live(void);
/* SDL scancode → ImGuiKey (JCE_KEY_* values are SDL scancodes).  The
 * editor's single canonical translation table, implemented in
 * jce_editor.cpp; returns the ImGuiKey enum value, or 0 (ImGuiKey_None)
 * when the scancode is unmapped.  Do not add per-panel copies. */
int   jce_editor_scancode_to_imgui_key(int scancode);
void  jce_editor_panel_package_manager_content(void);
void  jce_editor_panel_frame_debugger_content(void);
/* Shared per-view GPU/CPU table (single implementation in the Profiling
 * workbench; self-refreshes when the CPU tab's sampler didn't run this
 * frame).  Used by the CPU tab and the Frame Debugger tab. */
void  jce_panel_profiler_draw_view_table(void);
void  jce_editor_panel_sprite_editor_content(void);
void  jce_editor_panel_tile_palette_content(void);
/* Open the Tile Palette on a specific map (+ optional sprites atlas):
   sets the panel's paths, loads the map, and makes the panel visible.
   Called by the Tilemap inspector's "Edit in Tile Palette" button. */
void  jce_editor_panel_tile_palette_edit(const char *tilemap_path,
                                         const char *sprites_path);
void  jce_editor_panel_vfx_graph_content(void);
void  jce_editor_panel_test_runner_content(void);
void  jce_editor_panel_build_profiles_content(void);
void  jce_editor_panel_profiler_content(void);
void  jce_editor_panel_memory_profiler_content(void);
void  jce_editor_panel_physics_debugger_content(void);
void  jce_editor_panel_light_explorer_content(void);
void  jce_editor_panel_reflection_probes_content(void);
void  jce_editor_panel_shader_graph_content(void);
void  jce_editor_panel_shader_graph(void);
void  jce_editor_panel_particle_editor_content(void);
void  jce_panel_material_graph_request_tab(int idx);
int   jce_panel_material_graph_current_tab(void);
void  jce_editor_panel_search_content(void);
void  jce_editor_panel_version_control_content(void);
void  jce_editor_panel_time_of_day_content(void);
void  jce_editor_panel_vcam_manager_content(void);
void  jce_editor_panel_reverb_zones_content(void);
void  jce_editor_panel_save_browser_content(void);
void  jce_editor_panel_user_guide_content(void);
/* Deep-link: select a guide chapter by index before focusing the panel. */
void  jce_editor_panel_user_guide_select_chapter(int chapter_index);
void  jce_editor_panel_bundle_browser_content(void);
void  jce_panel_bundle_browser_request_tab(int idx);
int   jce_panel_bundle_browser_current_tab(void);
void  jce_editor_panel_physics_layers(void);
void  jce_editor_panel_physics_layers_content(void);
void  jce_editor_panel_tags_layers(void);
void  jce_editor_panel_tags_layers_content(void);
void  jce_editor_panel_lighting_settings_content(void);
void  jce_panel_lighting_settings_request_tab(int idx);
int   jce_panel_lighting_settings_current_tab(void);
int   jce_panel_lighting_settings_current_inner_tab(void);
void  jce_editor_lighting_settings_focus_tab_time_of_day(void);
void  jce_editor_lighting_settings_focus_tab_light_explorer(void);
void  jce_editor_panel_lightmap_bake_content(void);
void  jce_editor_panel_reflection_probes(void);
void  jce_editor_panel_render_pipeline(void);
void  jce_editor_panel_build_report(void);
void  jce_editor_panel_build_report_content(void);
void  jce_panel_build_profiles_request_tab(int idx);
int   jce_panel_build_profiles_current_tab(void);
void  jce_editor_panel_systems_content(void);
void  jce_editor_panel_project_settings(void);
void  jce_editor_panel_project_settings_content(void);
void  jce_editor_panel_lan_discovery(void);
void  jce_editor_panel_lan_discovery_content(void);
void  jce_editor_panel_network_stats(void);
void  jce_editor_panel_network_stats_content(void);
void  jce_panel_network_stats_request_tab(int idx);
int   jce_panel_network_stats_current_tab(void);
void  jce_editor_panel_profile_analyzer(void);
void  jce_editor_panel_profile_analyzer_content(void);
void  jce_editor_panel_memory_profiler(void);
void  jce_panel_profiler_request_tab(int idx);
int   jce_panel_profiler_current_tab(void);

/* About dialog (modal). */
void  jce_editor_about_dialog(bool *p_open);

/* File viewer: open a file for preview. */
void  jce_file_viewer_open(const char *path);

/* Asset browser: set the real project root directory.
 * Clears any temporary browser-only simulated root. */
void  jce_editor_assets_set_project(const char *path);
/* Asset browser: returns the real project root (never NULL; may be "").
 * Browser-only simulated roots do not change this value. */
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
/* Canonicalize a path for storage in a component/scene field: anchors
 * CWD-relative inputs (e.g. the model importer's extracted embedded-
 * texture files) to an absolute path, then converts to the canonical
 * PROJECT-relative form.  Paths outside the project keep their absolute
 * form (the bundle packer virtualises those). */
void jce_editor_path_store_asset_ref(char *out, size_t out_size,
                                     const char *path);
/* Same as above but uses a caller-supplied base directory.  Useful when
 * the natural anchor is the scene file's directory rather than project root.
 */
void jce_editor_path_to_relative_to(char *out, size_t out_size,
                                    const char *abs_or_rel_path,
                                    const char *base_dir);

/* Common combo: normalize `src` into `buf` (project-relative) and return
 * either `buf` (success) or `src` (fallback when normalization yielded
 * an empty string). Saves the `rel[0] ? rel : src` ternary that
 * appears in every drag-drop accept path field.
 */
const char *jce_editor_path_relative_or(char *buf, size_t buf_size,
                                        const char *src);

/* Zero-copy view of the last path component (filename or final dir name)
 * inside `path` — returns a pointer into the input string after the last
 * '/' or '\\'.  If no separator, returns `path` itself.  Empty/NULL -> "".
 * Cheaper than jce_path_basename when you don't need a writable copy. */
const char *jce_editor_path_basename_view(const char *path);

/* In-place truncate `path` at its last '/' or '\\' separator, removing
 * the trailing component.  No-op (path becomes "") if no separator. */
void jce_editor_path_trim_to_parent(char *path);

/* In-place strip the final extension from `path` (e.g. "foo/bar.png" -> "foo/bar").
 * Only strips when the last '.' appears after the last path separator (so
 * dotfiles and "../foo" are safe).  No-op when no extension present. */
void jce_editor_path_strip_extension(char *path);

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

/* Console entries are ALSO written to jce_log (so they survive in the log
 * file) under this tag.  The Console panel's jce_log sink filters records
 * carrying it, so a message still appears exactly once on screen. */
extern const char *const kEditorConsoleLogTag;

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

/* Scene-view op: drop every selected entity straight down so the bottom of
   its world AABB rests on the first surface below it (other entities'
   AABBs, the terrain heightfield, or the Y=0 ground plane as fallback).
   One undo entry for the whole selection.  Hotkey: End (Edit / Snap To
   Ground); also in the Edit menu. */
void  jce_scene_view_snap_selection_to_ground(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_PANELS_H */
