/*
 * jce_camera.c  Camera system implementation.
 */

#include <jce/renderer/jce_camera.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <stdlib.h>   /* getenv - JCE_LOG_CAMERA */
#include <jce/os/core/jce_log.h>

#ifndef LOG_TAG
#define LOG_TAG "camera"
#endif

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

    JceCamera *cam = (JceCamera *)JCE_CALLOC(1, sizeof(*cam));
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
    JCE_FREE(cam);
}

/* -- Matrices ------------------------------------------------------- */

jce_mat4 jce_camera_view(const JceCamera *cam)
{
    if (!cam) return jce_m4_identity();

    jce_vec3 forward = calc_forward(cam->yaw, cam->pitch);
    jce_vec3 target  = jce_v3_add(cam->position, forward);
    return jce_m4_look_at(cam->position, target, cam->up_world);
}

/* What this camera is about to project WITH, logged on change.
 *
 * Set JCE_LOG_CAMERA=1 to get it.  Level gated for the same reason the ambient
 * line in jce_sr_draw.c is: JCE_DIST compiles LOG_INFO out and every SDK
 * consumer exe is built with JCE_DIST=1, so an INFO line would be readable in
 * the editor and silent in the shipped game -- mute in exactly the half of an
 * editor-vs-runtime comparison that it exists to make.
 *
 * Why it is worth a permanent line: two hosts rendering one scene must build
 * the same projection, and nothing else makes the inputs observable.  A
 * difference here reads, in a screenshot diff, as "the editor draws the board
 * 2.4% shorter" -- a number with no mechanism attached, and indistinguishable
 * by eye from a camera-position or a viewport bug. */
static void cam_log_projection(const JceCamera *cam, float aspect,
                               bool homogeneous_ndc, float hw, float hh)
{
    static int loud = -1;
    static float last[11];
    static int last_mode = -1;
    static bool last_hd;
    if (loud < 0) {
        const char *v = getenv("JCE_LOG_CAMERA");
        loud = (v && v[0] && v[0] != '0') ? 1 : 0;
    }
    /* The POSE is part of this line, not a separate one.  A projection whose
     * every number matches can still put the picture somewhere else, and a
     * rigid screen-space shift with an identical scale is exactly what a
     * camera-position difference looks like -- measured once as "the editor
     * draws the same board 3 px higher" with no other number disagreeing. */
    const float now[11] = { aspect, hw, hh, cam->near_plane, cam->far_plane,
                            cam->fov_rad, cam->position.x, cam->position.y,
                            cam->position.z, cam->yaw, cam->pitch };
    bool same = (last_mode == (int)cam->mode) && (last_hd == homogeneous_ndc);
    for (int i = 0; same && i < 11; ++i)
        same = (last[i] == now[i]);
    if (same)
        return;
    last_mode = (int)cam->mode;
    last_hd   = homogeneous_ndc;
    for (int i = 0; i < 11; ++i)
        last[i] = now[i];
    if (loud)
        LOG_WARN(LOG_TAG,
                 "camera proj: mode=%s aspect=%.6f half=(%.4f, %.4f) "
                 "near=%.4f far=%.4f fov_rad=%.6f homogeneous_ndc=%d "
                 "pos=(%.5f, %.5f, %.5f) yaw=%.6f pitch=%.6f",
                 cam->mode == JCE_CAMERA_ORTHO ? "ortho" : "persp",
                 (double)aspect, (double)hw, (double)hh,
                 (double)cam->near_plane, (double)cam->far_plane,
                 (double)cam->fov_rad, homogeneous_ndc ? 1 : 0,
                 (double)cam->position.x, (double)cam->position.y,
                 (double)cam->position.z, (double)cam->yaw,
                 (double)cam->pitch);
    else
        LOG_INFO(LOG_TAG,
                 "camera proj: mode=%s aspect=%.6f half=(%.4f, %.4f) "
                 "near=%.4f far=%.4f fov_rad=%.6f homogeneous_ndc=%d "
                 "pos=(%.5f, %.5f, %.5f) yaw=%.6f pitch=%.6f",
                 cam->mode == JCE_CAMERA_ORTHO ? "ortho" : "persp",
                 (double)aspect, (double)hw, (double)hh,
                 (double)cam->near_plane, (double)cam->far_plane,
                 (double)cam->fov_rad, homogeneous_ndc ? 1 : 0,
                 (double)cam->position.x, (double)cam->position.y,
                 (double)cam->position.z, (double)cam->yaw,
                 (double)cam->pitch);
}

