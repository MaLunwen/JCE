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
