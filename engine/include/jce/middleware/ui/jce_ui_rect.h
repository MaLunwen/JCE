/*
 * jce_ui_rect.h  Unity-style RectTransform anchor / pivot math.
 *
 * Pure-CPU geometry: given a parent rect and a child's anchor settings
 * (anchor_min / anchor_max in [0,1] of parent), offsets, sizeDelta,
 * and pivot, compute the child's final on-screen rect.
 *
 * The RectTransform model:
 *   - anchor_min and anchor_max are normalised positions inside the
 *     parent rect.  When equal, the child has a fixed size and floats
 *     at that anchor.  When different, the child stretches between
 *     them.
 *   - offset_min / offset_max are applied to the anchor positions to
 *     give the final corners of the child rect.
 *   - pivot is the rotation/scale anchor inside the child rect (also
 *     in [0,1] of the child, used by transform composition; included
 *     here for completeness even though this helper only computes the
 *     rect).
 *
 * This is the same formulation Unity uses; expressing it as a flat
 * helper keeps it usable from both the engine UI runtime and editor
 * preview tools.
 *
 * Layer: middleware / ui (Layer 4) — public.
 */

#ifndef JCE_UI_RECT_H
#define JCE_UI_RECT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

/* Axis-aligned rectangle in pixels (or virtual pixels).  x/y is the
 * top-left corner; w/h are non-negative dimensions. */
#ifndef JCE_UI_RECT_TYPEDEF_DEFINED
#define JCE_UI_RECT_TYPEDEF_DEFINED
typedef struct {
    float x, y;
    float w, h;
} JceUIRect;
#endif

/* Anchored layout description for a child rect. */
typedef struct {
    /* Normalised anchor positions inside the parent rect. */
    float anchor_min[2];     /* default (0.5, 0.5) — centred */
    float anchor_max[2];     /* default (0.5, 0.5) */
    /* Offset from anchor_min / anchor_max in pixels.  When
     * anchor_min == anchor_max, offset_min is interpreted as the
     * top-left position relative to that anchor and offset_max as
     * size; otherwise both are positional offsets. */
    float offset_min[2];     /* default (-50, -50) */
    float offset_max[2];     /* default ( 50,  50) */
    /* Pivot in [0,1] inside the child rect (Unity default 0.5,0.5). */
    float pivot[2];
} JceUIAnchors;

/* Returns sensible defaults: centred anchor, 100×100 px, pivot 0.5. */
JCE_API JceUIAnchors jce_ui_anchors_default(void);

/* Compute the child rect from the parent rect + anchors.  The result
 * is well-defined for any anchor combination (corner-pinned, edge-
 * stretched, fully stretched).  Pivot does not affect the rect — it
 * only affects rotation/scale composition by other systems. */
JCE_API JceUIRect jce_ui_rect_compute(JceUIRect parent, JceUIAnchors a);

/* Convenience: snap a rect to integer pixel boundaries (for crisp
 * UI rendering on non-DPI-scaled targets). */
JCE_API JceUIRect jce_ui_rect_pixel_snap(JceUIRect r);

JCE_EXTERN_C_END

#endif /* JCE_UI_RECT_H */
