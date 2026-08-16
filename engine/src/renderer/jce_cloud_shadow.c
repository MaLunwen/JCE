#include "renderer/jce_cloud_shadow.h"

#include <math.h>

static bool cs_finite3(const float v[3])
{
    return isfinite(v[0]) && isfinite(v[1]) && isfinite(v[2]);
}

bool jce_cloud_shadow_bake_rows(const JceCloudShadowDesc *d, float *out,
                               uint32_t row_begin, uint32_t row_end)
{
    if (!d || !out) return false;
    if (d->resolution == 0u || d->resolution > 1024u) return false;
    if (!(d->world_extent_m > 0.0f) || !isfinite(d->world_extent_m)) return false;
    if (!cs_finite3(d->sun_dir)) return false;
    if (!(d->layer_top_m > d->layer_bottom_m)) return false;

    const float sx = d->sun_dir[0], sy = d->sun_dir[1], sz = d->sun_dir[2];
    const float slen = sqrtf(sx * sx + sy * sy + sz * sz);
    if (!(slen > 1e-6f)) return false;

    /* A sun at or below the horizon casts no cloud shadow that means anything:
     * the ray to it travels sideways through an unbounded slab, and the
     * integral diverges toward "everything is shadowed".  Refusing is right --
     * at night the directional light is already near zero, and a map full of
     * darkness multiplied by it would be indistinguishable from correct until
     * dawn, when the world would stay black. */
    const float ny = sy / slen;
    if (ny < 0.05f) return false;

    const float nx = sx / slen, nz = sz / slen;
    const float thickness = d->layer_top_m - d->layer_bottom_m;
    const float inv_thick = 1.0f / thickness;

    /* Distance along the sun ray needed to cross the slab.  Dividing by the
     * vertical component is what makes a low sun cast LONGER shadows through
     * more cloud -- using the slab thickness directly would make the shadow
     * identical at noon and at sunset, which is exactly backwards. */
    const float ray_len  = thickness / ny;
    const float step_len = ray_len / (float)JCE_CLOUD_SHADOW_STEPS;

    const uint32_t res = d->resolution;
    const float    ext = d->world_extent_m;
    const float    texel = ext / (float)res;
    const float    origin_x = d->center_x - 0.5f * ext;
    const float    origin_z = d->center_z - 0.5f * ext;

    const float sigma = (d->extinction > 0.0f) ? d->extinction : 0.0f;

    /* Once per slice, not once per sample. jce_cloud_density validates its
     * parameters on every call -- it builds a default set, copies, and clamps
     * about twenty fields -- and this loop calls it res * res * STEPS times.
     * The parameters are the same for every one of those. */
    JceCloudNoiseParams sp;
    jce_cloud_noise_params_sanitize(d->noise, &sp);

    if (row_end > res) row_end = res;
    if (row_begin >= row_end) return true;   /* nothing asked for is not a failure */

    for (uint32_t iz = row_begin; iz < row_end; ++iz) {
        /* Texel CENTRES, not corners.  Sampling at corners shifts the whole map
         * by half a texel, which at this resolution is metres on the ground --
         * a constant offset between a cloud and its shadow that reads as wind
         * rather than as a sampling error. */
        const float wz = origin_z + ((float)iz + 0.5f) * texel;
        for (uint32_t ix = 0; ix < res; ++ix) {
            const float wx = origin_x + ((float)ix + 0.5f) * texel;

            float tau = 0.0f;
            for (int s = 0; s < JCE_CLOUD_SHADOW_STEPS; ++s) {
                const float t = ((float)s + 0.5f) * step_len;
                const float px = wx + nx * t;
                const float py = d->layer_bottom_m + ny * t;
                const float pz = wz + nz * t;

                float h01 = (py - d->layer_bottom_m) * inv_thick;
                if (h01 < 0.0f) h01 = 0.0f;
                if (h01 > 1.0f) h01 = 1.0f;

                /* Weather-driven, exactly as the sky atlas samples it. */
                const float den = jce_cloud_density_weather_prepared(
                                      &sp, px, py, pz, h01);
                tau += den * sigma * step_len;
            }

            float T = expf(-tau);
            if (!(T >= 0.0f)) T = 0.0f;      /* catches NaN, which > would not */
            if (T > 1.0f)     T = 1.0f;
            out[(size_t)iz * res + ix] = T;
        }
    }
    return true;
}

bool jce_cloud_shadow_bake(const JceCloudShadowDesc *d, float *out)
{
    return jce_cloud_shadow_bake_rows(d, out, 0u,
                                      d ? d->resolution : 0u);
}

float jce_cloud_shadow_sample(const float *map, uint32_t resolution,
                              float world_extent_m, float center_x,
                              float center_z, float x, float z)
{
    if (!map || resolution == 0u || !(world_extent_m > 0.0f)) return 1.0f;

    const float ext = world_extent_m;
    const float u = (x - (center_x - 0.5f * ext)) / ext;
    const float v = (z - (center_z - 0.5f * ext)) / ext;

    /* Outside the map is FULL SUN, not the edge texel.  Clamping smears the
     * border value across the whole rest of the world, so one dark texel at the
     * edge becomes a hemisphere-wide shadow -- and that looks like weather. */
    if (u < 0.0f || u >= 1.0f || v < 0.0f || v >= 1.0f) return 1.0f;

    const float fx = u * (float)resolution - 0.5f;
    const float fz = v * (float)resolution - 0.5f;
    int x0 = (int)floorf(fx), z0 = (int)floorf(fz);
    const float tx = fx - (float)x0, tz = fz - (float)z0;

    const int maxi = (int)resolution - 1;
    int x1 = x0 + 1, z1 = z0 + 1;
    if (x0 < 0) x0 = 0;  if (x0 > maxi) x0 = maxi;
    if (z0 < 0) z0 = 0;  if (z0 > maxi) z0 = maxi;
    if (x1 < 0) x1 = 0;  if (x1 > maxi) x1 = maxi;
    if (z1 < 0) z1 = 0;  if (z1 > maxi) z1 = maxi;

    const float a = map[(size_t)z0 * resolution + x0];
    const float b = map[(size_t)z0 * resolution + x1];
    const float c = map[(size_t)z1 * resolution + x0];
    const float e = map[(size_t)z1 * resolution + x1];
    const float top = a + (b - a) * tx;
    const float bot = c + (e - c) * tx;
    return top + (bot - top) * tz;
}
