/*
 * jce_scene_camera_focus.cpp  Unity-style Scene View focus helper.
 */

#include "scene/jce_scene_camera_focus.h"

#include <math.h>
#include <string.h>

#define FOCUS_MIN_RADIUS      0.5f
#define FOCUS_MARGIN          1.15f
#define FOCUS_DISTANCE_MIN    0.001f
#define FOCUS_DISTANCE_MAX    100000.0f
#define FOCUS_ZOOM_STEP_COUNT 2

static float focus_clampf(float v, float lo, float hi)
{
    if (v < lo) return lo;
    if (v > hi) return hi;
    return v;
}

static float focus_zoom_scale(int step)
{
    static const float scales[FOCUS_ZOOM_STEP_COUNT] = {
        0.65f, 2.0f,
    };

    if (step < 0) step = 0;
    step %= FOCUS_ZOOM_STEP_COUNT;
    return scales[step];
}

static float focus_ease(float t)
{
    t = focus_clampf(t, 0.0f, 1.0f);
    return t * t * (3.0f - 2.0f * t);
}

JceEditorSceneFocusTarget
jce_editor_scene_focus_make_target(const float min3[3],
                                   const float max3[3],
                                   float fov_deg,
                                   int zoom_step)
{
    JceEditorSceneFocusTarget out;
    memset(&out, 0, sizeof(out));

    if (!min3 || !max3) {
        out.base_distance = 1.0f;
        out.distance = 1.0f;
        return out;
    }

    float cx = (min3[0] + max3[0]) * 0.5f;
    float cy = (min3[1] + max3[1]) * 0.5f;
    float cz = (min3[2] + max3[2]) * 0.5f;

    float ex = fabsf(max3[0] - min3[0]) * 0.5f;
    float ey = fabsf(max3[1] - min3[1]) * 0.5f;
    float ez = fabsf(max3[2] - min3[2]) * 0.5f;
    float radius = sqrtf(ex * ex + ey * ey + ez * ez);
    if (radius < FOCUS_MIN_RADIUS) radius = FOCUS_MIN_RADIUS;

    if (fov_deg < 1.0f) fov_deg = 45.0f;
    float fov_y = fov_deg * JCE_DEG2RAD;
    float base = radius / sinf(fov_y * 0.5f);
    base *= FOCUS_MARGIN;
    base = focus_clampf(base, FOCUS_DISTANCE_MIN, FOCUS_DISTANCE_MAX);

    if (zoom_step < 0) zoom_step = 0;
    zoom_step %= FOCUS_ZOOM_STEP_COUNT;

    out.center = jce_v3(cx, cy, cz);
    out.base_distance = base;
    out.distance = focus_clampf(base * focus_zoom_scale(zoom_step),
                                FOCUS_DISTANCE_MIN,
                                FOCUS_DISTANCE_MAX);
    out.zoom_step = zoom_step;
    return out;
}

bool jce_editor_scene_focus_same_bounds(const float a_min3[3],
                                        const float a_max3[3],
                                        const float b_min3[3],
                                        const float b_max3[3],
                                        float epsilon)
{
    if (!a_min3 || !a_max3 || !b_min3 || !b_max3) return false;
    if (epsilon < 0.0f) epsilon = -epsilon;

    for (int i = 0; i < 3; i++) {
        if (fabsf(a_min3[i] - b_min3[i]) > epsilon) return false;
        if (fabsf(a_max3[i] - b_max3[i]) > epsilon) return false;
    }
    return true;
}

void jce_editor_scene_focus_transform_aabb(const float local_min3[3],
                                           const float local_max3[3],
                                           jce_vec3 position,
                                           jce_quat rotation,
                                           jce_vec3 scale,
                                           float out_min3[3],
                                           float out_max3[3])
{
    if (!out_min3 || !out_max3) return;

    out_min3[0] = out_min3[1] = out_min3[2] = 0.0f;
    out_max3[0] = out_max3[1] = out_max3[2] = 0.0f;
    if (!local_min3 || !local_max3) return;

    jce_mat4 model = jce_m4_from_trs(position, rotation, scale);
    bool first = true;

    for (int xi = 0; xi < 2; xi++) {
        for (int yi = 0; yi < 2; yi++) {
            for (int zi = 0; zi < 2; zi++) {
                jce_vec4 p = jce_v4(xi ? local_max3[0] : local_min3[0],
                                    yi ? local_max3[1] : local_min3[1],
                                    zi ? local_max3[2] : local_min3[2],
                                    1.0f);
                jce_vec4 w = jce_m4_mul_v4(&model, p);

                if (first) {
                    out_min3[0] = out_max3[0] = w.x;
                    out_min3[1] = out_max3[1] = w.y;
                    out_min3[2] = out_max3[2] = w.z;
                    first = false;
                } else {
                    if (w.x < out_min3[0]) out_min3[0] = w.x;
                    if (w.y < out_min3[1]) out_min3[1] = w.y;
                    if (w.z < out_min3[2]) out_min3[2] = w.z;
                    if (w.x > out_max3[0]) out_max3[0] = w.x;
                    if (w.y > out_max3[1]) out_max3[1] = w.y;
                    if (w.z > out_max3[2]) out_max3[2] = w.z;
                }
            }
        }
    }
}

int jce_editor_scene_focus_next_zoom_step(bool same_bounds,
                                          int previous_step)
{
    if (!same_bounds) return 0;
    if (previous_step < 0) previous_step = 0;
    previous_step %= FOCUS_ZOOM_STEP_COUNT;
    return previous_step == 0 ? 1 : 0;
}

void jce_editor_scene_focus_anim_start(JceEditorSceneFocusAnim *anim,
                                       jce_vec3 from_center,
                                       float from_distance,
                                       jce_vec3 to_center,
                                       float to_distance,
                                       float duration_sec)
{
    if (!anim) return;

    anim->active = duration_sec > 0.0001f;
    anim->from_center = from_center;
    anim->to_center = to_center;
    anim->from_distance = from_distance;
    anim->to_distance = to_distance;
    anim->elapsed_sec = 0.0f;
    anim->duration_sec = duration_sec > 0.0001f ? duration_sec : 0.0001f;
}

void jce_editor_scene_focus_anim_cancel(JceEditorSceneFocusAnim *anim)
{
    if (anim) anim->active = false;
}

JceEditorSceneFocusSample
jce_editor_scene_focus_anim_step(JceEditorSceneFocusAnim *anim,
                                 float dt_sec)
{
    JceEditorSceneFocusSample out;
    memset(&out, 0, sizeof(out));

    if (!anim) return out;
    if (dt_sec < 0.0f) dt_sec = 0.0f;

    if (anim->active)
        anim->elapsed_sec += dt_sec;

    float t = anim->elapsed_sec / anim->duration_sec;
    if (t >= 1.0f) {
        t = 1.0f;
        anim->active = false;
    }

    float e = focus_ease(t);
    out.center = jce_v3_lerp(anim->from_center, anim->to_center, e);
    out.distance = anim->from_distance
                 + (anim->to_distance - anim->from_distance) * e;
    return out;
}
