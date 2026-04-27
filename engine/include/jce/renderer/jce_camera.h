/*
 * jce_camera.h  3D/2D camera system.
 *
 * Supports perspective (3D) and orthographic (2D) projection modes.
 * Uses jce_math.h types for view/projection matrices.
 */

#ifndef JCE_CAMERA_H
#define JCE_CAMERA_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef struct JceCamera JceCamera;

/* Projection mode. */
typedef enum {
    JCE_CAMERA_PERSPECTIVE,
    JCE_CAMERA_ORTHO
} JceCameraMode;

/* Camera creation parameters. */
typedef struct {
    JceCameraMode mode;
    jce_vec3      position;
    jce_vec3      target;       /* look-at target (perspective mode) */
    jce_vec3      up;           /* world up vector, default (0,1,0) */
    float         fov_deg;      /* vertical FOV in degrees (perspective) */
    float         near_plane;
    float         far_plane;
    float         ortho_w;      /* ortho width  (ortho mode) */
    float         ortho_h;      /* ortho height (ortho mode) */
} JceCameraDesc;

/* Create a camera from a descriptor. Returns NULL on failure. */
JCE_API JceCamera *jce_camera_create(const JceCameraDesc *desc);

/* Destroy a camera. */
JCE_API void jce_camera_destroy(JceCamera *cam);

/* -- Getters -------------------------------------------------------- */

/* Get the view matrix (world  camera). */
JCE_API jce_mat4 jce_camera_view(const JceCamera *cam);

/* Get the projection matrix. Requires aspect ratio for perspective mode. */
jce_mat4 jce_camera_proj(const JceCamera *cam, float aspect,
                          bool homogeneous_ndc);

JCE_API jce_vec3       jce_camera_get_position(const JceCamera *cam);
JCE_API jce_vec3       jce_camera_get_forward(const JceCamera *cam);
JCE_API jce_vec3       jce_camera_get_right(const JceCamera *cam);
JCE_API jce_vec3       jce_camera_get_up(const JceCamera *cam);
JCE_API float          jce_camera_get_fov(const JceCamera *cam);
JCE_API float          jce_camera_get_near(const JceCamera *cam);
JCE_API float          jce_camera_get_far(const JceCamera *cam);
JCE_API JceCameraMode  jce_camera_get_mode(const JceCamera *cam);

/* -- Setters / Manipulation ---------------------------------------- */

JCE_API void jce_camera_set_position(JceCamera *cam, jce_vec3 pos);
JCE_API void jce_camera_set_target(JceCamera *cam, jce_vec3 target);
JCE_API void jce_camera_set_fov(JceCamera *cam, float fov_deg);
JCE_API void jce_camera_set_near_far(JceCamera *cam, float near_plane, float far_plane);
JCE_API void jce_camera_set_ortho_size(JceCamera *cam, float w, float h);
JCE_API void jce_camera_set_mode(JceCamera *cam, JceCameraMode mode);

/* Move relative to camera orientation (FPS-style). */
JCE_API void jce_camera_move_forward(JceCamera *cam, float distance);
JCE_API void jce_camera_move_right(JceCamera *cam, float distance);
JCE_API void jce_camera_move_up(JceCamera *cam, float distance);

/* Rotate by yaw/pitch (radians). Clamps pitch to 89. */
JCE_API void jce_camera_rotate(JceCamera *cam, float yaw_rad, float pitch_rad);

/* Look at a specific target from the current position. */
JCE_API void jce_camera_look_at(JceCamera *cam, jce_vec3 target);

JCE_EXTERN_C_END

#endif /* JCE_CAMERA_H */
