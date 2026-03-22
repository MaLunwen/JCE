/*
 * jce_camera.h  3D/2D camera system.
 *
 * Supports perspective (3D) and orthographic (2D) projection modes.
 * Uses jce_math.h types for view/projection matrices.
 */

#ifndef JCE_CAMERA_H
#define JCE_CAMERA_H

#include "foundation/jce_math.h"
#include <stdbool.h>

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
JceCamera *jce_camera_create(const JceCameraDesc *desc);

/* Destroy a camera. */
void jce_camera_destroy(JceCamera *cam);

/* -- Getters -------------------------------------------------------- */

/* Get the view matrix (world  camera). */
jce_mat4 jce_camera_view(const JceCamera *cam);

/* Get the projection matrix. Requires aspect ratio for perspective mode. */
jce_mat4 jce_camera_proj(const JceCamera *cam, float aspect,
                          bool homogeneous_ndc);

jce_vec3       jce_camera_get_position(const JceCamera *cam);
jce_vec3       jce_camera_get_forward(const JceCamera *cam);
jce_vec3       jce_camera_get_right(const JceCamera *cam);
jce_vec3       jce_camera_get_up(const JceCamera *cam);
float          jce_camera_get_fov(const JceCamera *cam);
float          jce_camera_get_near(const JceCamera *cam);
float          jce_camera_get_far(const JceCamera *cam);
JceCameraMode  jce_camera_get_mode(const JceCamera *cam);

/* -- Setters / Manipulation ---------------------------------------- */

void jce_camera_set_position(JceCamera *cam, jce_vec3 pos);
void jce_camera_set_target(JceCamera *cam, jce_vec3 target);
void jce_camera_set_fov(JceCamera *cam, float fov_deg);
void jce_camera_set_near_far(JceCamera *cam, float near_plane, float far_plane);
void jce_camera_set_ortho_size(JceCamera *cam, float w, float h);
void jce_camera_set_mode(JceCamera *cam, JceCameraMode mode);

/* Move relative to camera orientation (FPS-style). */
void jce_camera_move_forward(JceCamera *cam, float distance);
void jce_camera_move_right(JceCamera *cam, float distance);
void jce_camera_move_up(JceCamera *cam, float distance);

/* Rotate by yaw/pitch (radians). Clamps pitch to 89. */
void jce_camera_rotate(JceCamera *cam, float yaw_rad, float pitch_rad);

/* Look at a specific target from the current position. */
void jce_camera_look_at(JceCamera *cam, jce_vec3 target);

#endif /* JCE_CAMERA_H */
