/*
 * jce_scene_view_input_policy.cpp  Pure Scene View pointer ownership rules.
 */

#include "jce_scene_view_input_policy.h"

bool jce_scene_view_should_start_selection(bool viewport_left_clicked,
                                           bool alt_held,
                                           bool gizmo_active,
                                           bool gizmo_axis_hovered)
{
    return viewport_left_clicked
        && !alt_held
        && !gizmo_active
        && !gizmo_axis_hovered;
}

bool jce_scene_view_left_input_belongs_to_viewport(bool viewport_left_clicked,
                                                   bool viewport_active,
                                                   bool mouse_left_down)
{
    return mouse_left_down && (viewport_left_clicked || viewport_active);
}

bool jce_scene_view_should_cancel_deferred_pick(bool mouse_left_clicked,
                                                bool viewport_left_clicked,
                                                bool viewport_active)
{
    return mouse_left_clicked && !viewport_left_clicked && !viewport_active;
}

bool jce_scene_view_should_open_context_menu(bool viewport_right_clicked,
                                             bool alt_held)
{
    return viewport_right_clicked && !alt_held;
}

static float navigation_radius(float width, float height, float factor, float minimum, float maximum)
{
    float radius = (width < height ? width : height) * factor;
    if (radius < minimum) radius = minimum;
    if (radius > maximum) radius = maximum;
    return radius;
}

JceSceneViewNavigationLayout jce_scene_view_cube_layout(float x, float y, float width, float height)
{
    const float radius = navigation_radius(width, height, 0.06f, 28.0f, 50.0f);
    const float cx = x + width - 10.0f - radius - 5.0f;
    const float cy = y + 10.0f + radius + 5.0f;
    const float extent = radius + 10.0f;
    return {cx, cy, radius, {cx - extent, cy - extent, cx + extent, cy + extent}};
}

JceSceneViewNavigationLayout jce_scene_view_axis_layout(float x, float y, float width, float height)
{
    const float radius = navigation_radius(width, height, 0.04f, 20.0f, 45.0f);
    const float cx = x + 10.0f + radius + 5.0f;
    const float cy = y + height - 10.0f - radius - 5.0f;
    /* Axis tip hit discs extend twelve pixels beyond their center. */
    const float extent = radius + 12.0f;
    return {cx, cy, radius, {cx - extent, cy - extent, cx + extent, cy + extent}};
}

bool jce_scene_view_clip_overlay_rect(JceSceneViewOverlayRect *rect,
                                     float x, float y, float width, float height)
{
    if (!rect || width <= 0.0f || height <= 0.0f) return false;
    if (rect->x0 < x) rect->x0 = x;
    if (rect->y0 < y) rect->y0 = y;
    if (rect->x1 > x + width) rect->x1 = x + width;
    if (rect->y1 > y + height) rect->y1 = y + height;
    return rect->x1 > rect->x0 && rect->y1 > rect->y0;
}

bool jce_scene_view_should_update_gizmo(bool navigation_hovered,
                                        bool navigation_active,
                                        bool gizmo_active)
{
    return gizmo_active || (!navigation_hovered && !navigation_active);
}
