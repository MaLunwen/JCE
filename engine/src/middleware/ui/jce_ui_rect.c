/*
 * jce_ui_rect.c  RectTransform anchor compute.
 */

#include <jce/middleware/ui/jce_ui_rect.h>

#include <math.h>

JceUIAnchors jce_ui_anchors_default(void)
{
    JceUIAnchors a = { {0.5f, 0.5f}, {0.5f, 0.5f},
                       {-50.0f, -50.0f}, {50.0f, 50.0f},
                       {0.5f, 0.5f} };
    return a;
}

/* Anchor positions are normalised — translate to absolute parent-space
 * coordinates and apply offsets to derive the child's corners. */
JceUIRect jce_ui_rect_compute(JceUIRect parent, JceUIAnchors a)
{
    /* Anchor points in absolute parent-space pixels. */
    float anchor_min_x = parent.x + parent.w * a.anchor_min[0];
    float anchor_min_y = parent.y + parent.h * a.anchor_min[1];
    float anchor_max_x = parent.x + parent.w * a.anchor_max[0];
    float anchor_max_y = parent.y + parent.h * a.anchor_max[1];

    /* Apply per-corner offsets.  Same formulation as Unity:
     * - top-left corner = anchor_min + offset_min
     * - bottom-right corner = anchor_max + offset_max
     * The min/max naming is preserved from Unity; in screen space with
     * Y-down (our convention) "min" still refers to the upper-left
     * anchor and "max" to the lower-right. */
    float min_x = anchor_min_x + a.offset_min[0];
    float min_y = anchor_min_y + a.offset_min[1];
    float max_x = anchor_max_x + a.offset_max[0];
    float max_y = anchor_max_y + a.offset_max[1];

    JceUIRect r;
    r.x = min_x;
    r.y = min_y;
    r.w = max_x - min_x;
    r.h = max_y - min_y;
    /* Negative dimensions can happen if offsets cross — clamp to 0
     * rather than propagate as a flipped rect. */
    if (r.w < 0.0f) r.w = 0.0f;
    if (r.h < 0.0f) r.h = 0.0f;
    return r;
}

JceUIRect jce_ui_rect_pixel_snap(JceUIRect r)
{
    JceUIRect o;
    o.x = floorf(r.x + 0.5f);
    o.y = floorf(r.y + 0.5f);
    /* Snap right edge separately to keep the right edge aligned even
     * if x and width round in opposite directions. */
    float right  = floorf(r.x + r.w + 0.5f);
    float bottom = floorf(r.y + r.h + 0.5f);
    o.w = right - o.x;
    o.h = bottom - o.y;
    if (o.w < 0.0f) o.w = 0.0f;
    if (o.h < 0.0f) o.h = 0.0f;
    return o;
}
