/* Exact 1024-sample production BRDF integration shared with the offline baker. */
#ifndef JCE_IBL_BRDF_H
#define JCE_IBL_BRDF_H
#include "jce_ibl_sampling.h"
/* ================================================================== */
/* BRDF Integration LUT                                                */
/* ================================================================== */

/* Geometry term for IBL (Schlick-GGX with k = roughness^2 / 2). */
static float geometry_schlick_ggx_ibl(float n_dot_v, float roughness)
{
    float k = (roughness * roughness) / 2.0f;
    return n_dot_v / (n_dot_v * (1.0f - k) + k);
}

static float geometry_smith_ibl(float n_dot_v, float n_dot_l, float roughness)
{
    return geometry_schlick_ggx_ibl(n_dot_v, roughness)
         * geometry_schlick_ggx_ibl(n_dot_l, roughness);
}

/* Integrate the BRDF for a given (NdotV, roughness) pair.
 * Returns (scale, bias) for the split-sum approximation. */
static void integrate_brdf(float n_dot_v, float roughness,
                           float *out_scale, float *out_bias)
{
    /* Clamp to avoid degenerate cases. */
    if (n_dot_v < 0.001f) n_dot_v = 0.001f;

    float vx = sqrtf(1.0f - n_dot_v * n_dot_v);
    float vy = 0.0f;
    float vz = n_dot_v;

    float scale = 0.0f;
    float bias  = 0.0f;

    const uint32_t SAMPLE_COUNT = 1024;

    for (uint32_t i = 0; i < SAMPLE_COUNT; i++) {
        float xi1, xi2;
        hammersley(i, SAMPLE_COUNT, &xi1, &xi2);

        float hx, hy, hz;
        importance_sample_ggx(xi1, xi2, roughness, &hx, &hy, &hz);

        /* Reflect V around H to get L. */
        float v_dot_h = vx * hx + vy * hy + vz * hz;
        float lx = 2.0f * v_dot_h * hx - vx;
        float ly = 2.0f * v_dot_h * hy - vy;
        float lz = 2.0f * v_dot_h * hz - vz;

        float n_dot_l = lz;  /* N = (0,0,1) in tangent space */
        float n_dot_h = hz;
        float vdh     = v_dot_h;

        if (n_dot_l > 0.0f) {
            float G = geometry_smith_ibl(n_dot_v, n_dot_l, roughness);
            float G_vis = (G * vdh) / (n_dot_h * n_dot_v);
            float Fc = powf(1.0f - vdh, 5.0f);

            scale += (1.0f - Fc) * G_vis;
            bias  += Fc * G_vis;
        }
    }

    *out_scale = scale / (float)SAMPLE_COUNT;
    *out_bias  = bias  / (float)SAMPLE_COUNT;
}


static void jce_ibl_brdf_row(uint32_t size, uint32_t y, uint16_t *out)
{
    float roughness = ((float)y + 0.5f) / (float)size;
    for (uint32_t x = 0; x < size; ++x) {
        float scale, bias;
        integrate_brdf(((float)x + 0.5f) / (float)size, roughness, &scale, &bias);
        out[x * 4] = f32_to_f16(scale);
        out[x * 4 + 1] = f32_to_f16(bias);
        out[x * 4 + 2] = f32_to_f16(0.0f);
        out[x * 4 + 3] = f32_to_f16(1.0f);
    }
}
#endif
