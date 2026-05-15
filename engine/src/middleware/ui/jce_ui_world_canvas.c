/*
 * jce_ui_world_canvas.c  Canvas world-matrix + pick helpers.
 */

#include <jce/middleware/ui/jce_ui_world_canvas.h>

#include <math.h>
#include <string.h>

static void identity4(float m[16])
{
    memset(m, 0, 16 * sizeof(float));
    m[0] = m[5] = m[10] = m[15] = 1.0f;
}

static void mul4(const float a[16], const float b[16], float o[16])
{
    float r[16];
    for (int i = 0; i < 4; ++i)
        for (int j = 0; j < 4; ++j) {
            float s = 0;
            for (int k = 0; k < 4; ++k) s += a[i*4+k] * b[k*4+j];
            r[i*4+j] = s;
        }
    memcpy(o, r, 16 * sizeof(float));
}

void jce_canvas_build_world_matrix(const JceCanvasComponent *c,
                                     float out[16])
{
    if (!c || !out) return;
    if (c->mode != JCE_CANVAS_WORLD) {
        identity4(out);
        return;
    }
    float cx = cosf(c->rotation_euler_deg[0] * 0.01745329f);
    float sx = sinf(c->rotation_euler_deg[0] * 0.01745329f);
    float cy = cosf(c->rotation_euler_deg[1] * 0.01745329f);
    float sy = sinf(c->rotation_euler_deg[1] * 0.01745329f);
    float cz = cosf(c->rotation_euler_deg[2] * 0.01745329f);
    float sz = sinf(c->rotation_euler_deg[2] * 0.01745329f);
    /* Rz * Ry * Rx. */
    float rx[16] = {
        1,0,0,0,
        0,cx,sx,0,
        0,-sx,cx,0,
        0,0,0,1
    };
    float ry[16] = {
        cy,0,-sy,0,
        0,1,0,0,
        sy,0,cy,0,
        0,0,0,1
    };
    float rz[16] = {
        cz,sz,0,0,
        -sz,cz,0,0,
        0,0,1,0,
        0,0,0,1
    };
    float tmp[16];
    mul4(rz, ry, tmp);
    mul4(tmp, rx, out);
    /* Apply pixel-scale to the x/y basis. */
    float s = c->pixel_scale > 0 ? c->pixel_scale : 1.0f;
    out[ 0] *= s; out[ 1] *= s; out[ 2] *= s;
    out[ 4] *= s; out[ 5] *= s; out[ 6] *= s;
    /* Translate. */
    out[12] = c->position[0];
    out[13] = c->position[1];
    out[14] = c->position[2];
}

/* Plane = canvas (z=0 in canvas-local space).  Build inverse of the
 * forward matrix to map the ray into local coords + intersect with
 * z=0. */
bool jce_canvas_world_pick(const JceCanvasComponent *c,
                            const float ro[3], const float rd[3],
                            float *out_u, float *out_v)
{
    if (!c || c->mode != JCE_CANVAS_WORLD || !ro || !rd) return false;
    /* Build world matrix's inverse — for the simple TRS with scale
     * absorbed into the basis, inverse = transpose of rotation part
     * combined with translation negation in original space.  We do
     * a full 4x4 inverse for safety. */
    float m[16];
    jce_canvas_build_world_matrix(c, m);
    /* Numerical inverse — generic 4x4. */
    float inv[16];
    /* Translation portion only — use cofactor of the upper 3x3 plus
     * the translation column. */
    /* For brevity, assume orthogonal basis after applying scale:
     * inv-rot = transpose of rotation columns / scale². */
    float s = c->pixel_scale > 0 ? c->pixel_scale : 1.0f;
    float s2 = s * s;
    /* 3x3 transpose of upper-left, divided by s². */
    inv[0] = m[0] / s2; inv[1] = m[4] / s2; inv[ 2] = m[ 8] / 1.0f;
    inv[4] = m[1] / s2; inv[5] = m[5] / s2; inv[ 6] = m[ 9] / 1.0f;
    inv[8] = m[2] / s2; inv[9] = m[6] / s2; inv[10] = m[10] / 1.0f;
    inv[3] = inv[7] = inv[11] = 0;
    /* Translation: -inv_R * t. */
    inv[12] = -(inv[0]*m[12] + inv[1]*m[13] + inv[2]*m[14]);
    inv[13] = -(inv[4]*m[12] + inv[5]*m[13] + inv[6]*m[14]);
    inv[14] = -(inv[8]*m[12] + inv[9]*m[13] + inv[10]*m[14]);
    inv[15] = 1;
    /* Transform ray. */
    float lro[3] = {
        inv[0]*ro[0] + inv[1]*ro[1] + inv[2]*ro[2] + inv[12],
        inv[4]*ro[0] + inv[5]*ro[1] + inv[6]*ro[2] + inv[13],
        inv[8]*ro[0] + inv[9]*ro[1] + inv[10]*ro[2] + inv[14],
    };
    float lrd[3] = {
        inv[0]*rd[0] + inv[1]*rd[1] + inv[2]*rd[2],
        inv[4]*rd[0] + inv[5]*rd[1] + inv[6]*rd[2],
        inv[8]*rd[0] + inv[9]*rd[1] + inv[10]*rd[2],
    };
    if (fabsf(lrd[2]) < 1e-6f) return false;
    float t = -lro[2] / lrd[2];
    if (t < 0) return false;
    float x = lro[0] + t * lrd[0];
    float y = lro[1] + t * lrd[1];
    /* Convert local x,y → canvas reference UV (0..1). */
    if (c->reference_w <= 0 || c->reference_h <= 0) return false;
    float half_w = c->reference_w * 0.5f;
    float half_h = c->reference_h * 0.5f;
    *out_u = (x + half_w) / c->reference_w;
    *out_v = (y + half_h) / c->reference_h;
    return (*out_u >= 0 && *out_u <= 1 && *out_v >= 0 && *out_v <= 1);
}
