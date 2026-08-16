/* Camera-tool pose conversion. No editor state or UI dependencies. */

#include "jce_editor_camera_tools.h"

#include <cmath>

namespace {

constexpr float kDirectionEpsilon = 1.0e-6f;

bool finite_vec3(jce_vec3 v)
{
    return std::isfinite(v.x) && std::isfinite(v.y) && std::isfinite(v.z);
}

} /* namespace */

bool jce_editor_camera_tools_make_orbit(const JceEditorCameraPose &pose,
                                        float distance,
                                        JceEditorCameraOrbit *out_orbit)
{
    if (!out_orbit || !finite_vec3(pose.position)
        || !finite_vec3(pose.forward) || !std::isfinite(distance)
        || distance <= kDirectionEpsilon)
        return false;

    const float forward_len = jce_v3_len(pose.forward);
    if (forward_len <= kDirectionEpsilon)
        return false;

    const jce_vec3 forward = jce_v3_scale(pose.forward, 1.0f / forward_len);
    const jce_vec3 offset = jce_v3_scale(forward, -distance);
    const float y = fmaxf(-1.0f, fminf(1.0f, offset.y / distance));

    out_orbit->target = jce_v3_add(pose.position,
                                   jce_v3_scale(forward, distance));
    out_orbit->yaw = atan2f(offset.x, -offset.z);
    out_orbit->pitch = asinf(y);
    out_orbit->distance = distance;
    return true;
}

bool jce_editor_camera_tools_make_rotation(const JceEditorCameraPose &pose,
                                           jce_quat *out_rotation)
{
    if (!out_rotation || !finite_vec3(pose.forward) || !finite_vec3(pose.up))
        return false;

    const float forward_len = jce_v3_len(pose.forward);
    if (forward_len <= kDirectionEpsilon)
        return false;
    const jce_vec3 forward = jce_v3_scale(pose.forward, 1.0f / forward_len);

    jce_vec3 up = pose.up;
    if (jce_v3_len(up) <= kDirectionEpsilon)
        up = jce_v3(0.0f, 1.0f, 0.0f);
    else
        up = jce_v3_normalize(up);

    jce_vec3 right = jce_v3_cross(forward, up);
    if (jce_v3_len(right) <= kDirectionEpsilon) {
        up = fabsf(forward.y) < 0.99f
            ? jce_v3(0.0f, 1.0f, 0.0f)
            : jce_v3(1.0f, 0.0f, 0.0f);
        right = jce_v3_cross(forward, up);
    }
    right = jce_v3_normalize(right);
    up = jce_v3_normalize(jce_v3_cross(right, forward));

    jce_mat4 basis = jce_m4_identity();
    basis.col[0] = jce_v4(right.x, right.y, right.z, 0.0f);
    basis.col[1] = jce_v4(up.x, up.y, up.z, 0.0f);
    basis.col[2] = jce_v4(-forward.x, -forward.y, -forward.z, 0.0f);
    *out_rotation = jce_m4_to_quat(&basis);
    return true;
}
