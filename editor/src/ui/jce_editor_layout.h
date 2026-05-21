/*
 * jce_editor_layout.h  DockSpace and menu bar layout.
 */

#ifndef JCE_EDITOR_LAYOUT_H
#define JCE_EDITOR_LAYOUT_H

#ifdef __cplusplus
extern "C" {
#endif

/* Draw the full editor layout (DockSpace + menu bar + panels). */
void jce_editor_layout_draw(void);

/* Request focus for Scene View window on next layout frame. */
void jce_editor_layout_request_focus_scene_view(void);

/* Request focus for Inspector window on next layout frame. */
void jce_editor_layout_request_focus_inspector(void);

/* Request focus for File Viewer window on next layout frame. */
void jce_editor_layout_request_focus_file_viewer(void);

/* Trigger the unsaved-changes dialog (e.g. when user tries to close the
   window).  Safe to call multiple times — only opens once. */
void jce_editor_layout_request_quit(void);

/* Returns true once the user has confirmed the quit action (either
   saved or chose "Don't Save" in the unsaved-changes dialog). */
bool jce_editor_layout_is_quit_confirmed(void);

/* Hot-reload all bgfx shader programs from disk (uses
   <dev_dir>/shaders, falls back to PAK per-shader).  dev_dir
   defaults to the JCE_SHADER_DEV_DIR env var; if unset, the
   editor uses the build output's shaders dir as inferred from
   the running executable.  Returns true on success. */
bool jce_editor_reload_shaders(void);

/* Apply a sensible centered-and-large default size+position to a panel window
   the very first time it is encountered (no effect once the user moves it,
   no effect if the panel is already docked). Call BEFORE the panel's
   ImGui::Begin(name, ...). Uses ImGuiCond_FirstUseEver so user drags,
   docking, and persisted imgui.ini state are always preserved. */
void jce_editor_panel_default_pose(const char *imgui_window_name);

/* Request a specific layout preset (0..9) be applied on the next frame.
   Mirrors the Window > Layout Presets menu entries and is the seam used
   by the Maya-style Workspace switcher (jce_workspace_set_active). Safe
   to call from any UI code path; the actual rebuild happens inside
   jce_editor_layout_draw(). preset_idx is clamped to [0,9]. */
void jce_editor_layout_request_preset(int preset_idx);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_LAYOUT_H */
