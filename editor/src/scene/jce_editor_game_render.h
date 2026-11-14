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
#include <jce/application/jce_engine.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceRenderer JceRenderer;
typedef struct JceCamera   JceCamera;
typedef struct JcePakArchive JcePakArchive;
typedef struct JceRuntime  JceRuntime;
struct JceWindow;
struct JceScene;
struct JceInput;
struct JceInputActions;

/* Document input is prepared before the Play runtime consumes its sample. */
bool jce_editor_game_render_prepare_document_input(struct JceScene *scene,
    const struct JceInput *input, const struct JceInputActions *actions);
bool jce_editor_game_render_document_wants_input(void);
bool jce_editor_game_render_has_virtual_camera(void);
bool jce_editor_game_render_document_owns_free_pointer(void);

/* Set / clear the FPS-style relative-mouse capture on the editor window.
 * When `capture` is true, the OS cursor is hidden + locked, mouse motion
 * comes through as relative deltas — typical FPS feel.  Returns the new
 * state (false if the call had no effect or the window is unset). */
bool jce_editor_game_render_set_mouse_capture(bool capture);

/* Query current mouse-capture state. */
bool jce_editor_game_render_is_mouse_captured(void);
/* Apply the focused Play viewport's host-owned script cursor. */
bool jce_editor_game_render_apply_cursor(void);

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

/* Captured mouse-button state, fed by the editor event pump while the Game
 * View holds FPS capture (when ImGui no longer receives mouse buttons).  The
 * Game View reads this for gameplay clicks (e.g. melee on left-mouse) instead
 * of ImGui::IsKeyDown(ImGuiKey_MouseLeft), which sticks during capture.
 * btn: 0=left, 1=right, 2=middle, 3=x1, 4=x2. */
void jce_editor_game_render_push_mouse_button(int btn, bool down);
bool jce_editor_game_render_mouse_button(int btn);

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

/* Drop the game-view occlusion culler's per-entity slots.  Call when the
 * ECS world is destroyed+recreated (Play-stop snapshot restore). */
void jce_editor_game_render_reset_occlusion(void);

/* Feed the ECS-UI (Canvas) graphic raycaster the pointer state for the next
 * jce_editor_game_render_frame() call.  Coordinates are in panel/viewport-
 * local pixels (top-left origin), matching the rendered size.  `valid` should
 * be false when the cursor is outside the Game View (no hover/click). */
void jce_editor_game_render_set_ui_pointer(float x, float y,
                                           bool down, bool valid);

/* Owner-thread Play camera admission/release. Allocation failure refuses Play
 * before Runtime creation; end is idempotent and independent of viewport draws.
 * The stopped preview object remains untouched, including projection and roll. */
bool jce_editor_game_render_begin_play_camera(void);
void jce_editor_game_render_end_play_camera(void);

/* Hand the game renderer the live Play-mode runtime (NULL to detach on stop).
 * While set, canvas UI events drained after the overlay render are dispatched to
 * its gameplay script VM, so in-editor Play exercises UIButton on_click / widget
 * on_value_changed / InputField on_submit (not just visual feedback).  Called by
 * jce_editor_play.cpp on Play start / stop. */
void jce_editor_game_render_set_play_runtime(JceRuntime *rt);

/* Forward a UTF-8 text-input chunk / an editing key into the focused ECS-UI
 * InputField (single-line text entry).  The Game View panel sources these from
 * ImGui's per-frame character queue + key state — its natural event source,
 * mirroring how the pointer above is fed — only while the panel is hovered /
 * focused in Play.  Both are fire-and-forget: a no-op when no field is focused,
 * and they never consume events the rest of the editor needs.  OS text input
 * is started/stopped automatically inside jce_editor_game_render_frame() as
 * InputField focus changes. */
void jce_editor_game_render_text_input(const char *utf8);
void jce_editor_game_render_key_edit(int scancode, unsigned short mod);

/* Forward a mouse-wheel delta into the ECS-UI ScrollView under the pointer
 * (dy = +up, dx = +right; ImGui's io.MouseWheel / io.MouseWheelH).  The Game
 * View panel sources these from ImGui's IO — its natural event source, the same
 * place the pointer is fed — only while hovered in Play.  Fire-and-forget: a
 * no-op when no scroll view is hovered, and it does NOT consume the wheel (ImGui
 * still sees it).  Apply after the per-frame render so the canvas has resolved
 * this frame's hovered scroll view. */
void jce_editor_game_render_scroll(float dx, float dy);

/* Resolve requested presentation pixels through the current pipeline once.
 * No OS query. Pointer, runtime extent and Canvas use this resolved extent. */
void jce_editor_game_render_resolve_extent(uint32_t width, uint32_t height,
                                          uint32_t *out_w, uint32_t *out_h);

/* Optional current app.draw diagnostics, reset before every layout. The
 * copied public POD contains CPU observations only and retains no pointers. */
void jce_editor_game_render_observation_begin(bool enabled);
void jce_editor_game_render_observation_policy(uint32_t reason);
void jce_editor_game_render_observation_request(uint32_t width, uint32_t height);
void jce_editor_game_render_observation_image(uint16_t texture);
bool jce_editor_game_render_observation_copy(JceFrameViewportObservation *out);

/* Current Game bridge extent after this renderer frame's completed CPU draw.
 * Reads committed target metadata only, never a suggested extent or GPU timer.
 * Returns false/zero for stale, refused, uninitialized or missing targets. */
bool jce_editor_game_render_get_committed_extent(uint32_t *width, uint32_t *height);

/* Render one frame at the resolved Canvas/FBO size. The texture handle
 * returned by jce_editor_game_render_get_texture() is updated in-place. */
void jce_editor_game_render_frame(uint32_t width, uint32_t height);

/* bgfx texture handle index of the colour attachment, or UINT16_MAX
 * before the first frame. Use with ImGui::Image. */
uint16_t jce_editor_game_render_get_texture(void);

/* Capture the exact LDR texture displayed by Game View.  Submission is
 * asynchronous and shares the renderer's single readback queue; poll once per
 * editor frame until the return value is non-zero (-1 idle, 0 pending,
 * 1 written, 2 failed). */
typedef struct JceEditorGameScreenshotFrame {
    uint32_t source_frame;
    uint32_t submit_frame;
} JceEditorGameScreenshotFrame;

/* Optional frame receipt is written only after the readback is admitted. */
bool jce_editor_game_render_screenshot(
    const char *path, JceEditorGameScreenshotFrame *frame);
int jce_editor_game_render_capture_poll(void);

/* Borrow the current display camera for owner-thread movement/look: the Play
 * camera until Stop, otherwise stopped preview. Do not retain across Stop. */
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
