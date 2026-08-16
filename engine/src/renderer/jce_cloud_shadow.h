/*
 * jce_cloud_shadow.h -- top-down cloud transmittance, baked on the CPU.
 *
 * Clouds that do not darken the ground read as a painted backdrop: the sky is
 * volumetric and the world under it is uniformly sunlit, and the two look like
 * they belong to different scenes. The fix is one scalar per world XZ -- how
 * much sun survives the cloud layer on the way down -- multiplied into the
 * directional light.
 *
 * WHY CPU, and why that is not a compromise:
 *
 * The obvious answer is a compute shader marching the density field. This
 * engine cannot use one: BGFX_CAPS_COMPUTE is false on WebGL2, which the
 * project charter requires, so a compute path would simply not exist on a
 * supported target. A fragment-shader bake would work but needs a render
 * target and a view id, and the map is small, low-frequency, and changes only
 * when the clouds or the sun move -- which is to say it is exactly the kind of
 * thing that should not be recomputed every frame on any processor.
 *
 * The map is baked at a low resolution and re-baked only when the inputs move
 * enough to matter. A 256x256 bake with 8 march steps is 512k density taps;
 * amortised over the hundreds of frames between rebakes it is far below what
 * one frame of a GPU march would cost, and it runs identically on every
 * backend -- which for a term that darkens the whole world is worth more than
 * the speed.
 *
 * Layer: Renderer (L3). Pure computation, no bgfx.
 */

#ifndef JCE_CLOUD_SHADOW_H
#define JCE_CLOUD_SHADOW_H

#include <stdbool.h>
#include <stdint.h>

#include "renderer/jce_cloud_noise.h"

#ifdef __cplusplus
extern "C" {
#endif

/* Steps taken through the cloud slab per texel. Fixed, and small: the slab is
 * thin relative to the map's texel size, so more steps resolve detail the map
 * cannot store anyway. */
#define JCE_CLOUD_SHADOW_STEPS 8

typedef struct JceCloudShadowDesc {
    const JceCloudNoiseParams *noise;   /* NULL = defaults                    */
    float    sun_dir[3];      /* direction TOWARD the sun, world, unit        */
    /* Coverage and cloud type are NOT parameters here. They come from the
     * field's own 2-D weather map, sampled per texel, because that is what the
     * sky's atlas bake does and this map has to agree with it.
     *
     * They used to be: one scalar coverage and a cloud_type fixed at 0.5 for
     * the entire map. The sky therefore had weather -- coverage and type
     * varying over kilometres -- and the ground had a uniform overcast, so a
     * cloud overhead could be a gap underfoot. Set `noise->weather_coverage_bias`
     * to control how much cloud there is; that is the same control the atlas
     * uses, and it must be given the same value. */
    float    layer_bottom_m;  /* cloud slab, world Y                          */
    float    layer_top_m;
    float    extinction;      /* per-unit-density extinction through the slab */
    float    world_extent_m;  /* the map covers extent x extent, centred on   */
    float    center_x;        /* (center_x, center_z)                         */
    float    center_z;
    uint32_t resolution;      /* texels per side                              */
} JceCloudShadowDesc;

/* Bake rows [row_begin, row_end) of the map.
 *
 * The rows are independent -- each texel is its own march through the slab --
 * so this splits cleanly across threads or across frames, and the choice of
 * which belongs to the caller, not here. It exists because measuring the whole
 * bake found 300 ms for a 128x128x8 map in a Release build: the header above
 * claims the cost is "far below what one frame of a GPU march would cost",
 * and that claim was never checked because until recently nothing ever
 * reached this function. 300 ms is twenty frames of a sixty-hertz budget,
 * landing in one of them.
 *
 * Returns false and touches nothing on a bad descriptor, exactly as the
 * whole-map entry does. A row range outside the map is clamped, not refused. */

/*
 * Bake `resolution * resolution` transmittance values into `out`, row-major,
 * x fastest. Each value is in [0,1]: 1 = full sun, 0 = fully shadowed.
 *
 * Returns false on any degenerate input WITHOUT writing `out`. The caller must
 * treat that as "no cloud shadows this frame" and keep whatever it had --
 * filling with zeros would black out the world, and filling with ones would
 * silently claim clear sky under an overcast one.
 */
bool jce_cloud_shadow_bake_rows(const JceCloudShadowDesc *d, float *out,
                               uint32_t row_begin, uint32_t row_end);

bool jce_cloud_shadow_bake(const JceCloudShadowDesc *desc, float *out);

/*
 * Sample the baked map at a world XZ, with bilinear filtering.
 *
 * Outside the map's extent returns 1.0 (full sun) rather than clamping to the
 * edge texel. Clamping smears whatever is at the border across the entire rest
 * of the world, so a single dark texel at the edge becomes a hemisphere-wide
 * shadow -- and it looks like weather, not like a sampling bug.
 */
float jce_cloud_shadow_sample(const float *map, uint32_t resolution,
                              float world_extent_m, float center_x,
                              float center_z, float x, float z);

#ifdef __cplusplus
}
#endif

#endif /* JCE_CLOUD_SHADOW_H */
