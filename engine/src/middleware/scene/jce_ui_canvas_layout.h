/*
 * jce_ui_canvas_layout.h -- LayoutGroup: where a node's children go.
 *
 * WHY THIS IS A SEPARATE FILE.  jce_ui_canvas.c crossed AGENTS.md's 3000-line
 * cap the moment weighted per-child sizing and the grid constraint landed in
 * it -- the same thing that happened when the ContentSizeFitter landed, and
 * jce_ui_canvas_fitter.h records the answer: raising the cap would have been
 * cheaper and wrong, so the newest self-contained question is the code that
 * leaves.
 *
 * THE SEAM IS FIVE VALUES, NOT UCFrame.  jce_ui_canvas_widgets.h says why the
 * widget state machines were NOT split -- they need UCFrame, "the canvas's
 * whole per-frame state, so the boundary would have been the file's interior
 * rather than a seam".  The arrangement needs the scene, the parent rect, the
 * component, the children and the UI scale.  Nothing else, so the boundary
 * stays a seam.
 */
#ifndef JCE_UI_CANVAS_LAYOUT_H
#define JCE_UI_CANVAS_LAYOUT_H

#include "jce_ui_canvas_widgets.h"

/* Overwrite `child_rects` with the packed positions a LayoutGroup on `parent`
 * gives its UI children.  `children` and `child_rects` are parallel and
 * `child_count` long; a child whose LayoutElement sets ignore_layout keeps the
 * rect it came in with. */
/* `uc` may be NULL, and then the arrange falls back to each child's authored
 * rect exactly as it did before content measurement existed -- which is what a
 * headless caller with no font cache gets, and what makes that path keep
 * working rather than silently sizing everything to zero. */
void uc_apply_layout_group(JceScene *s, JceUICanvas *uc,
                           const UCRect *parent_rect,
                           const JceLayoutGroupComponent *lg,
                           JceEntity *children, int child_count,
                           UCRect *child_rects, float scale);

#endif /* JCE_UI_CANVAS_LAYOUT_H */
