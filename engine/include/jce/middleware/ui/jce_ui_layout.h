/*
 * jce_ui_layout.h  uGUI-style automatic layout groups.
 *
 * Three layout strategies matching Unity:
 *   - Horizontal Layout Group: arrange children left-to-right
 *   - Vertical   Layout Group: top-to-bottom
 *   - Grid       Layout Group: fixed cell size in a wrapping grid
 *
 * Inputs:
 *   - Parent rect (x, y, w, h) in pixels
 *   - Per-child preferred size (w, h)
 *   - Group parameters (padding, spacing, alignment)
 * Output:
 *   - Per-child computed rect (x, y, w, h)
 *
 * The layout is one-shot — caller invokes whenever any input changes.
 * No retained state.  Used by the future uGUI ECS bridge (B17 wire)
 * to drive RmlUi child positions.
 *
 * Layer: middleware/ui (Layer 4) — public.
 */

#ifndef JCE_UI_LAYOUT_H
#define JCE_UI_LAYOUT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_UI_LAYOUT_HORIZONTAL = 0,
    JCE_UI_LAYOUT_VERTICAL   = 1,
    JCE_UI_LAYOUT_GRID       = 2,
} JceUiLayoutKind;

typedef enum {
    JCE_UI_ALIGN_START  = 0,    /* left / top */
    JCE_UI_ALIGN_CENTER = 1,
    JCE_UI_ALIGN_END    = 2,    /* right / bottom */
    JCE_UI_ALIGN_STRETCH = 3,   /* fill perpendicular axis */
} JceUiAlign;

typedef struct {
    JceUiLayoutKind kind;
    float padding[4];           /* left, top, right, bottom (px) */
    float spacing;              /* gap between children (px) */
    JceUiAlign child_align;     /* perpendicular-axis alignment */
    /* Grid only — cell dimensions when kind == GRID. */
    float cell_w;
    float cell_h;
    bool  reverse;              /* reverse child order */
} JceUiLayoutGroup;

typedef struct {
    float pref_w;
    float pref_h;
    /* Flex factor for stretch-style sharing of leftover space. */
    float flex;
} JceUiChildSize;

typedef struct {
    float x, y, w, h;
} JceUiRect;

/* Compute child rects.  `parent` is the parent rect, `group` is the
 * layout spec, `children` is an array of `count` child preferred
 * sizes.  Output rects are written to `out`. */
JCE_API bool jce_ui_layout_compute(const JceUiRect        *parent,
                                    const JceUiLayoutGroup *group,
                                    const JceUiChildSize   *children,
                                    JceUiRect              *out,
                                    uint32_t                count);

JCE_EXTERN_C_END

#endif /* JCE_UI_LAYOUT_H */
