/*
 * jce_editor_game_render.h  Embedded "Game View" rendering.
 *
 * The Game View panel renders the same JceScene as the Scene View, but
 * through its own free-fly FPS camera, into a dedicated off-screen FBO.
 * It reuses the engine `JceSceneRenderer` instance owned by the editor
 * scene render module so PBR / shadows / IBL / PostFX all behave the
 * same way they do in the main scene viewport.
 */

#ifndef JCE_EDITOR_GAME_RENDER_H
#define JCE_EDITOR_GAME_RENDER_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;
typedef struct JceCamera   JceCamera;
typedef struct JcePakArchive JcePakArchive;
struct JceWindow;

/* Set / clear the FPS-style relative-mouse capture on the editor window.
 * When `capture` is true, the OS cursor is hidden + locked, mouse motion
 * comes through as relative deltas — typical FPS feel.  Returns the new
 * state (false if the call had no effect or the window is unset). */
bool jce_editor_game_render_set_mouse_capture(bool capture);

/* Query current mouse-capture state. */
bool jce_editor_game_render_is_mouse_captured(void);

/* Warp the OS cursor to (x, y) in editor-window-local pixel coordinates.
 * Used by the Game View panel to keep the cursor pinned at the centre of
 * the viewport every frame so FPS look never loses input even if the OS
 * cursor would otherwise drift to another window or screen edge. */
void jce_editor_game_render_warp_cursor(int x, int y);

/* Confine the OS cursor to the rectangle (x, y, w, h) inside the editor
 * window so it cannot escape the embedded Game View panel.  Pass w<=0 or
 * h<=0 to remove the confinement. */
void jce_editor_game_render_set_cursor_rect(int x, int y, int w, int h);

/* Push a relative mouse motion delta from the platform layer into the
 * Game View panel.  Called from the editor event handler whenever an
 * SDL mouse motion event arrives (xrel/yrel) — these are accurate even
 * when SDL relative-mouse-mode pins the visible cursor at one position
 * and ImGui's io.MouseDelta is therefore zero. */
void jce_editor_game_render_push_mouse_delta(float xrel, float yrel);

/* Atomically read and clear the accumulated relative-mouse delta.
 * The Game View panel calls this every frame while FPS-captured to
 * drive yaw/pitch instead of relying on ImGui::IO::MouseDelta. */
void jce_editor_game_render_consume_mouse_delta(float *dx, float *dy);

/* Initialise the embedded game viewport renderer.
 * Must be called AFTER jce_editor_scene_render_init() so the engine
 * scene renderer is already available. The window is used for the
 * relative-mouse / cursor capture toggle (FPS fly-cam).
 * pak is used to load the post-processing shaders for the game view's
 * dedicated PostFX pipeline. */
bool jce_editor_game_render_init(JceRenderer *renderer, struct JceWindow *window,
                                 const JcePakArchive *pak);

/* Tear down. Safe to call even if init failed. */
void jce_editor_game_render_shutdown(void);

/* Feed the ECS-UI (Canvas) graphic raycaster the pointer state for the next
 * jce_editor_game_render_frame() call.  Coordinates are in panel/viewport-
 * local pixels (top-left origin), matching the rendered size.  `valid` should
 * be false when the cursor is outside the Game View (no hover/click). */
void jce_editor_game_render_set_ui_pointer(float x, float y,
                                           bool down, bool valid);

/* Render one frame at the requested viewport size. The texture handle
 * returned by jce_editor_game_render_get_texture() is updated in-place. */
void jce_editor_game_render_frame(uint32_t width, uint32_t height);

/* bgfx texture handle index of the colour attachment, or UINT16_MAX
 * before the first frame. Use with ImGui::Image. */
uint16_t jce_editor_game_render_get_texture(void);

/* Access the FPS fly-camera so the panel can drive movement / look. */
JceCamera *jce_editor_game_render_get_camera(void);

/* Reset the FPS camera to a sensible default (slightly above the origin
 * looking towards -Z). Called once at init and on user request. */
void jce_editor_game_render_reset_camera(void);

/* ── Game module integration (B-phase) ───────────────────────────── */

typedef struct JceAppDesc JceGameModule;

/* Select the active game module driven by Play state.  Pass NULL to
 * use the built-in default (pure ECS tick).  Switching modules while
 * Playing safely tears down the previous one. */
void jce_editor_game_render_set_module(const JceGameModule *mod);

/* Currently selected module (defaults to engine's default module). */
const JceGameModule *jce_editor_game_render_get_module(void);

#ifdef __cplusplus
}
#endif

#endif /* JCE_EDITOR_GAME_RENDER_H */
