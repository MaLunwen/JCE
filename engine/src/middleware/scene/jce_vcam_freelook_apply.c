/*
 * jce_vcam_freelook_apply.c  FreeLook + Composer + Hard Lookat.
 *
 * FreeLook rig math:
 *   - vertical_axis ∈ [-1, +1] interpolates between bottom orbit
 *     (-1), mid (0), top (+1).
 *   - horizontal_axis is yaw, radians, around target's up axis.
 *   - radius + height = quadratic Bezier through the three (radius,
 *     height) control points: top, mid, bottom.
 *
 * Composer:
 *   - Project target onto screen-space via cam_pos + cam_forward.
 *   - If outside dead zone but inside soft zone, lerp toward centre
 *     proportional to how far into the soft zone we are.
 *   - Outside soft zone: hard-snap (clamp into soft-zone edge).
 *
 * No bgfx; no platform input — caller feeds normalised deltas.
 */

#include <jce/middleware/scene/jce_vcam_freelook_apply.h>

#include <math.h>
#include <string.h>

/* Quadratic Bezier sample at t ∈ [0,1] over (p0, p1, p2). */
static float qbez(float p0, float p1, float p2, float t)
{
    float omt = 1.0f - t;
    return omt * omt * p0 + 2.0f * omt * t * p1 + t * t * p2;
}

static void freelook_compute_pos(const JceVcamFreeLookRig *rig,
                                   const float target[3],
                                   float out_pos[3])
{
    /* Map vertical_axis [-1, +1] → t in [0, 1]. */
    float t = (rig->vertical_axis + 1.0f) * 0.5f;
    if (t < 0) t = 0;
    if (t > 1) t = 1;

    /* Bezier interpolates radius + height across (bottom, mid, top). */
    float r = qbez(rig->orbit_radius[2], rig->orbit_radius[1],
                    rig->orbit_radius[0], t);
    float h = qbez(rig->orbit_height[2], rig->orbit_height[1],
                    rig->orbit_height[0], t);

    /* Orbit around target on the XZ plane, height adds Y. */
    float yaw = rig->horizontal_axis;
    out_pos[0] = target[0] + r * sinf(yaw);
    out_pos[1] = target[1] + h;
    out_pos[2] = target[2] + r * cosf(yaw);
}

void jce_vcam_freelook_apply(JceVirtualCamera *vcam,
                                const JceVcamFreeLookInput *in,
                                JceVcamFreeLookOutput *out)
{
    if (!vcam || !in || !out) return;
    if (vcam->rig == JCE_VCAM_RIG_BASIC) {
        /* No-op; fall back to caller's standard evaluation. */
        memcpy(out->position, vcam->position, sizeof(out->position));
        memcpy(out->target,   vcam->target,   sizeof(out->target));
        out->fov_deg = vcam->fov_deg;
        return;
    }

    JceVcamFreeLookRig *rig = &vcam->freelook;
    rig->horizontal_axis += in->yaw_input  * rig->horizontal_speed * in->dt;
    rig->vertical_axis   += in->pitch_input * rig->vertical_speed   * in->dt;
    /* Clamp vertical_axis to [-1, +1]. */
    if (rig->vertical_axis < -1.0f) rig->vertical_axis = -1.0f;
    if (rig->vertical_axis >  1.0f) rig->vertical_axis =  1.0f;

    /* FreeLook orbits the look_at point. */
    const float *target = vcam->look_at_pos;
    freelook_compute_pos(rig, target, out->position);

    /* Composer / hard lookat. */
    float fwd[3] = { target[0] - out->position[0],
                     target[1] - out->position[1],
                     target[2] - out->position[2] };
    float L = sqrtf(fwd[0]*fwd[0] + fwd[1]*fwd[1] + fwd[2]*fwd[2]);
    if (L > 1e-6f) { fwd[0]/=L; fwd[1]/=L; fwd[2]/=L; }
    jce_vcam_composer_apply(&vcam->composer, out->position, fwd, target,
                              out->target);

    out->fov_deg = vcam->fov_deg;
}

void jce_vcam_composer_apply(const JceVcamComposer *composer,
                                const float cam_pos[3],
                                const float cam_forward[3],
                                const float target_world[3],
                                float       out_lookat[3])
{
    if (!composer || !target_world || !out_lookat) return;
    if (composer->lookat_mode == JCE_VCAM_LOOKAT_HARD) {
        memcpy(out_lookat, target_world, 3 * sizeof(float));
        return;
    }

    /* Project target into camera space.  We use a single-axis screen
     * projection (yaw + pitch error) since the composer rect is in
     * NDC; full screen projection lives in the renderer. */
    float to_target[3] = { target_world[0] - cam_pos[0],
                            target_world[1] - cam_pos[1],
                            target_world[2] - cam_pos[2] };
    float L = sqrtf(to_target[0]*to_target[0] +
                    to_target[1]*to_target[1] +
                    to_target[2]*to_target[2]);
    if (L < 1e-6f || !cam_forward) {
        memcpy(out_lookat, target_world, 3 * sizeof(float));
        return;
    }
    to_target[0] /= L; to_target[1] /= L; to_target[2] /= L;

    /* Screen-space approximation: signed angles in horizontal +
     * vertical planes relative to cam_forward. */
    float yaw_err   = atan2f(to_target[0], to_target[2]) -
                       atan2f(cam_forward[0], cam_forward[2]);
    float pitch_err = asinf(to_target[1]) - asinf(cam_forward[1]);

    /* Bring into [-π, π]. */
    while (yaw_err >  3.14159265f) yaw_err -= 6.28318530f;
    while (yaw_err < -3.14159265f) yaw_err += 6.28318530f;

    /* Compare to dead/soft zones (treated as half-widths in radians;
     * scaled by FOV in a host build but here we apply directly). */
    float corr_yaw   = 0;
    float corr_pitch = 0;
    if (fabsf(yaw_err) > composer->dead_zone_w) {
        float over = fabsf(yaw_err) - composer->dead_zone_w;
        float soft = composer->soft_zone_w > 0 ? composer->soft_zone_w : 1;
        float w    = over / soft;
        if (w > 1) w = 1;
        corr_yaw = (yaw_err > 0 ? 1 : -1) * over * w;
    }
    if (fabsf(pitch_err) > composer->dead_zone_h) {
        float over = fabsf(pitch_err) - composer->dead_zone_h;
        float soft = composer->soft_zone_h > 0 ? composer->soft_zone_h : 1;
        float w    = over / soft;
        if (w > 1) w = 1;
        corr_pitch = (pitch_err > 0 ? 1 : -1) * over * w;
    }

    /* Rotate forward by corr_yaw/corr_pitch and shoot a ray of length
     * L from cam_pos; that's the new look-at. */
    float cy = cosf(corr_yaw), sy = sinf(corr_yaw);
    float cp = cosf(corr_pitch), sp = sinf(corr_pitch);
    float dir[3] = {
        cam_forward[0] * cy + cam_forward[2] * sy,
        cam_forward[1] * cp + sp,
        -cam_forward[0] * sy + cam_forward[2] * cy,
    };
    float dlen = sqrtf(dir[0]*dir[0] + dir[1]*dir[1] + dir[2]*dir[2]);
    if (dlen > 1e-6f) { dir[0]/=dlen; dir[1]/=dlen; dir[2]/=dlen; }
    out_lookat[0] = cam_pos[0] + dir[0] * L;
    out_lookat[1] = cam_pos[1] + dir[1] * L;
    out_lookat[2] = cam_pos[2] + dir[2] * L;
}
