/*
 * jce_taa_apply.c  TAA state machine: jitter + history + reproject.
 *
 * Inverse-projection uses Gauss-Jordan with partial pivoting (small,
 * 4x4, fine).  Matrix layout is column-major to match bgfx convention.
 */

#include <jce/renderer/jce_taa_apply.h>

#include <math.h>
#include <string.h>

void jce_taa_apply_init(JceTaaApplyState *s)
{
    if (!s) return;
    memset(s, 0, sizeof(*s));
}

void jce_taa_apply_reset_history(JceTaaApplyState *s)
{
    if (!s) return;
    s->has_prev = false;
}

/* Generic 4x4 inverse — Cramer's rule for column-major matrices. */
static bool mat4_inverse(const float m[16], float out[16])
{
    float inv[16];
    inv[0]  =  m[5]*m[10]*m[15] - m[5]*m[11]*m[14] - m[9]*m[6]*m[15] +
                m[9]*m[7]*m[14]  + m[13]*m[6]*m[11] - m[13]*m[7]*m[10];
    inv[4]  = -m[4]*m[10]*m[15] + m[4]*m[11]*m[14] + m[8]*m[6]*m[15] -
                m[8]*m[7]*m[14]  - m[12]*m[6]*m[11] + m[12]*m[7]*m[10];
    inv[8]  =  m[4]*m[ 9]*m[15] - m[4]*m[11]*m[13] - m[8]*m[5]*m[15] +
                m[8]*m[7]*m[13]  + m[12]*m[5]*m[11] - m[12]*m[7]*m[9];
    inv[12] = -m[4]*m[ 9]*m[14] + m[4]*m[10]*m[13] + m[8]*m[5]*m[14] -
                m[8]*m[6]*m[13]  - m[12]*m[5]*m[10] + m[12]*m[6]*m[9];

    inv[1]  = -m[1]*m[10]*m[15] + m[1]*m[11]*m[14] + m[9]*m[2]*m[15] -
                m[9]*m[3]*m[14]  - m[13]*m[2]*m[11] + m[13]*m[3]*m[10];
    inv[5]  =  m[0]*m[10]*m[15] - m[0]*m[11]*m[14] - m[8]*m[2]*m[15] +
                m[8]*m[3]*m[14]  + m[12]*m[2]*m[11] - m[12]*m[3]*m[10];
    inv[9]  = -m[0]*m[ 9]*m[15] + m[0]*m[11]*m[13] + m[8]*m[1]*m[15] -
                m[8]*m[3]*m[13]  - m[12]*m[1]*m[11] + m[12]*m[3]*m[9];
    inv[13] =  m[0]*m[ 9]*m[14] - m[0]*m[10]*m[13] - m[8]*m[1]*m[14] +
                m[8]*m[2]*m[13]  + m[12]*m[1]*m[10] - m[12]*m[2]*m[9];

    inv[2]  =  m[1]*m[ 6]*m[15] - m[1]*m[ 7]*m[14] - m[5]*m[2]*m[15] +
                m[5]*m[3]*m[14]  + m[13]*m[2]*m[ 7] - m[13]*m[3]*m[6];
    inv[6]  = -m[0]*m[ 6]*m[15] + m[0]*m[ 7]*m[14] + m[4]*m[2]*m[15] -
                m[4]*m[3]*m[14]  - m[12]*m[2]*m[ 7] + m[12]*m[3]*m[6];
    inv[10] =  m[0]*m[ 5]*m[15] - m[0]*m[ 7]*m[13] - m[4]*m[1]*m[15] +
                m[4]*m[3]*m[13]  + m[12]*m[1]*m[ 7] - m[12]*m[3]*m[5];
    inv[14] = -m[0]*m[ 5]*m[14] + m[0]*m[ 6]*m[13] + m[4]*m[1]*m[14] -
                m[4]*m[2]*m[13]  - m[12]*m[1]*m[ 6] + m[12]*m[2]*m[5];

    inv[3]  = -m[1]*m[ 6]*m[11] + m[1]*m[ 7]*m[10] + m[5]*m[2]*m[11] -
                m[5]*m[3]*m[10]  - m[9]*m[2]*m[7]   + m[9]*m[3]*m[6];
    inv[7]  =  m[0]*m[ 6]*m[11] - m[0]*m[ 7]*m[10] - m[4]*m[2]*m[11] +
                m[4]*m[3]*m[10]  + m[8]*m[2]*m[7]   - m[8]*m[3]*m[6];
    inv[11] = -m[0]*m[ 5]*m[11] + m[0]*m[ 7]*m[ 9] + m[4]*m[1]*m[11] -
                m[4]*m[3]*m[ 9]  - m[8]*m[1]*m[7]   + m[8]*m[3]*m[5];
    inv[15] =  m[0]*m[ 5]*m[10] - m[0]*m[ 6]*m[ 9] - m[4]*m[1]*m[10] +
                m[4]*m[2]*m[ 9]  + m[8]*m[1]*m[6]   - m[8]*m[2]*m[5];

    float det = m[0]*inv[0] + m[1]*inv[4] + m[2]*inv[8] + m[3]*inv[12];
    if (fabsf(det) < 1e-9f) return false;
    float inv_det = 1.0f / det;
    for (int i = 0; i < 16; ++i) out[i] = inv[i] * inv_det;
    return true;
}

/* out = a * b, column-major. */
static void mat4_mul(const float a[16], const float b[16], float out[16])
{
    float r[16];
    for (int i = 0; i < 4; ++i) {
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i + k*4] * b[k + j*4];
            r[i + j*4] = s;
        }
    }
    memcpy(out, r, sizeof(r));
}

void jce_taa_apply_step(JceTaaApplyState *s,
                          const float view[16], const float proj[16],
                          uint32_t w, uint32_t h, uint32_t frame_index,
                          JceTaaApplyFrame *out)
{
    if (!s || !view || !proj || !out) return;

    /* Halton-driven sub-pixel jitter. */
    float jx = jce_taa_halton(frame_index, 2) - 0.5f;
    float jy = jce_taa_halton(frame_index, 3) - 0.5f;
    out->jitter_offset[0] = (w > 0) ? jx / (float)w : 0.0f;
    out->jitter_offset[1] = (h > 0) ? jy / (float)h : 0.0f;

    /* Reprojection: prev_VP * inv(curr_VP). */
    float curr_vp[16];
    mat4_mul(proj, view, curr_vp);
    if (s->has_prev) {
        float prev_vp[16], inv_curr[16];
        mat4_mul(s->prev_proj, s->prev_view, prev_vp);
        if (mat4_inverse(curr_vp, inv_curr))
            mat4_mul(prev_vp, inv_curr, out->reproject_mat4);
        else
            memcpy(out->reproject_mat4, prev_vp, sizeof(prev_vp));
        out->has_history = true;
    } else {
        /* Identity reproject — resolve pass will see no jitter mismatch. */
        memset(out->reproject_mat4, 0, sizeof(out->reproject_mat4));
        out->reproject_mat4[0]  = 1;
        out->reproject_mat4[5]  = 1;
        out->reproject_mat4[10] = 1;
        out->reproject_mat4[15] = 1;
        out->has_history = false;
    }

    out->write_slot   = s->history_slot;
    out->history_slot = (uint8_t)(s->history_slot ^ 1);

    /* Update persistent state for next frame. */
    memcpy(s->prev_view, view, sizeof(s->prev_view));
    memcpy(s->prev_proj, proj, sizeof(s->prev_proj));
    s->prev_frame_index = frame_index;
    s->history_slot     = (uint8_t)(s->history_slot ^ 1);
    s->has_prev         = true;
}
