/*
 * jce_xr_camera.c  Per-eye view/projection from XR poses + FOVs.
 */

#include <jce/renderer/jce_xr_camera.h>

#include <math.h>
#include <string.h>

void jce_xr_camera_init(JceXrCameraRig *r)
{
    if (!r) return;
    memset(r, 0, sizeof(*r));
    for (int e = 0; e < JCE_XR_EYE_COUNT; ++e) {
        r->eye_fov[e].fov_up    = 0.7853982f;
        r->eye_fov[e].fov_down  = 0.7853982f;
        r->eye_fov[e].fov_left  = 0.7853982f;
        r->eye_fov[e].fov_right = 0.7853982f;
        r->eye_fov[e].near_z    = 0.1f;
        r->eye_fov[e].far_z     = 1000.0f;
        r->eye_pose[e].orientation[3] = 1.0f;
    }
    r->head_pose.orientation[3] = 1.0f;
    r->ipd = 0.063f;
}

/* Quaternion → rotation matrix (column-major, right-handed). */
static void quat_to_mat3(const float q[4], float m[9])
{
    float x = q[0], y = q[1], z = q[2], w = q[3];
    float xx = x*x, yy = y*y, zz = z*z;
    float xy = x*y, xz = x*z, yz = y*z;
    float wx = w*x, wy = w*y, wz = w*z;
    m[0] = 1 - 2*(yy + zz); m[3] = 2*(xy - wz);     m[6] = 2*(xz + wy);
    m[1] = 2*(xy + wz);     m[4] = 1 - 2*(xx + zz); m[7] = 2*(yz - wx);
    m[2] = 2*(xz - wy);     m[5] = 2*(yz + wx);     m[8] = 1 - 2*(xx + yy);
}

static void compose_view(const JceXrPose *head, const JceXrPose *eye_rel,
                          float out[16])
{
    /* World pose of the eye = head ⊕ eye_rel.
     * View = inverse(eye world pose). */
    float hm[9], em[9];
    quat_to_mat3(head->orientation, hm);
    quat_to_mat3(eye_rel->orientation, em);
    /* Eye world rotation = hm * em (column-major mul). */
    float rw[9];
    for (int i = 0; i < 3; ++i) {
        for (int j = 0; j < 3; ++j) {
            float s = 0;
            for (int k = 0; k < 3; ++k) s += hm[i + k*3] * em[k + j*3];
            rw[i + j*3] = s;
        }
    }
    /* Eye world position = head pos + hm * eye_rel pos. */
    float et[3] = {
        head->position[0] + hm[0]*eye_rel->position[0] +
                             hm[3]*eye_rel->position[1] +
                             hm[6]*eye_rel->position[2],
        head->position[1] + hm[1]*eye_rel->position[0] +
                             hm[4]*eye_rel->position[1] +
                             hm[7]*eye_rel->position[2],
        head->position[2] + hm[2]*eye_rel->position[0] +
                             hm[5]*eye_rel->position[1] +
                             hm[8]*eye_rel->position[2],
    };
    /* View matrix = transpose(rw) translated by -rw^T * et. */
    out[ 0] = rw[0]; out[ 1] = rw[3]; out[ 2] = rw[6]; out[ 3] = 0;
    out[ 4] = rw[1]; out[ 5] = rw[4]; out[ 6] = rw[7]; out[ 7] = 0;
    out[ 8] = rw[2]; out[ 9] = rw[5]; out[10] = rw[8]; out[11] = 0;
    out[12] = -(out[0]*et[0] + out[4]*et[1] + out[ 8]*et[2]);
    out[13] = -(out[1]*et[0] + out[5]*et[1] + out[ 9]*et[2]);
    out[14] = -(out[2]*et[0] + out[6]*et[1] + out[10]*et[2]);
    out[15] = 1;
}

void jce_xr_camera_view(const JceXrCameraRig *r, JceXrEye e, float out[16])
{
    if (!r || !out || e >= JCE_XR_EYE_COUNT) return;
    compose_view(&r->head_pose, &r->eye_pose[e], out);
}

void jce_xr_camera_projection(const JceXrCameraRig *r, JceXrEye e, float out[16])
{
    if (!r || !out || e >= JCE_XR_EYE_COUNT) return;
    const JceXrEyeFov *f = &r->eye_fov[e];
    float tan_l = tanf(f->fov_left);
    float tan_r = tanf(f->fov_right);
    float tan_u = tanf(f->fov_up);
    float tan_d = tanf(f->fov_down);
    float w = tan_l + tan_r;
    float h = tan_u + tan_d;
    float n = f->near_z;
    float fz = f->far_z;
    memset(out, 0, 16 * sizeof(float));
    out[ 0] = 2.0f / w;
    out[ 5] = 2.0f / h;
    out[ 8] = (tan_r - tan_l) / w;
    out[ 9] = (tan_u - tan_d) / h;
    out[10] = -(fz + n) / (fz - n);
    out[11] = -1.0f;
    out[14] = -(2 * fz * n) / (fz - n);
}
