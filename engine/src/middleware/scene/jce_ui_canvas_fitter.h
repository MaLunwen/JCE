/*
 * jce_ui_canvas_fitter.h -- ContentSizeFitter: an element sized to its content.
 *
 * WHY THIS IS A SEPARATE FILE.  jce_ui_canvas.c crossed AGENTS.md's 3000-line
 * cap the moment the fitter landed in it.  Raising the cap would have been
 * cheaper and wrong: the fitter is the newest code in the file and it answers
 * one self-contained question, so it is the code that has to leave.
 *
 * THE SEAM IS THREE VALUES, NOT UCFrame.  jce_ui_canvas_widgets.h records why
 * the widget state machines were NOT split -- they need UCFrame, "the canvas's
 * whole per-frame state, so the boundary would have been the file's interior
 * rather than a seam".  The fitter needs exactly the scene, the canvas (for
 * fonts) and the UI scale, so it takes those three and the boundary stays a
 * seam.  It costs three more accessors in the widgets header, which is what a
 * 130-line removal is worth here.
 */
#ifndef JCE_UI_CANVAS_FITTER_H
#define JCE_UI_CANVAS_FITTER_H

#include "jce_ui_canvas_widgets.h"

/* Resize `r` in place to the node's content, per axis.  True when anything
 * changed; false when the node has no (enabled) ContentSizeFitter, when both
 * axes are Unconstrained, or when nothing about the element can answer how
 * big its content is -- only a UIText or a LayoutGroup node can. */
bool uc_fit_rect(JceScene *s, JceUICanvas *uc, float ui_scale,
                 JceEntity node, UCRect *r);

/* What this node NEEDS, in draw pixels: a UIText's shaped extent, a
 * LayoutGroup node's packed extent, false when the node cannot say.
 *
 * The measurement is RECURSIVE and that is the point of it: a group asks each
 * child what it needs, and a child that is itself a label or a group answers
 * with its content rather than its authored rect.  Without that a column of
 * labels sizes itself to their rects and the text overflows -- which is the
 * bottom-up pass Unity has as ILayoutElement, Godot as get_minimum_size and
 * Slate as ComputeDesiredSize.
 *
 * Public to the module so the ARRANGE can consult it too, not just the
 * ContentSizeFitter: they are the same question asked at two moments. */
bool uc_preferred_size(JceScene *s, JceUICanvas *uc, float ui_scale,
                       JceEntity node, float *out_w, float *out_h);

#endif /* JCE_UI_CANVAS_FITTER_H */
