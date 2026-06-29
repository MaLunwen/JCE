/*
 * jce_foliage.c -- Deterministic vegetation scatter implementation.
 *
 * See jce_foliage.h.  Pure math; depends only on jce_terrain (height sampling)
 * and the C math library.  No allocation, no globals, no RNG state outside the
 * locally-seeded xorshift32, so a given (params, terrain, origin) is fully
 * reproducible across runs and platforms.
 */

#include <jce/middleware/scene/jce_foliage.h>
#include <jce/middleware/scene/jce_terrain.h>

#include <math.h>

#define FOLIAGE_DEG2RAD 0.01745329251994329577f
#define FOLIAGE_TWO_PI  6.28318530717958647692f

/* Deterministic per-scatter RNG (same pattern used by particles / weapons). */
static uint32_t xs32(uint32_t *s)
{
    uint32_t x = *s;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    *s = x;
    return x;
}

/* Uniform [0,1) with a clean 24-bit mantissa. */
static float randf01(uint32_t *s)
{
    return (float)(xs32(s) & 0x00FFFFFFu) / (float)0x01000000u;
}

uint32_t jce_foliage_scatter(const JceFoliageScatterParams *p,
                             const JceTerrain              *terrain,
                             const jce_vec3                *origin,
                             JceFoliageInstance            *out,
                             uint32_t                       out_cap)
{
    if (!p || !out || out_cap == 0) return 0;

    const float ax = p->area_x;
    const float az = p->area_z;
    const float density = p->density;
    if (ax <= 0.0f || az <= 0.0f || density <= 0.0f) return 0;

    float ox = 0.0f, oy = 0.0f, oz = 0.0f;
    if (origin) { ox = origin->x; oy = origin->y; oz = origin->z; }

    uint32_t target = (uint32_t)(density * ax * az);
    if (target > out_cap)                  target = out_cap;
    if (target > JCE_FOLIAGE_MAX_INSTANCES) target = JCE_FOLIAGE_MAX_INSTANCES;
    if (target == 0) return 0;

    uint32_t state = p->seed ? p->seed : 0x9E3779B9u;

    float smin = (p->scale_min > 0.0f) ? p->scale_min : 1.0f;
    float smax = (p->scale_max >= smin) ? p->scale_max : smin;

    /* Density mask (large-world #8a): when present, each candidate is kept with
     * probability = its mask cell, so brush-painted regions modulate density. */
    const bool have_mask = (p->density_mask != NULL) && (p->mask_dim > 0);
    const float inv_ax = 1.0f / ax, inv_az = 1.0f / az;

    /* A placement is kept when the terrain normal's Y >= cos(max_slope); a
     * limit of >=90° (or no terrain) disables the test. */
    const bool slope_limit =
        (terrain != NULL) && (p->max_slope_deg > 0.0f) && (p->max_slope_deg < 90.0f);
    const float cos_max = slope_limit ? cosf(p->max_slope_deg * FOLIAGE_DEG2RAD) : -1.0f;

    uint32_t n = 0;
    for (uint32_t i = 0; i < target; ++i) {
        /* Always draw the same 4 randoms per candidate so the sequence — and
         * thus the kept set — is identical regardless of slope rejections. */
        const float rx  = (randf01(&state) - 0.5f) * ax;
        const float rz  = (randf01(&state) - 0.5f) * az;
        const float yaw = randf01(&state) * FOLIAGE_TWO_PI;
        const float sc  = smin + (smax - smin) * randf01(&state);

        /* Density-mask rejection (5th RNG draw, only in mask mode so the
         * maskless sequence is unchanged): keep with probability = mask cell. */
        if (have_mask) {
            const float mr = randf01(&state);
            float u = rx * inv_ax + 0.5f;          /* candidate UV in the rect */
            float v = rz * inv_az + 0.5f;
            int mx = (int)(u * (float)p->mask_dim);
            int mz = (int)(v * (float)p->mask_dim);
            if (mx < 0) mx = 0; else if (mx >= p->mask_dim) mx = p->mask_dim - 1;
            if (mz < 0) mz = 0; else if (mz >= p->mask_dim) mz = p->mask_dim - 1;
            if (mr >= p->density_mask[(size_t)mz * (size_t)p->mask_dim + (size_t)mx])
                continue;                          /* painted-sparse — reject */
        }

        const float wx = ox + rx;
        const float wz = oz + rz;
        float wy = oy;

        if (terrain) {
            wy = jce_terrain_sample_height(terrain, wx, wz);
            if (slope_limit) {
                const float e   = 0.5f;
                const float hxp = jce_terrain_sample_height(terrain, wx + e, wz);
                const float hxm = jce_terrain_sample_height(terrain, wx - e, wz);
                const float hzp = jce_terrain_sample_height(terrain, wx, wz + e);
                const float hzm = jce_terrain_sample_height(terrain, wx, wz - e);
                const float dx  = (hxp - hxm) / (2.0f * e);
                const float dz  = (hzp - hzm) / (2.0f * e);
                /* normal.y of normalize(-dx, 1, -dz). */
                const float ny  = 1.0f / sqrtf(dx * dx + dz * dz + 1.0f);
                if (ny < cos_max) continue;   /* too steep — skip */
            }
        }

        out[n].pos[0] = wx;
        out[n].pos[1] = wy;
        out[n].pos[2] = wz;
        out[n].rot_y  = yaw;
        out[n].scale  = sc;
        ++n;
    }
    return n;
}
