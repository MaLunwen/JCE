/*
 * jce_camera.c  Camera system implementation.
 */

#include "jce_camera.h"
#include <SDL3/SDL.h>   /* SDL_calloc / SDL_free */
#include <math.h>

/* Max pitch to avoid gimbal lock at poles. */
#define MAX_PITCH (89.0f * JCE_DEG2RAD)

struct JceCamera {
    JceCameraMode mode;
    jce_vec3      position;
    jce_vec3      up_world;     /* world up (usually 0,1,0) */

    /* Euler angles for FPS-style control (perspective). */
    float         yaw;          /* radians, 0 = looking down -Z */
    float         pitch;        /* radians */

    /* Perspective params. */
    float         fov_rad;
    float         near_plane;
    float         far_plane;

    /* Ortho params. */
    float         ortho_w;
    float         ortho_h;
};

/* Compute forward direction from yaw/pitch. */
static jce_vec3 calc_forward(float yaw, float pitch)
{
    return jce_v3_normalize(jce_v3(
        sinf(yaw) * cosf(pitch),
        sinf(pitch),
       -cosf(yaw) * cosf(pitch)
    ));
}

/* -- Lifecycle ------------------------------------------------------ */

JceCamera *jce_camera_create(const JceCameraDesc *desc)
{
    if (!desc) return NULL;

    JceCamera *cam = (JceCamera *)SDL_calloc(1, sizeof(*cam));
    if (!cam) return NULL;

    cam->mode       = desc->mode;
    cam->position   = desc->position;
    cam->up_world   = (desc->up.x == 0 && desc->up.y == 0 && desc->up.z == 0)
                    ? jce_v3(0, 1, 0) : desc->up;
    cam->fov_rad    = desc->fov_deg > 0 ? desc->fov_deg * JCE_DEG2RAD : 60.0f * JCE_DEG2RAD;
    cam->near_plane = desc->near_plane > 0 ? desc->near_plane : 0.1f;
    cam->far_plane  = desc->far_plane > 0 ? desc->far_plane : 1000.0f;
    cam->ortho_w    = desc->ortho_w > 0 ? desc->ortho_w : 800.0f;
    cam->ortho_h    = desc->ortho_h > 0 ? desc->ortho_h : 600.0f;

    /* Compute initial yaw/pitch from position  target direction. */
    jce_vec3 dir = jce_v3_normalize(jce_v3_sub(desc->target, desc->position));
    if (jce_v3_len(dir) < 1e-6f)
        dir = jce_v3(0, 0, -1); /* default: looking down -Z */

    cam->yaw   = atan2f(dir.x, -dir.z);
    cam->pitch = asinf(dir.y);
    if (cam->pitch >  MAX_PITCH) cam->pitch =  MAX_PITCH;
    if (cam->pitch < -MAX_PITCH) cam->pitch = -MAX_PITCH;

    return cam;
}

void jce_camera_destroy(JceCamera *cam)
{
    SDL_free(cam);
}

/* -- Matrices ------------------------------------------------------- */

jce_mat4 jce_camera_view(const JceCamera *cam)
{
    if (!cam) return jce_m4_identity();

    jce_vec3 forward = calc_forward(cam->yaw, cam->pitch);
    jce_vec3 target  = jce_v3_add(cam->position, forward);
    return jce_m4_look_at(cam->position, target, cam->up_world);
}

jce_mat4 jce_camera_proj(const JceCamera *cam, float aspect,
                          bool homogeneous_ndc)
{
    if (!cam) return jce_m4_identity();

    if (cam->mode == JCE_CAMERA_ORTHO) {
        float hw = cam->ortho_w * 0.5f;
        float hh = cam->ortho_h * 0.5f;
        return jce_m4_ortho(-hw, hw, -hh, hh,
                            cam->near_plane, cam->far_plane, homogeneous_ndc);
    }

    return jce_m4_perspective(cam->fov_rad, aspect,
                              cam->near_plane, cam->far_plane, homogeneous_ndc);
}

/* -- Getters -------------------------------------------------------- */

jce_vec3 jce_camera_get_position(const JceCamera *cam)
{
    return cam ? cam->position : jce_v3(0, 0, 0);
}

jce_vec3 jce_camera_get_forward(const JceCamera *cam)
{
    return cam ? calc_forward(cam->yaw, cam->pitch) : jce_v3(0, 0, -1);
}

