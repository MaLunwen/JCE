/*
 * jce_scene_camera_focus.h  Pure Scene View focus math.
 */

#ifndef JCE_SCENE_CAMERA_FOCUS_H
#define JCE_SCENE_CAMERA_FOCUS_H

#include <stdbool.h>
#include <jce/os/core/jce_math.h>

struct JceEditorSceneFocusTarget {
    jce_vec3 center;
    float    base_distance;
    float    distance;
    int      zoom_step;
};

struct JceEditorSceneFocusSample {
    jce_vec3 center;
    float    distance;
};

struct JceEditorSceneFocusAnim {
    bool     active;
    jce_vec3 from_center;
    jce_vec3 to_center;
    float    from_distance;
    float    to_distance;
    float    elapsed_sec;
    float    duration_sec;
};

JceEditorSceneFocusTarget
jce_editor_scene_focus_make_target(const float min3[3],
                                   const float max3[3],
                                   float fov_deg,
                                   int zoom_step);

bool jce_editor_scene_focus_same_bounds(const float a_min3[3],
                                        const float a_max3[3],
                                        const float b_min3[3],
                                        const float b_max3[3],
                                        float epsilon);

void jce_editor_scene_focus_transform_aabb(const float local_min3[3],
                                           const float local_max3[3],
                                           jce_vec3 position,
                                           jce_quat rotation,
                                           jce_vec3 scale,
                                           float out_min3[3],
                                           float out_max3[3]);

int jce_editor_scene_focus_next_zoom_step(bool same_bounds,
                                          int previous_step);

void jce_editor_scene_focus_anim_start(JceEditorSceneFocusAnim *anim,
                                       jce_vec3 from_center,
                                       float from_distance,
                                       jce_vec3 to_center,
                                       float to_distance,
                                       float duration_sec);

void jce_editor_scene_focus_anim_cancel(JceEditorSceneFocusAnim *anim);

JceEditorSceneFocusSample
jce_editor_scene_focus_anim_step(JceEditorSceneFocusAnim *anim,
                                 float dt_sec);

#endif /* JCE_SCENE_CAMERA_FOCUS_H */