jce_mat4 jce_camera_proj(const JceCamera *cam, float aspect,
                          bool homogeneous_ndc)
{
    if (!cam) return jce_m4_identity();

    if (cam->mode == JCE_CAMERA_ORTHO) {
        /* ortho_w <= 0 spells "follow the viewport": the caller supplied a
         * world-unit HEIGHT and wants this frame's aspect to decide the
         * width.  Callers that set both keep their exact box. */
        float hh = cam->ortho_h * 0.5f;
        float hw = (cam->ortho_w > 0.0f)
                 ? cam->ortho_w * 0.5f
                 : hh * (aspect > 0.0f ? aspect : 1.0f);
        cam_log_projection(cam, aspect, homogeneous_ndc, hw, hh);
        return jce_m4_ortho(-hw, hw, -hh, hh,
                            cam->near_plane, cam->far_plane, homogeneous_ndc);
    }

    cam_log_projection(cam, aspect, homogeneous_ndc, 0.0f, 0.0f);
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

bool jce_camera_set_pose(JceCamera *cam, jce_vec3 position,
                         jce_vec3 forward, jce_vec3 up)
{
    const float epsilon = 1.0e-6f;
    jce_vec3 ortho_up;

    if (!cam || !isfinite(position.x) || !isfinite(position.y) ||
        !isfinite(position.z) || !isfinite(forward.x) ||
        !isfinite(forward.y) || !isfinite(forward.z) ||
        !isfinite(up.x) || !isfinite(up.y) || !isfinite(up.z) ||
        jce_v3_len(forward) <= epsilon || jce_v3_len(up) <= epsilon)
        return false;

    forward = jce_v3_normalize(forward);
    ortho_up = jce_v3_sub(up, jce_v3_scale(forward,
        jce_v3_dot(up, forward)));
    if (jce_v3_len(ortho_up) <= epsilon)
        return false;
    ortho_up = jce_v3_normalize(ortho_up);

    cam->position = position;
    cam->up_world = ortho_up;
    cam->yaw = atan2f(forward.x, -forward.z);
    cam->pitch = asinf(fmaxf(-1.0f, fminf(1.0f, forward.y)));
    if (cam->pitch > MAX_PITCH) cam->pitch = MAX_PITCH;
    if (cam->pitch < -MAX_PITCH) cam->pitch = -MAX_PITCH;
    return true;
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

void jce_camera_set_ortho_height(JceCamera *cam, float h)
{
    if (!cam || h <= 0.0f) return;
    cam->ortho_h = h;
    /* Zero is the sentinel jce_camera_proj reads as "derive from aspect".
     * Cleared here rather than left alone so a camera that was given an
     * explicit box earlier does not keep that width against a new height. */
    cam->ortho_w = 0.0f;
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

void jce_camera_third_person_follow(JceCamera *cam, jce_vec3 target)
{
    if (!cam) return;
    /* Orbit direction = the camera's current horizontal forward, so mouse-look
     * (which changes yaw) rotates the rig around the character. */
    jce_vec3 fwd  = jce_camera_get_forward(cam);
    float    hlen = sqrtf(fwd.x * fwd.x + fwd.z * fwd.z);
    jce_vec3 hf   = (hlen > 1e-4f) ? jce_v3(fwd.x / hlen, 0.0f, fwd.z / hlen)
                                   : jce_v3(0.0f, 0.0f, -1.0f);
    jce_vec3 cam_pos  = jce_v3(target.x - hf.x * 7.0f,
                              target.y + 3.0f,
                              target.z - hf.z * 7.0f);
    jce_vec3 look_tgt = jce_v3(target.x + hf.x * 14.0f,
                              target.y + 1.6f,
                              target.z + hf.z * 14.0f);
    jce_camera_set_position(cam, cam_pos);
    jce_camera_look_at(cam, look_tgt);
}
