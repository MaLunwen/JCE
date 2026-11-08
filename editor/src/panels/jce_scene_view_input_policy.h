/*
 * jce_scene_view_input_policy.h  Pure Scene View pointer ownership rules.
 */

#ifndef JCE_SCENE_VIEW_INPUT_POLICY_H
#define JCE_SCENE_VIEW_INPUT_POLICY_H

/* Canonical draw/hit geometry for the viewport's navigation overlays. */
struct JceSceneViewOverlayRect { float x0, y0, x1, y1; };
struct JceSceneViewNavigationLayout {
    float center_x, center_y, radius;
    JceSceneViewOverlayRect bounds;
};
JceSceneViewNavigationLayout jce_scene_view_cube_layout(float x, float y, float width, float height);
JceSceneViewNavigationLayout jce_scene_view_axis_layout(float x, float y, float width, float height);
bool jce_scene_view_clip_overlay_rect(JceSceneViewOverlayRect *rect,
                                     float x, float y, float width, float height);

bool jce_scene_view_should_update_gizmo(bool navigation_hovered,
                                        bool navigation_active,
                                        bool gizmo_active);

bool jce_scene_view_should_start_selection(bool viewport_left_clicked,
                                           bool alt_held,
                                           bool gizmo_active,
                                           bool gizmo_axis_hovered);

bool jce_scene_view_left_input_belongs_to_viewport(bool viewport_left_clicked,
                                                   bool viewport_active,
                                                   bool mouse_left_down);

bool jce_scene_view_should_cancel_deferred_pick(bool mouse_left_clicked,
                                                bool viewport_left_clicked,
                                                bool viewport_active);

bool jce_scene_view_should_open_context_menu(bool viewport_right_clicked,
                                             bool alt_held);

#endif /* JCE_SCENE_VIEW_INPUT_POLICY_H */
