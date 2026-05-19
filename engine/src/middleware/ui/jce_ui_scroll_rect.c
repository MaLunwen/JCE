/*
 * jce_ui_scroll_rect.c  ScrollRect + Mask + LayoutElement runtime.
 *
 * ScrollRect math
 *   - content_overflow = max(content - viewport, 0)
 *   - scroll position in [0,1] maps to a content-space offset of
 *     pos * content_overflow.
 *   - drag delta is normalised by content_overflow before adding
 *     to position; velocity tracks this delta over dt for inertia.
 *   - On release (dragging=false): velocity decays each frame by
 *     deceleration^dt; elastic mode rubber-bands position back to
 *     the [0,1] interval using `elasticity` as a snap rate.
 */

#include <jce/middleware/ui/jce_ui_scroll_rect.h>

#include <math.h>
#include <stddef.h>

static float overflow(float content, float viewport)
{
    float d = content - viewport;
    return d > 0.0f ? d : 0.0f;
}

void jce_ui_scroll_update(JceUIScrollRectComponent      *sr,
                            const JceUIScrollPointerState *p,
                            float                           dt)
{
    if (!sr) return;
    if (dt <= 0.0f) return;
    float of_x = overflow(sr->content_w, sr->viewport_w);
    float of_y = overflow(sr->content_h, sr->viewport_h);

    if (p && p->dragging) {
        if (sr->horizontal && of_x > 0.0f) {
            float dp = -p->drag_delta_x / of_x;
            sr->position_x   += dp;
            sr->velocity_x    = -p->drag_delta_x / dt;
        }
        if (sr->vertical && of_y > 0.0f) {
            float dp = -p->drag_delta_y / of_y;
            sr->position_y   += dp;
            sr->velocity_y    = -p->drag_delta_y / dt;
        }
    } else {
        /* Inertia decay. */
        if (sr->inertia) {
            float decay = sr->deceleration > 0.0f
                          ? powf(1.0f - sr->deceleration, dt) : 0.0f;
            sr->velocity_x *= decay;
            sr->velocity_y *= decay;
            if (sr->horizontal && of_x > 0.0f)
                sr->position_x += (sr->velocity_x * dt) / of_x;
            if (sr->vertical && of_y > 0.0f)
                sr->position_y += (sr->velocity_y * dt) / of_y;
        } else {
            sr->velocity_x = 0.0f;
            sr->velocity_y = 0.0f;
        }
    }

    /* Edge handling. */
    switch (sr->movement) {
    case JCE_UI_SCROLL_CLAMPED: {
        if (sr->position_x < 0) sr->position_x = 0;
        if (sr->position_x > 1) sr->position_x = 1;
        if (sr->position_y < 0) sr->position_y = 0;
        if (sr->position_y > 1) sr->position_y = 1;
        break;
    }
    case JCE_UI_SCROLL_ELASTIC: {
        float rate = sr->elasticity > 0.0f ? sr->elasticity : 5.0f;
        if (p && p->dragging) break;
        if (sr->position_x < 0)
            sr->position_x += (0.0f - sr->position_x) * (1.0f - expf(-rate * dt));
        else if (sr->position_x > 1)
            sr->position_x += (1.0f - sr->position_x) * (1.0f - expf(-rate * dt));
        if (sr->position_y < 0)
            sr->position_y += (0.0f - sr->position_y) * (1.0f - expf(-rate * dt));
        else if (sr->position_y > 1)
            sr->position_y += (1.0f - sr->position_y) * (1.0f - expf(-rate * dt));
        break;
    }
    case JCE_UI_SCROLL_UNRESTRICTED:
    default:
        break;
    }
}

bool jce_ui_mask_contains(const JceUIRectMask2DComponent *m,
                            const float rect[4], float x, float y)
{
    if (!m || !m->enabled || !rect) return true;
    float lx = rect[0] + m->padding[0];
    float ty = rect[1] + m->padding[1];
    float rx = rect[0] + rect[2] - m->padding[2];
    float by = rect[1] + rect[3] - m->padding[3];
    return (x >= lx && x <= rx && y >= ty && y <= by);
}

static float max_f(float a, float b) { return a > b ? a : b; }

void jce_ui_layout_element_apply(const JceUILayoutElementComponent *e,
                                   float *io_min_w, float *io_min_h,
                                   float *io_pref_w, float *io_pref_h,
                                   float *io_flex_w, float *io_flex_h)
{
    if (!e || e->ignore_layout) return;
    if (io_min_w && e->min_width  > 0) *io_min_w  = max_f(*io_min_w,  e->min_width);
    if (io_min_h && e->min_height > 0) *io_min_h  = max_f(*io_min_h,  e->min_height);
    if (io_pref_w && e->preferred_width  > 0) *io_pref_w = max_f(*io_pref_w, e->preferred_width);
    if (io_pref_h && e->preferred_height > 0) *io_pref_h = max_f(*io_pref_h, e->preferred_height);
    if (io_flex_w && e->flexible_width  > 0) *io_flex_w = e->flexible_width;
    if (io_flex_h && e->flexible_height > 0) *io_flex_h = e->flexible_height;
}
