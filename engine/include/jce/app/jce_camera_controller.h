/*
 * jce_camera_controller.h  FPS camera controller.
 *
 * Wraps a JceCamera with FPS-style movement: velocity + friction,
 * sprint toggle, mouse/touch look. Game code feeds raw input;
 * the controller updates the camera.
 *
 * Layer: Application (Layer 5).
 */

#ifndef JCE_CAMERA_CONTROLLER_H
#define JCE_CAMERA_CONTROLLER_H

#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceCamera          JceCamera;
typedef struct JceCameraController JceCameraController;

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

typedef struct JceCameraControllerDesc {
    float walk_speed;       /* units/s (default 3.0)  */
    float sprint_speed;     /* units/s (default 6.0)  */
    float accel_rate;       /* acceleration (default 20.0) */
    float friction_rate;    /* deceleration (default 14.0) */
    float mouse_sensitivity; /* rad/pixel (default 0.002) */
    float fov_normal;       /* degrees (default 60.0) */
    float fov_sprint;       /* degrees (default 70.0) */
    float fov_lerp_speed;   /* per-second (default 8.0) */
} JceCameraControllerDesc;

/* ================================================================== */
/* Input snapshot (filled by game each frame)                          */
/* ================================================================== */

typedef struct JceCameraInput {
    float move_forward;     /* -1 to 1: negative = forward */
    float move_right;       /* -1 to 1: positive = right   */
    float move_up;          /* -1 to 1: positive = up      */
    float look_yaw;         /* radians this frame           */
    float look_pitch;       /* radians this frame           */
    bool  sprint_toggle;    /* true on the frame sprint is toggled */
} JceCameraInput;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Create a controller bound to an existing camera.
   The camera is NOT owned  caller manages its lifetime.
   Pass NULL desc for defaults. */
JceCameraController *jce_camctrl_create(JceCamera *cam,
                                         const JceCameraControllerDesc *desc);

void jce_camctrl_destroy(JceCameraController *ctrl);

/* ================================================================== */
/* Per-frame                                                           */
/* ================================================================== */

/* Feed input and advance one frame (dt in seconds). */
void jce_camctrl_update(JceCameraController *ctrl,
                         const JceCameraInput *input, float dt);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

bool  jce_camctrl_is_sprinting(const JceCameraController *ctrl);
float jce_camctrl_get_current_fov(const JceCameraController *ctrl);

#ifdef __cplusplus
}
#endif

#endif /* JCE_CAMERA_CONTROLLER_H */
