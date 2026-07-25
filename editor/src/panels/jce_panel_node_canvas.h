/*
 * jce_panel_node_canvas.h
 *
 * The pieces the node-graph panels genuinely draw the same way:
 *   - jce_node_canvas_draw_grid() — the scrolling background grid
 *     (Animator State Machine, VFX Graph, Material Graph).
 *   - jce_node_canvas_draw_link() — the left-to-right cubic bezier between
 *     two pins (VFX Graph, Material Graph).
 *
 * Deliberately NOT here: node appearance, pin semantics, link validation,
 * the selection model and the pan/zoom policy.  Those differ per graph on
 * purpose — the Animator zooms and hit-tests fixed-size state boxes and
 * joins them with arrows, the Material Graph marquee-selects a set of ids,
 * the VFX Graph has no view transform at all — so a canvas that tried to
 * serve all three would be worse than the duplication it removed.
 *
 * Header-only, like jce_panel_common.h: two short bodies, and the panels
 * are the only translation units that need them.
 */

#pragma once

#include <jce/tools/jce_imgui.hpp>

/* ── Background grid ──────────────────────────────────────────────── */

/*
 * Vertical + horizontal lines over the rect [origin, origin + size).
 * `offset` is where the first line of each axis sits inside the rect,
 * normally fmod(pan, step) so the grid slides with the view; it may be
 * negative, which places one line just outside the rect (the window clips
 * it).  Filling the background stays with the caller — the VFX canvas
 * inherits its child window's background and wants no fill at all.
 */
inline void jce_node_canvas_draw_grid(ImDrawList *dl, ImVec2 origin, ImVec2 size,
                                      float step, ImVec2 offset, ImU32 col)
{
    if (!(step > 0.0f))
        return;
    for (float x = offset.x; x < size.x; x += step)
        dl->AddLine(ImVec2(origin.x + x, origin.y),
                    ImVec2(origin.x + x, origin.y + size.y), col);
    for (float y = offset.y; y < size.y; y += step)
        dl->AddLine(ImVec2(origin.x, origin.y + y),
                    ImVec2(origin.x + size.x, origin.y + y), col);
}

/* ── Pin-to-pin link ──────────────────────────────────────────────── */

/*
 * Cubic bezier whose control points sit `curvature` pixels right of `a`
 * and left of `b`, so the curve leaves an output pin and enters an input
 * pin horizontally.  Also used for the rubber band while a link is being
 * dragged, with `b` at the mouse.
 */
inline void jce_node_canvas_draw_link(ImDrawList *dl, ImVec2 a, ImVec2 b,
                                      float curvature, ImU32 col, float thickness)
{
    dl->AddBezierCubic(a, ImVec2(a.x + curvature, a.y),
                       ImVec2(b.x - curvature, b.y), b, col, thickness);
}
