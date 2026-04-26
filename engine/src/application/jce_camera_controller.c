/*
 * jce_camera_controller.c  FPS camera controller implementation.
 */

#include <jce/application/jce_camera_controller.h>
#include <jce/renderer/jce_camera.h>
#include "os/core/jce_memory.h"
#include <math.h>

/* ================================================================== */
/* Internals                                                           */
/* ================================================================== */

struct JceCameraController {
    JceCamera *cam;

    /* Config */
    float walk_speed;
    float sprint_speed;
    float accel_rate;
    float friction_rate;
    float mouse_sensitivity;
    float fov_normal;
    float fov_sprint;
    float fov_lerp_speed;

    /* State */
    float vel_fwd;
    float vel_right;
    float vel_up;
    bool  sprinting;
    float fov_current;
};

/* Exponential damping toward target value. */
static float damp_to(float current, float target, float rate, float dt)
{
    float t = 1.0f - expf(-rate * dt);
    if (t > 1.0f) t = 1.0f;
    return current + (target - current) * t;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceCameraController *jce_camctrl_create(JceCamera *cam,
                                         const JceCameraControllerDesc *desc)
{
    if (!cam) return NULL;

    JceCameraController *c = JCE_NEW(JceCameraController);
    if (!c) return NULL;

    c->cam = cam;

    /* Apply desc or defaults. */
    if (desc) {
        c->walk_speed       = desc->walk_speed   > 0 ? desc->walk_speed   : 3.0f;
        c->sprint_speed     = desc->sprint_speed > 0 ? desc->sprint_speed : 6.0f;
        c->accel_rate       = desc->accel_rate   > 0 ? desc->accel_rate   : 20.0f;
        c->friction_rate    = desc->friction_rate> 0 ? desc->friction_rate: 14.0f;
        c->mouse_sensitivity= desc->mouse_sensitivity > 0 ? desc->mouse_sensitivity : 0.002f;
        c->fov_normal       = desc->fov_normal   > 0 ? desc->fov_normal   : 60.0f;
        c->fov_sprint       = desc->fov_sprint   > 0 ? desc->fov_sprint   : 70.0f;
        c->fov_lerp_speed   = desc->fov_lerp_speed> 0 ? desc->fov_lerp_speed : 8.0f;
    } else {
        c->walk_speed        = 3.0f;
        c->sprint_speed      = 6.0f;
        c->accel_rate        = 20.0f;
        c->friction_rate     = 14.0f;
        c->mouse_sensitivity = 0.002f;
        c->fov_normal        = 60.0f;
        c->fov_sprint        = 70.0f;
        c->fov_lerp_speed    = 8.0f;
    }

    c->vel_fwd    = 0;
    c->vel_right  = 0;
    c->vel_up     = 0;
    c->sprinting  = false;
    c->fov_current = c->fov_normal;

    return c;
}

void jce_camctrl_destroy(JceCameraController *ctrl)
{
    JCE_FREE(ctrl);
}

/* ================================================================== */
/* Per-frame                                                           */
/* ================================================================== */

void jce_camctrl_update(JceCameraController *ctrl,
                         const JceCameraInput *input, float dt)
{
    if (!ctrl || !input || dt <= 0) return;

    /* Sprint toggle. */
    if (input->sprint_toggle)
        ctrl->sprinting = !ctrl->sprinting;

    /* Normalize movement to unit-length if diagonal exceeds 1. */
    float mf = input->move_forward;
    float mr = input->move_right;
    {
        float len_sq = mf * mf + mr * mr;
        if (len_sq > 1.0f) {
            jce_vec2 move = jce_v2_normalize(jce_v2(mf, mr));
            mf = move.x;
            mr = move.y;
        }
    }

    /* Minecraft: sprint cancels when not moving forward.
       move_forward < 0 = forward (W subtracts 1). */
    if (ctrl->sprinting && mf >= 0)
        ctrl->sprinting = false;

    /* Horizontal velocity with friction. */
    float max_speed = ctrl->sprinting ? ctrl->sprint_speed : ctrl->walk_speed;
    float target_fwd   = -mf * max_speed;
    float target_right = mr * max_speed;
    float horiz_input  = fabsf(mf) + fabsf(mr);
    float horiz_rate   = (horiz_input > 0.001f) ? ctrl->accel_rate : ctrl->friction_rate;

    ctrl->vel_fwd   = damp_to(ctrl->vel_fwd,   target_fwd,   horiz_rate, dt);
    ctrl->vel_right = damp_to(ctrl->vel_right, target_right, horiz_rate, dt);

    /* Vertical velocity. */
    float target_up = input->move_up * max_speed;
    float up_rate   = (fabsf(input->move_up) > 0.001f) ? ctrl->accel_rate : ctrl->friction_rate;
    ctrl->vel_up    = damp_to(ctrl->vel_up, target_up, up_rate, dt);

    /* Apply movement to camera. */
    if (fabsf(ctrl->vel_fwd) > 0.0001f)
        jce_camera_move_forward(ctrl->cam, ctrl->vel_fwd * dt);
    if (fabsf(ctrl->vel_right) > 0.0001f)
        jce_camera_move_right(ctrl->cam, ctrl->vel_right * dt);
    if (fabsf(ctrl->vel_up) > 0.0001f)
        jce_camera_move_up(ctrl->cam, ctrl->vel_up * dt);

    /* Look rotation. */
    float yaw   = input->look_yaw;
    float pitch = input->look_pitch;
    if (yaw != 0 || pitch != 0)
        jce_camera_rotate(ctrl->cam, yaw, pitch);

    /* FOV sprint effect. */
    float fov_target = ctrl->sprinting ? ctrl->fov_sprint : ctrl->fov_normal;
    ctrl->fov_current += (fov_target - ctrl->fov_current)
                       * fminf(dt * ctrl->fov_lerp_speed, 1.0f);
    jce_camera_set_fov(ctrl->cam, ctrl->fov_current);
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

bool jce_camctrl_is_sprinting(const JceCameraController *ctrl)
{
    return ctrl ? ctrl->sprinting : false;
}

float jce_camctrl_get_current_fov(const JceCameraController *ctrl)
{
    return ctrl ? ctrl->fov_current : 60.0f;
}
