/*
 * jce_ui_canvas.h  ECS UI (Canvas/RectTransform/Image/Text/Button) renderer.
 *
 * Resolves the Unity-UGUI-shaped ECS UI components in a JceScene into
 * screen-space rectangles (layout pass), draws them as batched transient
 * quads + glyph atlas text (render pass), and hit-tests the pointer against
 * raycast-target rects to drive UIButton hover/press/click state.
 *
 * Render mode support: Screen-Space Overlay (the primary path).  World- and
 * camera-space canvases are laid out as overlays for now (see AGENTS notes).
 *
 * Layer: Middleware/Scene (Layer 4).  Built into the jce_scene layer next to
 * jce_scene_renderer.c because it needs both scene component access and the
 * renderer's low-level 2D draw path — the exact dependency set that file
 * already carries.
 *
 * OWNERSHIP (ADR-0002): this system owns SCENE UI — anything placed in a
 * scene, carried by a prefab, or addressed per entity from a gameplay script.
 * RmlUI (jce_ui.h) owns DOCUMENT UI — shell, settings, menus, anything that
 * wants flow/flex layout or a CSS cascade.  The split is structural, not a
 * preference: only this one has entities and serialization, and only that one
 * has a layout engine.
 *
 * This header used to describe RmlUI as "the shipping game-UI path", which
 * was wrong for everything the editor authors here.  The two must not
 * converge: no CSS/stylesheet/document concept belongs in this API, and no
 * JceEntity belongs in RmlUI's.
 */

#ifndef JCE_UI_CANVAS_H
#define JCE_UI_CANVAS_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceScene         JceScene;
typedef struct JceRenderer      JceRenderer;
typedef struct JcePakArchive    JcePakArchive;
typedef struct JceUICanvas      JceUICanvas;

/* Per-frame pointer state fed to the graphic raycaster. Coordinates are in
 * the SAME logical pixel space as screen_w/screen_h passed to render(). */
typedef struct {
    float x, y;          /* pointer position (logical px) */
    bool  down;          /* button currently held this frame */
    bool  valid;         /* false ⇒ no pointer (skip raycast/hover) */
} JceUIPointer;

/* Create a UI canvas renderer.
 *   renderer : engine renderer (low-level 2D draw path; texture binding).
 *              May be NULL for a HEADLESS canvas: layout + the graphic
 *              raycaster + the UIButton click state machine all still run
 *              (so jce_ui_canvas_last_clicked works), but every GPU draw is
 *              skipped.  Use this to drive click dispatch without a live bgfx
 *              context (gameplay polling / unit tests).
 *   pak      : PAK archive used to load fonts (e.g. "fonts/JCE.ttf") and
 *              sprite textures referenced by UIImage. May be NULL (text and
 *              sprites then fall back to solid quads).
 * Returns NULL on allocation failure. */
JCE_API JceUICanvas *jce_ui_canvas_create(JceRenderer         *renderer,
                                          const JcePakArchive *pak);

JCE_API void jce_ui_canvas_destroy(JceUICanvas *uc);

/* Process-global content root for canvas assets (project source-assets dir).
 * When set, UIText fonts resolve from `<root>/<fontPath>` on the host
 * filesystem FIRST, then fall back to the canvas pak — the editor points
 * this at the open project so project-authored fonts render without being
 * baked into the editor's embedded pak.  Pass NULL/"" to clear.  Changing
 * the root invalidates every canvas's font cache. */
JCE_API void jce_ui_canvas_set_asset_root(const char *root_dir);

/* App-scoped override for the fallback font used when a UIText leaves its
 * font_path empty.  Pass a canvas-relative path (e.g. "fonts/MyFont.ttf") to
 * override; pass NULL/"" to restore the built-in engine default.  Resolves via
 * the same asset-root / pak lookup as any explicit fontPath and invalidates
 * cached fonts so the change takes effect immediately.  Leaving it unset keeps
 * the editor and every other app on the built-in default. */
JCE_API void jce_ui_canvas_set_default_font(const char *font_path);

/* Render every Screen-Space-Overlay Canvas in `scene` into bgfx `view_id`.
 *
 * This function fully configures `view_id`: it binds the given framebuffer,
 * sets the view rect to (screen_w, screen_h), and sets a top-left-origin
 * orthographic projection so all UI coordinates are logical pixels with
 * (0,0) at the top-left — identical to the engine's JCE_VIEW_UI convention.
 *
 *   view_id    : a dedicated bgfx view id (must not collide with the scene
 *                renderer's shadow/post range for the same frame).
 *   fb_idx     : destination framebuffer handle index, or UINT16_MAX to draw
 *                into the backbuffer.  For an editor panel pass this is the
 *                offscreen target's framebuffer (jce_offscreen_target_get_
 *                frame_buffer); for a shipping game it is UINT16_MAX.
 *   screen_w/h : logical UI canvas size in pixels (drawable/panel size).
 *   pointer    : pointer state for the raycaster (NULL ⇒ no interaction).
 *                Coordinates are in the same logical px space as screen_w/h.
 *   dt_sec     : frame delta (drives UIButton colour fades).
 *
 * Layout, raycast and draw all happen here in one pass so callers only wire
 * a single call.  Safe to call every frame; transient buffers only. */