jce_vec3 jce_camera_get_right(const JceCamera *cam)
{
    if (!cam) return jce_v3(1, 0, 0);
    jce_vec3 fwd = calc_forward(cam->yaw, cam->pitch);
    return jce_v3_normalize(jce_v3_cross(fwd, cam->up_world));
}

jce_vec3 jce_camera_get_up(const JceCamera *cam)
{
    if (!cam) return jce_v3(0, 1, 0);
    jce_vec3 fwd   = calc_forward(cam->yaw, cam->pitch);
    jce_vec3 right = jce_v3_normalize(jce_v3_cross(fwd, cam->up_world));
    return jce_v3_cross(right, fwd);
}

float jce_camera_get_fov(const JceCamera *cam)
{
    return cam ? cam->fov_rad * JCE_RAD2DEG : 60.0f;
}

float jce_camera_get_near(const JceCamera *cam)
{
    return cam ? cam->near_plane : 0.1f;
}

float jce_camera_get_far(const JceCamera *cam)
{
    return cam ? cam->far_plane : 1000.0f;
}

JceCameraMode jce_camera_get_mode(const JceCamera *cam)
{
    return cam ? cam->mode : JCE_CAMERA_PERSPECTIVE;
}

/* -- Setters -------------------------------------------------------- */

void jce_camera_set_position(JceCamera *cam, jce_vec3 pos)
{
    if (cam) cam->position = pos;
}

void jce_camera_set_target(JceCamera *cam, jce_vec3 target)
{
    if (!cam) return;
    jce_camera_look_at(cam, target);
}

void jce_camera_set_fov(JceCamera *cam, float fov_deg)
{
    if (cam && fov_deg > 0)
        cam->fov_rad = fov_deg * JCE_DEG2RAD;
}

void jce_camera_set_near_far(JceCamera *cam, float near_plane, float far_plane)
{
    if (!cam) return;
    if (near_plane > 0) cam->near_plane = near_plane;
    if (far_plane > 0)  cam->far_plane  = far_plane;
}

void jce_camera_set_ortho_size(JceCamera *cam, float w, float h)
{
    if (!cam) return;
    if (w > 0) cam->ortho_w = w;
    if (h > 0) cam->ortho_h = h;
}

void jce_camera_set_mode(JceCamera *cam, JceCameraMode mode)
{
    if (cam) cam->mode = mode;
}

/* -- Movement ------------------------------------------------------- */

void jce_camera_move_forward(JceCamera *cam, float distance)
{
    if (!cam) return;
    jce_vec3 fwd = calc_forward(cam->yaw, cam->pitch);
    cam->position = jce_v3_add(cam->position, jce_v3_scale(fwd, distance));
}

void jce_camera_move_right(JceCamera *cam, float distance)
{
    if (!cam) return;
    jce_vec3 fwd   = calc_forward(cam->yaw, cam->pitch);
    jce_vec3 right = jce_v3_normalize(jce_v3_cross(fwd, cam->up_world));
    cam->position = jce_v3_add(cam->position, jce_v3_scale(right, distance));
}

void jce_camera_move_up(JceCamera *cam, float distance)
{
    if (!cam) return;
    cam->position = jce_v3_add(cam->position,
                               jce_v3_scale(cam->up_world, distance));
}

void jce_camera_rotate(JceCamera *cam, float yaw_rad, float pitch_rad)
{
    if (!cam) return;
    cam->yaw   += yaw_rad;
    cam->pitch += pitch_rad;
    if (cam->pitch >  MAX_PITCH) cam->pitch =  MAX_PITCH;
    if (cam->pitch < -MAX_PITCH) cam->pitch = -MAX_PITCH;
}

void jce_camera_look_at(JceCamera *cam, jce_vec3 target)
{
    if (!cam) return;
    jce_vec3 dir = jce_v3_normalize(jce_v3_sub(target, cam->position));
    if (jce_v3_len(dir) < 1e-6f) return;

    cam->yaw   = atan2f(dir.x, -dir.z);
    cam->pitch = asinf(dir.y);
    if (cam->pitch >  MAX_PITCH) cam->pitch =  MAX_PITCH;
    if (cam->pitch < -MAX_PITCH) cam->pitch = -MAX_PITCH;
}
