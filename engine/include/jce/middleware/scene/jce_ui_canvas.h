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
/* JceCanvasComponent is an anonymous `typedef struct { ... }`, so it
 * cannot be forward-declared; jce_ui_canvas_scale_for takes one by
 * pointer and this include is that function's honest cost. */
#include <jce/middleware/scene/jce_scene.h>

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

/* Fonts to fall back to when the UI font lacks a character, most specific
 * first, separated by ';' (e.g. "fonts/NotoSansSC.ttf;fonts/NotoEmoji.ttf").
 * NULL or "" clears the list.
 *
 * WHY THIS EXISTS.  A font that lacks a codepoint draws .notdef -- a tofu box
 * -- so a Latin UI face in front of a Chinese, Japanese or Korean string draws
 * a box for EVERY character, and pre-rendering the i18n codepoint set does not
 * help: the glyphs are not in the face.  Each fallback is opened at the same
 * size and through the same three-tier ladder as the primary (project asset
 * root, active VFS, canvas pak), and the shaper splits a mixed string into
 * runs so a fallback contributes its OWN advances rather than borrowed ones.
 *
 * Process-scoped, like the default font beside it and for the same reason: the
 * shipped drop-in main sets it while reading the project, before any canvas
 * exists.  Changing it invalidates the cached fonts. */
JCE_API void jce_ui_canvas_set_font_fallbacks(const char *paths_semicolon);

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

/* ── Editor queries: what is where, as of the LAST render ────────────
 *
 * The canvas resolves the top-most UI entity under a point every frame, by the
 * rules that actually govern UI hit-testing -- raycast_target, the inherited
 * CanvasGroup blocksRaycasts chain, canvas-group alpha, ScrollView clipping,
 * and an open dropdown's modal rect.  These expose that answer so a tool does
 * not have to reimplement it (and get it subtly different).
 *
 * The editor Scene View needs them because jce_scene_pick.c is a GPU
 * object-ID render over MESHES: a UIImage has no mesh, so the whole ECS-UI
 * tree was unselectable in the viewport.  An ID buffer also could not honour
 * any of the rules above even if the quads were added to it.
 *
 * Both answer about the last jce_ui_canvas_render call and are valid whether
 * or not a pointer was fed to it -- the hit list is built by the layout walk,
 * not by the pointer.  Coordinates are canvas space: the same screen_w /
 * screen_h that render was called with, top-left origin, +Y down.  Use
 * jce_ui_canvas_last_size to convert without keeping a second copy of them. */

/* Top-most UI entity whose recorded rect contains (x, y), or 0 for none. */
JCE_API uint64_t jce_ui_canvas_pick(const JceUICanvas *uc, float x, float y);

/* ── The draw-side clip log ──────────────────────────────────────────────
 *
 * Every clip change the LAST render made, in order: the ScrollView subtrees
 * it scissored to and the restores between them.  `active` false is "no
 * clip", which a rect alone cannot express — (0,0,0,0) is a real and
 * different answer, and those two are the states a clipping bug lands in.
 *
 * WHY IT EXISTS.  The draw clip is otherwise unobservable from outside.
 * jce_ui_canvas_entity_rect answers the RAYCAST clip, and the two used to
 * DISAGREE: the scissor was set to a ScrollView's own rect while the raycast
 * used the intersection with its parent, so a nested viewport clipped input
 * and pixels differently — and every test could see only the half that was
 * right.  Bounded per render; a tree deep enough to overflow simply stops
 * recording rather than growing.
 *
 * Count is 0 before the first render.  Indices are stable only until the next
 * one. */
JCE_API int  jce_ui_canvas_clip_log_count(const JceUICanvas *uc);
JCE_API bool jce_ui_canvas_clip_log_at(const JceUICanvas *uc, int i,
                                       float out_xywh[4], bool *out_active);

/* Where `entity` actually drew last frame, already clipped to any ancestor
 * ScrollView viewport.  false when it recorded no rect (not a UI element, not
 * under an enabled Canvas, fully clipped away, or not raycastable). */