JCE_API void jce_ui_canvas_render(JceUICanvas        *uc,
                                  JceScene           *scene,
                                  uint16_t            view_id,
                                  uint16_t            fb_idx,
                                  float               screen_w,
                                  float               screen_h,
                                  const JceUIPointer *pointer,
                                  float               dt_sec);

/* Returns the entity id of the UIButton that was clicked (pressed AND
 * released over the same button) during the most recent render call, or 0 if
 * none.  Lets gameplay/editor poll button clicks without a callback. */
JCE_API uint64_t jce_ui_canvas_last_clicked(const JceUICanvas *uc);

/* Entity id of the interactive widget (UISlider / UIToggle / UIDropdown) whose
 * value the canvas CHANGED during the most recent render call, or 0 if none —
 * a slider drag that moved the value, a toggle flip, or a dropdown selection.
 * Like last_clicked it is set during render and cleared at the start of the
 * next render (read it after rendering, once per frame).  The caller looks up
 * the widget's authored on_value_changed handler and fires it through the
 * script VM (jce_runtime_dispatch_ui_value_changed). */
JCE_API uint64_t jce_ui_canvas_last_value_changed(const JceUICanvas *uc);

/* Entity id of the UIInputField whose `text` the canvas EDITED (insert/delete)
 * since the last call, or 0 if none.  Unlike last_value_changed this is set by
 * jce_ui_canvas_text_input / jce_ui_canvas_key_edit (which run OUTSIDE the
 * render call), so it is CLEARED ON READ rather than at render start — call it
 * exactly once per frame.  Drives a UIInputField's on_value_changed handler. */
JCE_API uint64_t jce_ui_canvas_last_text_changed(JceUICanvas *uc);

/* ── InputField (single-line text-entry) focus + edit channel ──────────
 *
 * The canvas tracks a single focused InputField entity (set by a pointer
 * click over the field's rect during render — see jce_ui_canvas_render) and a
 * byte caret index into that field's `text`.  The two delivery functions below
 * feed keyboard / text events the caller already receives from its window
 * event stream into the focused field; both are no-ops when nothing is focused
 * and never consume events the rest of the app needs (fire-and-forget).  All
 * focus / edit / caret logic runs on a HEADLESS canvas (renderer == NULL) so
 * gameplay polling and unit tests can drive it without a live bgfx context. */

/* Deliver UTF-8 text (e.g. JceTextInputEvent.text) to the focused field:
 * insert at the caret, honouring char_limit, the content_type filter and
 * read_only.  No-op if no field is focused. */
JCE_API void     jce_ui_canvas_text_input(JceUICanvas *uc, const char *utf8);

/* Deliver an editing key (JCE_KEY_*) to the focused field:
 *   BACKSPACE       delete the byte(s) before the caret
 *   DELETE          delete the byte(s) at the caret
 *   LEFT / RIGHT    move the caret one step
 *   HOME / END      caret to start / end
 *   RETURN/KP_ENTER commit  → jce_ui_canvas_last_submitted == entity
 *   ESCAPE          defocus (jce_ui_canvas_focused_input → 0)
 * `mod` is the JCE_KMOD_* bitset (reserved; unused for v1).  No-op if no field
 * is focused. */
JCE_API void     jce_ui_canvas_key_edit(JceUICanvas *uc, int scancode, uint16_t mod);

/* Entity id of the currently focused InputField, or 0 if none.  The CALLER
 * uses this (on a focus-state change) to start / stop OS text input. */
JCE_API uint64_t jce_ui_canvas_focused_input(const JceUICanvas *uc);

/* Entity whose InputField was submitted (RETURN/KP_ENTER) since the last call,
 * or 0 otherwise.  Set by jce_ui_canvas_key_edit (which runs OUTSIDE the render
 * call), so it is CLEARED ON READ — call it exactly once per frame.  Drives a
 * UIInputField's on_submit handler (jce_runtime_dispatch_ui_submit). */
JCE_API uint64_t jce_ui_canvas_last_submitted(JceUICanvas *uc);

/* ── ScrollView wheel/scroll channel ───────────────────────────────────
 *
 * Deliver a wheel/scroll delta (the caller feeds JCE_EVENT_MOUSE_WHEEL:
 * `dy` is +up, `dx` is +right) to the UIScrollView currently under the
 * pointer (the hovered scroll view tracked during the most recent render's
 * raycast pass).  Applies dy*scroll_sensitivity to the vertical axis and
 * dx*scroll_sensitivity to the horizontal axis, honouring the per-axis
 * enables + interactable, then clamps each axis to
 * [0, max(0, content_size - viewport_size)] and writes the result back into
 * jce_scene_get_ui_scroll_view(scene,e)->scroll_position.  No-op if no scroll
 * view is hovered.  Runs fully on a HEADLESS canvas (renderer == NULL) so
 * gameplay polling and unit tests can drive the scroll math without a live
 * bgfx context; fire-and-forget (never consumes wheel events other systems
 * need). */
JCE_API void     jce_ui_canvas_scroll(JceUICanvas *uc, float dx, float dy);

JCE_EXTERN_C_END

#endif /* JCE_UI_CANVAS_H */
