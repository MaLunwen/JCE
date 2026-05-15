/*
 * jce_light_probe_eval.c  SH3 evaluation + projection.
 *
 * Basis order (matching most graphics references):
 *   c0  = Y00
 *   c1  = Y1-1 (y), c2 = Y10 (z), c3 = Y11 (x)
 *   c4  = Y2-2 (xy), c5 = Y2-1 (yz), c6 = Y20 (3z²-1),
 *   c7  = Y21 (xz), c8 = Y22 (x²-y²)
 *
 * Constant prefactors absorbed into the cosine-convolution helper;
 * eval uses the same basis the projection writes into.
 */

#include <jce/renderer/jce_light_probe_eval.h>

#include <math.h>

float jce_sh3_eval(const float c[9], const float n[3])
{
    float x = n[0], y = n[1], z = n[2];
    return  c[0]
          + c[1] * y
          + c[2] * z
          + c[3] * x
          + c[4] * x * y
          + c[5] * y * z
          + c[6] * (3.0f * z * z - 1.0f)
          + c[7] * x * z
          + c[8] * (x * x - y * y);
}

void jce_sh3_eval_rgb(const float r[9], const float g[9], const float b[9],
                       const float n[3], float out[3])
{
    out[0] = jce_sh3_eval(r, n);
    out[1] = jce_sh3_eval(g, n);
    out[2] = jce_sh3_eval(b, n);
}

void jce_sh3_project_sample(const float L[3], const float d[3],
                              float cr[9], float cg[9], float cb[9])
{
    float x = d[0], y = d[1], z = d[2];
    float basis[9] = {
        1.0f,
        y,
        z,
        x,
        x * y,
        y * z,
        3.0f * z * z - 1.0f,
        x * z,
        x * x - y * y,
    };
    for (int i = 0; i < 9; ++i) {
        cr[i] += L[0] * basis[i];
        cg[i] += L[1] * basis[i];
        cb[i] += L[2] * basis[i];
    }
}

void jce_sh3_convolve_cosine(float c[9])
{
    /* Standard cosine-convolution factors:
     *   l=0: π,    l=1: 2π/3,    l=2: π/4. */
    const float A0 = 3.141592653f;
    const float A1 = 2.094395102f;
    const float A2 = 0.785398163f;
    c[0] *= A0;
    c[1] *= A1; c[2] *= A1; c[3] *= A1;
    c[4] *= A2; c[5] *= A2; c[6] *= A2; c[7] *= A2; c[8] *= A2;
}
