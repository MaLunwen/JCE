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

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_LAYOUT_H */
