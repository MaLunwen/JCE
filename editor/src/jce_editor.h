/*
 * jce_editor.h  Editor overlay module (ImGui-based).
 *
 * Provides a dockable editor UI on top of the game viewport.
 * The editor is independent of game logic and can be toggled
 * with the F1 key at runtime.
 *
 * All functions are C-linkage so the C99 engine can call them.
 */

#ifndef JCE_EDITOR_H
#define JCE_EDITOR_H

#include <stdbool.h>
#include <stdint.h>
#include <SDL3/SDL_events.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct PakArchive PakArchive;
typedef struct JceWindow  JceWindow;

/* Initialize the editor subsystem.
   Creates ImGui context, loads fonts, sets up bgfx backend.
   Must be called after bgfx is initialized. */
bool jce_editor_init(const PakArchive *pak, JceWindow *window);

/* Shut down the editor and release all resources. */
void jce_editor_shutdown(void);

/* Process an SDL event for ImGui input.
   Returns true if ImGui consumed the event (game should skip it). */
bool jce_editor_process_event(const SDL_Event *event);

/* Run one editor frame: NewFrame -> draw panels -> Render -> bgfx submit.
   Call between jce_renderer_begin_frame and jce_renderer_end_frame. */
void jce_editor_update(JceWindow *window);

/* Query whether the editor overlay is currently visible. */
bool jce_editor_is_active(void);

/* Toggle editor visibility on/off. */
void jce_editor_toggle(void);

/* Get/set the current font size in pixels (range 12-48). */
float jce_editor_get_font_size(void);
bool  jce_editor_set_font_size(float size);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_H */
