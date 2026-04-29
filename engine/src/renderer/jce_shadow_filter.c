/*
 * jce_shadow_filter.c -- shadow-map utility helpers.
 */

#include <jce/renderer/jce_shadow_filter.h>

#include <math.h>
#include <stdlib.h>

/* ----------------- xorshift32 PRNG ----------------- */
static uint32_t xs_next(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13; x ^= x >> 17; x ^= x << 5;
    *s = x ? x : 0xA5A5A5A5u;
    return *s;
}
static float xs_unit(uint32_t *s) { return (float)(xs_next(s) & 0xFFFFFF) / (float)0x1000000; }

void jce_shadow_filter_poisson_disk(uint32_t count, uint32_t seed, float *out)
{
    if (!out || count == 0) return;
    uint32_t state = seed ? seed : 0x12345678u;

    /* Mitchell's best-candidate: for each new sample, generate K candidates,
     * pick the one whose nearest-neighbor distance to existing samples is
     * largest.  K = 10 * (i+1) is the typical schedule. */
    out[0] = 0.0f; out[1] = 0.0f; /* anchor at center */
    for (uint32_t i = 1; i < count; ++i) {
        const uint32_t K = 10u * (i + 1u);
        float best_x = 0.0f, best_y = 0.0f, best_d = -1.0f;
        for (uint32_t k = 0; k < K; ++k) {
            float x, y;
            do {
                x = xs_unit(&state) * 2.0f - 1.0f;
                y = xs_unit(&state) * 2.0f - 1.0f;
            } while (x * x + y * y > 1.0f);
            float min_d2 = 1e30f;
            for (uint32_t j = 0; j < i; ++j) {
                float dx = x - out[2 * j + 0];
                float dy = y - out[2 * j + 1];
                float d2 = dx * dx + dy * dy;
                if (d2 < min_d2) min_d2 = d2;
            }
            if (min_d2 > best_d) { best_d = min_d2; best_x = x; best_y = y; }
        }
        out[2 * i + 0] = best_x;
        out[2 * i + 1] = best_y;
    }
}

void jce_shadow_filter_pssm_splits(float n, float f, uint32_t c,
                                   float lambda, float *out)
{
    if (!out || c == 0) return;
    if (lambda < 0.0f) lambda = 0.0f;
    if (lambda > 1.0f) lambda = 1.0f;
    out[0] = n;
    for (uint32_t i = 1; i < c; ++i) {
        float si = (float)i / (float)c;
        float log_split = n * powf(f / n, si);
        float lin_split = n + (f - n) * si;
        out[i] = lambda * log_split + (1.0f - lambda) * lin_split;
    }
    out[c] = f;
}

void jce_shadow_filter_stabilize_center(jce_vec3 center, float radius,
                                        jce_vec3 right, jce_vec3 up,
                                        uint32_t sm_size, jce_vec3 *out)
{
    if (!out || sm_size == 0 || radius <= 0.0f) {
        if (out) *out = center;
        return;
    }
    float texel_world = (radius * 2.0f) / (float)sm_size;
    /* Project center onto light axes, snap to texel grid, reconstruct. */
    float r_proj = center.x * right.x + center.y * right.y + center.z * right.z;
    float u_proj = center.x * up.x    + center.y * up.y    + center.z * up.z;
    float r_snap = floorf(r_proj / texel_world) * texel_world;
    float u_snap = floorf(u_proj / texel_world) * texel_world;
    float dr = r_snap - r_proj;
    float du = u_snap - u_proj;
    out->x = center.x + right.x * dr + up.x * du;
    out->y = center.y + right.y * dr + up.y * du;
    out->z = center.z + right.z * dr + up.z * du;
}

void jce_shadow_filter_gaussian_1d(uint32_t radius, float sigma, float *out)
{
    if (!out || radius == 0) return;
    if (radius > 8u) radius = 8u;
    if (sigma <= 0.0f) sigma = (float)radius * 0.5f;
    int32_t r = (int32_t)radius;
    float two_sigma2 = 2.0f * sigma * sigma;
    float sum = 0.0f;
    for (int32_t i = -r; i <= r; ++i) {
        float w = expf(-(float)(i * i) / two_sigma2);
        out[i + r] = w;
        sum += w;
    }
    if (sum > 0.0f) {
        float inv = 1.0f / sum;
        for (int32_t i = 0; i < 2 * r + 1; ++i) out[i] *= inv;
    }
}

/* ---------------------------------------------------------------- */
/* VSM helpers                                                       */
/* ---------------------------------------------------------------- */

float jce_shadow_filter_vsm_chebyshev(float moment_x,
                                       float moment_y,
                                       float t,
                                       float min_variance)
{
    /* Fully lit when the depth being tested is closer than the mean. */
    if (t <= moment_x) return 1.0f;

    /* Variance = E[X^2] - E[X]^2.  Clamp to a small positive value to
       avoid div-by-zero and to bound numerical noise.  This is the
       standard VSM upper bound (Donnelly & Lauritzen, 2006). */
    float variance = moment_y - moment_x * moment_x;
    if (variance < min_variance) variance = min_variance;

    float d  = t - moment_x;
    float p  = variance / (variance + d * d);
    return p;
}

float jce_shadow_filter_vsm_reduce_bleed(float p, float amount)
{
    if (amount <= 0.0f)            return p;
    if (amount >= 1.0f - 1e-6f)    return p > 0.999f ? 1.0f : 0.0f;
    if (p <= amount)               return 0.0f;
    return (p - amount) / (1.0f - amount);
}

const char *jce_shadow_mode_name(JceShadowMode mode)
{
    switch (mode) {
    case JCE_SHADOW_MODE_PCF: return "PCF";
    case JCE_SHADOW_MODE_VSM: return "VSM";
    default:                  return "?";
    }
}
