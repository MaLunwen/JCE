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
 * already carries.  This is NOT RmlUI (the shipping HTML/CSS game-UI path)
 * and does not touch it.
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
 *   pak      : PAK archive used to load fonts (e.g. "fonts/JCE.ttf") and
 *              sprite textures referenced by UIImage. May be NULL (text and
 *              sprites then fall back to solid quads).
 * Returns NULL on allocation failure. */
JCE_API JceUICanvas *jce_ui_canvas_create(JceRenderer         *renderer,
                                          const JcePakArchive *pak);

JCE_API void jce_ui_canvas_destroy(JceUICanvas *uc);

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

JCE_EXTERN_C_END

#endif /* JCE_UI_CANVAS_H */
