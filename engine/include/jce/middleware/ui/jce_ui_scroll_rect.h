/*
 * jce_ui_scroll_rect.h  uGUI ScrollRect + Mask + LayoutElement.
 *
 * Three Unity-parity additions to the uGUI scene-component set:
 *   - ScrollRect    : viewport that scrolls a larger content rect
 *                     with elastic/clamped/unrestricted edge modes
 *                     plus velocity + deceleration + inertia.
 *   - Mask          : rect clip that hides children outside its bounds
 *                     (alpha cutoff for soft / hard mask variants).
 *   - RectMask2D    : axis-aligned variant (cheaper than alpha mask).
 *   - LayoutElement : per-child overrides for LayoutGroup sizing
 *                     (min / preferred / flexible weight).
 *
 * Components are POD attached to entities (matches existing uGUI
 * component pattern from B18).  The scroll evaluator advances state
 * each frame given pointer-delta + viewport/content rects.
 *
 * Layer: middleware/ui (Layer 4) — public.
 */

#ifndef JCE_UI_SCROLL_RECT_H
#define JCE_UI_SCROLL_RECT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_UI_SCROLL_UNRESTRICTED = 0,
    JCE_UI_SCROLL_ELASTIC      = 1,   /* bounce-back outside bounds */
    JCE_UI_SCROLL_CLAMPED      = 2,   /* hard clamp */
} JceUIScrollMovement;

/* Scroll evaluator state — also exported via jce_scene.h as the
 * scene-attached component shape; guarded so co-inclusion is safe. */
#ifndef JCE_UI_SCROLL_RECT_TYPEDEFS_DEFINED
#define JCE_UI_SCROLL_RECT_TYPEDEFS_DEFINED
typedef struct {
    float viewport_w;
    float viewport_h;
    float content_w;
    float content_h;
    float position_x;
    float position_y;
    float velocity_x;
    float velocity_y;
    float deceleration;
    float elasticity;
    int   movement;            /* JceUIScrollMovement */
    bool  inertia;
    bool  horizontal;
    bool  vertical;
} JceUIScrollRectComponent;

typedef struct {
    bool  show_mask_graphic;
    float alpha_cutoff;
} JceUIMaskComponent;

typedef struct {
    float padding[4];
    bool  enabled;
} JceUIRectMask2DComponent;

typedef struct {
    float min_width;
    float min_height;
    float preferred_width;
    float preferred_height;
    float flexible_width;
    float flexible_height;
    int   layout_priority;
    bool  ignore_layout;
} JceUILayoutElementComponent;
#endif /* JCE_UI_SCROLL_RECT_TYPEDEFS_DEFINED */

/* ── Runtime ────────────────────────────────────────────────── */

/* Pointer input — caller fills with cursor delta + button state. */
typedef struct {
    float drag_delta_x;
    float drag_delta_y;
    bool  dragging;
} JceUIScrollPointerState;

/* Advance the scroll state by `dt`.  When `dragging`, set position
 * from cumulated delta and overwrite velocity = delta/dt.  When not
 * dragging, decay velocity by deceleration and (for elastic) snap
 * back toward [0,1]. */
JCE_API void jce_ui_scroll_update(JceUIScrollRectComponent      *sr,
                                    const JceUIScrollPointerState *pointer,
                                    float                           dt);

/* Hit-test: returns true when (x, y) (canvas-space pixels) lies
 * inside the mask's clip rect.  rect = (x, y, w, h). */
JCE_API bool jce_ui_mask_contains(const JceUIRectMask2DComponent *m,
                                    const float mask_rect[4],
                                    float test_x, float test_y);

/* Apply layout element overrides on top of caller's defaults. */
JCE_API void jce_ui_layout_element_apply(const JceUILayoutElementComponent *e,
                                           float *io_min_w, float *io_min_h,
                                           float *io_pref_w, float *io_pref_h,
                                           float *io_flex_w, float *io_flex_h);

JCE_EXTERN_C_END

#endif /* JCE_UI_SCROLL_RECT_H */