JCE_API bool jce_ui_canvas_entity_rect(const JceUICanvas *uc, uint64_t entity,
                                       float *out_x, float *out_y,
                                       float *out_w, float *out_h);

/* The screen_w / screen_h of the last render.  0 before the first one. */
JCE_API void jce_ui_canvas_last_size(const JceUICanvas *uc,
                                     float *out_w, float *out_h);

/* The pivot of the RectTransform the canvas would USE for `entity` (UGUI
 * fractions, measured from the element's BOTTOM-left).  false when the entity
 * carries no UI component with a rect.
 *
 * The component resolution order behind this matters and is not obvious --
 * UIButton resolves LAST so a sibling UIImage still wins, which is what keeps
 * every button authored before UIButton had a rect from re-laying out.  A tool
 * that wants the pivot (the Scene View's pivot marker) asks here instead of
 * keeping a second copy of that order, which would drift.
 *
 * Returns the two floats rather than the struct so this header keeps its
 * forward declaration of JceScene and does not pull in jce_scene.h. */
JCE_API bool jce_ui_canvas_entity_pivot(JceScene *scene, uint64_t entity,
                                        float out_pivot2[2]);

/* THE SCALE THIS CANVAS RESOLVES TO at a given screen size.
 *
 * One derivation, exposed, because the renderer is not the only thing that
 * wants it: an editor that shows "current scale 1.23x" and a test that
 * asserts the match curve both have to get the SAME answer, and a second
 * copy of a formula is how two answers start.
 *
 * Returns 1.0 for a canvas that is not scaled at all -- World-space mode, or
 * a reference_resolution that was never authored -- which is exactly what the
 * renderer leaves ui_scale at in those cases.
 *
 * match_width_or_height is honoured as jce_scene.h describes it, INCLUDING
 * the exact-0.5 shortcut: sqrtf(rsx*rsy) rather than the pow() form, because
 * the two are not bit-identical and every scene written before that field
 * existed parses to 0.5. */
JCE_API float jce_ui_canvas_scale_for(const JceCanvasComponent *cv,
                                      float screen_w, float screen_h);

/* Tell the canvas which part of the drawable is safe to put controls in,
 * in the same PIXEL space as jce_ui_canvas_render's screen_w/screen_h.
 *
 * PUSHED IN RATHER THAN PULLED, because this is L4 middleware and the answer
 * lives in the platform layer: a canvas that called jce_window_get_safe_area
 * itself would put an os/platform dependency inside middleware/scene, which
 * check_layer_dependencies exists to refuse.  The host -- the runtime's frame
 * or the editor's Game View -- already knows its window and passes it on.
 *
 * Never set, or set to a zero-area rect, means "the whole drawable", so a host
 * that does not call this behaves exactly as before.  Only canvases with
 * respect_safe_area set read it. */
JCE_API void jce_ui_canvas_set_safe_area(JceUICanvas *uc, int x, int y,
                                         int w, int h);

/* THE RECT THIS CANVAS LAYS OUT IN, given a drawable and a safe area.
 *
 * One derivation, exposed, because the renderer is not its only reader: a
 * test has to be able to ask what a notch does to a canvas without standing
 * up a scene, and an editor device preview will want the same answer.  A
 * second copy of the rule is how two answers start -- this tree has paid for
 * that (a hinge axis disagreed with its own clamp by 22 degrees).
 *
 * Yields the full drawable unless the canvas opted in AND the safe rect has
 * positive area, which is exactly what the renderer does.  `cv` NULL yields
 * the full drawable too, so a caller never has to test for it. */
JCE_API void jce_ui_canvas_root_rect(const JceCanvasComponent *cv,
                                     float screen_w, float screen_h,
                                     int safe_x, int safe_y,
                                     int safe_w, int safe_h,
                                     float *out_x, float *out_y,
                                     float *out_w, float *out_h);

JCE_EXTERN_C_END

#endif /* JCE_UI_CANVAS_H */
