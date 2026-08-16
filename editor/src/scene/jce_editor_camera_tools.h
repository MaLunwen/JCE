/* Pure camera-tool math shared by the Scene View and editor tests. */

#ifndef JCE_EDITOR_CAMERA_TOOLS_H
#define JCE_EDITOR_CAMERA_TOOLS_H

#include <jce/os/core/jce_math.h>

struct JceEditorCameraPose {
    jce_vec3 position;
    jce_vec3 forward;
    jce_vec3 up;
};

struct JceEditorCameraOrbit {
    jce_vec3 target;
    float yaw;
    float pitch;
    float distance;
};

bool jce_editor_camera_tools_make_orbit(const JceEditorCameraPose &pose,
                                        float distance,
                                        JceEditorCameraOrbit *out_orbit);

bool jce_editor_camera_tools_make_rotation(const JceEditorCameraPose &pose,
                                           jce_quat *out_rotation);

#endif /* JCE_EDITOR_CAMERA_TOOLS_H */
