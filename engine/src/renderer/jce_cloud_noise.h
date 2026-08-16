/*
 * jce_cloud_noise.h  Volumetric cloud density field (CPU, deterministic).
 *
 * Generates the 3-D density field volumetric clouds raymarch through, on the
 * CPU, so it can be baked into a 3-D texture (or a 2-D slice atlas on backends
 * without 3-D texture support) at cook time instead of being recomputed by a
 * compute shader every frame.
 *
 * Pipeline, in the order the field is built:
 *   1. Perlin-Worley base shape   -- Perlin remapped by inverted Worley, so the
 *                                    field keeps Perlin's connectivity but gains
 *                                    Worley's cauliflower billows.
 *   2. Height gradient            -- altitude band per cloud type, blended
 *                                    continuously so an authored weather map
 *                                    can interpolate types without popping.
 *   3. Coverage carve             -- weather-map coverage remaps the shape.
 *   4. Detail erosion             -- high-frequency Worley wisps, applied only
 *                                    near the silhouette; the core is left
 *                                    untouched (eroding it dissolves the cloud).
 *
 * The field TILES SEAMLESSLY in X and Z (and in Y) with period `period_x/y/z`.
 * See the "PERIOD" comment block in jce_cloud_noise.c for how that is enforced.
 *
 * Determinism: every value comes from an integer hash of integer lattice
 * coordinates. There is no frac(sin(x)*k), no rand(), no time, no global state,
 * so the field is bit-identical across compilers, backends and runs.
 *
 * Layer: Renderer (L3). C99, pure CPU, caller-allocates, no I/O, no threads.
 */

/* PRIVATE to the renderer layer.  Deliberately not in engine/include: there is
 * no volumetric cloud feature yet, and publishing an API before the thing it
 * serves exists commits to a shape that has not been validated against a real
 * consumer.  It moves to <jce/renderer/...> with JCE_API/JCE_EXTERN_C_BEGIN
 * when the cloud pass lands.
 */

#ifndef JCE_CLOUD_NOISE_H
#define JCE_CLOUD_NOISE_H

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Octave counts are clamped to this; more octaves buy nothing once the octave
 * frequency exceeds the bake resolution, and it bounds the per-sample cost. */
#define JCE_CLOUD_MAX_OCTAVES 8

/* Upper bound on a single bake, as a voxel count. Purely a sanity rail so a
 * bad dimension triple cannot walk off the end of a plausible buffer. */
#define JCE_CLOUD_BAKE_MAX_VOXELS (1u << 26)

/* Cloud type anchors on the continuous `cloud_type` axis in [0,1]. Values
 * between anchors blend linearly, so an authored weather texture may store any
 * value in [0,1] and the height gradient will interpolate rather than pop. */
#define JCE_CLOUD_TYPE_STRATUS      0.0f
#define JCE_CLOUD_TYPE_CUMULUS      0.5f
#define JCE_CLOUD_TYPE_CUMULONIMBUS 1.0f

/* One sample of the 2-D weather field, indexed by world XZ. */
typedef struct JceCloudWeather {
    float coverage;      /* [0,1] fraction of sky filled                    */
    float cloud_type;    /* [0,1] stratus .. cumulus .. cumulonimbus        */
    float precipitation; /* [0,1] consumer-side (darkening / rain shafts)   */
} JceCloudWeather;

/* Caller-supplied weather map sampler. Called with world X/Z; must fill `out`.
 * Values it writes are clamped by this module, so a NaN or an out-of-range
 * texel from an authored map cannot poison the density field. */
typedef void (*JceCloudWeatherFn)(void *user, float world_x, float world_z,
                                  JceCloudWeather *out);

typedef struct JceCloudNoiseParams {
    uint32_t seed;

    /* World-space size of one repeat of the field. The sky is unbounded, so
     * sampling wraps into one period; `period_y` is the full thickness of the
     * cloud layer, which is why the field never visibly repeats vertically.
     * Must be > 0 -- non-positive / non-finite values fall back to defaults. */
    float period_x;
    float period_y;
    float period_z;

    /* Frequencies are in CELLS PER PERIOD, not cycles per world unit. They must
     * be integers for the lattice to wrap, and are rounded to integers on use;
     * that rounding is what makes the tiling exact. */
    float   base_freq;
    int32_t base_octaves;
    float   lacunarity;   /* per-octave frequency multiplier, [1,8]  */
    float   gain;         /* per-octave amplitude multiplier, [0.05,0.95] */

    float   detail_freq;
    int32_t detail_octaves;
    float   detail_strength; /* [0,0.95]; 0 disables erosion entirely */

    /* Procedural default weather map (used only when `weather_fn` is NULL). */
    float weather_freq;
    float weather_coverage_bias; /* [-1,1], added to procedural coverage */

    float density_scale; /* [0,8] multiplier applied before the final clamp */
    float anvil_bias;    /* [0,1] how far cumulonimbus flares at the top   */

    JceCloudWeatherFn weather_fn;   /* NULL => procedural default map */
    void             *weather_user; /* opaque, forwarded to weather_fn */
} JceCloudNoiseParams;

/* Fill `p` with a usable cloudscape. NULL is a no-op. */
void jce_cloud_noise_params_default(JceCloudNoiseParams *p);

/* Clamp `in` into a usable parameter set, filling anything absent from the
 * defaults. NULL `in` yields the defaults outright.
 *
 * Exposed so a caller taking many samples with the same parameters can do this
 * ONCE. jce_cloud_density does it on every call: it builds a default set, then
 * copies and clamps some twenty fields, per sample. A 128x128 shadow bake with
 * eight march steps is a million samples, so that is a million redundant
 * parameter validations of a value that did not change between any two of
 * them. */
void jce_cloud_noise_params_sanitize(const JceCloudNoiseParams *in,
                                     JceCloudNoiseParams *out);

/* Density at a point, from ALREADY-sanitized parameters.
 *
 * Identical to jce_cloud_density except that it trusts its parameters. Passing
 * anything that did not come out of jce_cloud_noise_params_sanitize is a
 * caller error, and an out-of-range field will produce a wrong field rather
 * than a clamped one. */
float jce_cloud_density_prepared(const JceCloudNoiseParams *sanitized,
                                 float x, float y, float z,
                                 float height01, float coverage,
                                 float cloud_type);

/* Density from ALREADY-sanitized parameters, with coverage and cloud type
 * taken from the field's own 2-D weather map at this XZ rather than passed in.
 *
 * This is what the sky atlas bake does, so it is what anything that has to
 * AGREE with the sky must do. The cloud-shadow bake used to pass one scalar
 * coverage and a fixed cloud_type of 0.5 for the whole map, which meant the
 * sky had weather in it and the ground had a uniform overcast: a cloud
 * overhead could be a gap on the ground and a gap overhead could be a shadow. */
float jce_cloud_density_weather_prepared(const JceCloudNoiseParams *sanitized,
                                         float x, float y, float z,
                                         float height01);

/* remap(v, lo, hi, nlo, nhi). A degenerate window (hi == lo) collapses to
 * `nlo` instead of dividing by zero. */
float jce_cloud_remap(float v, float lo, float hi, float nlo, float nhi);

/* Individual field components, in world space. `p` may be NULL (defaults).
 * All return [0,1] and are finite for any input. */
float jce_cloud_perlin_fbm(const JceCloudNoiseParams *p, float x, float y, float z);
float jce_cloud_worley_fbm(const JceCloudNoiseParams *p, float x, float y, float z);
float jce_cloud_perlin_worley(const JceCloudNoiseParams *p, float x, float y, float z);

/* Altitude band for a cloud type. `height01` is normalised altitude within the
 * cloud layer; `cloud_type` in [0,1]; `anvil_bias` in [0,1]. Returns [0,1].
 * Out-of-range arguments are clamped rather than rejected. */
float jce_cloud_height_gradient(float height01, float cloud_type, float anvil_bias);

/* Sample the weather field at world XZ -- the caller's `weather_fn` if set,
 * otherwise the procedural default. `out` NULL is a no-op; on any failure
 * `out` is zeroed rather than left undefined. */
void jce_cloud_weather_sample(const JceCloudNoiseParams *p, float x, float z,
                              JceCloudWeather *out);

/* Sample the density field at a world point. `height01` is the normalised
 * altitude within the cloud layer [0,1]; `coverage` and `cloud_type` in [0,1].
 * Returns [0,1], always finite. `p` NULL uses defaults; non-finite inputs
 * return 0. Zero coverage returns exactly 0. */
float jce_cloud_density(const JceCloudNoiseParams *p,
                        float x, float y, float z,
                        float height01, float coverage, float cloud_type);

/* As above, but coverage and cloud type come from the weather map. */
float jce_cloud_density_weather(const JceCloudNoiseParams *p,
                                float x, float y, float z, float height01);

/* Bake one full tile of the field into a caller-provided buffer, ready for a
 * 3-D texture upload. `out` must hold dim_x*dim_y*dim_z floats, indexed
 *     out[(z * dim_y + y) * dim_x + x]      (x fastest)
 * Voxels are sampled at cell centres across exactly one period in X and Z and
 * across the full layer in Y, so the baked volume tiles seamlessly.
 * `p` NULL bakes with default params. Returns false on NULL `out`, a zero
 * dimension, or a voxel count above JCE_CLOUD_BAKE_MAX_VOXELS. */
bool jce_cloud_noise_bake(const JceCloudNoiseParams *p, float *out,
                          uint32_t dim_x, uint32_t dim_y, uint32_t dim_z);

/* Pixel dimensions of the 2-D slice atlas produced by
 * jce_cloud_noise_bake_atlas() for the same arguments: `tiles_x` slices per
 * atlas row, ceil(dim_z / tiles_x) rows. Returns false (and zeroes the outputs)
 * on degenerate input. */
bool jce_cloud_noise_atlas_size(uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                                uint32_t tiles_x,
                                uint32_t *out_w, uint32_t *out_h);

/* Bake the same volume as a 2-D slice atlas, for backends without 3-D
 * textures. `out` must hold out_w*out_h floats as reported by
 * jce_cloud_noise_atlas_size(); the buffer is fully written (padding tiles,
 * when dim_z is not a multiple of tiles_x, are zeroed). */
bool jce_cloud_noise_bake_atlas(const JceCloudNoiseParams *p, float *out,
                                uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                                uint32_t tiles_x);

/* ── Atlas addressing ───────────────────────────────────────────────────
 *
 * A slice atlas is a 3-D volume folded into a 2-D image, so every consumer has
 * to agree on the fold.  The fold itself is arithmetic nobody gets visibly
 * wrong -- a shader that mis-addresses it produces clouds that still LOOK like
 * clouds, just not the ones that were baked, and no screenshot reveals that.
 *
 * These two exist so the GPU-side addressing can be checked against the CPU
 * field headlessly, the same way the atmosphere LUT's UV mapping is.
 *
 * `z01` is the normalised slice coordinate; slices are sampled at their
 * CENTRES, so slice k of N sits at (k + 0.5)/N.  Sampling at k/N shifts the
 * whole volume half a slice, which reads as clouds sitting at the wrong
 * altitude rather than as an obvious error. */
void jce_cloud_atlas_uv(uint32_t dim_x, uint32_t dim_y, uint32_t dim_z,
                        uint32_t tiles_x,
                        float x01, float y01, float z01,
                        float *out_u, float *out_v);

/* Bilinear fetch within a slice, matching what a shader does.  Deliberately
 * does NOT blend between slices: the atlas layout puts neighbouring slices in
 * unrelated places in the image, so a naive bilinear tap across a tile border
 * reads a completely different altitude.  A consumer that wants trilinear must
 * fetch two slices and lerp -- which is exactly the trap this function exists
 * to make visible. */
float jce_cloud_atlas_sample(const float *atlas, uint32_t atlas_w,
                             uint32_t atlas_h, float u, float v);

/* ── March cost ─────────────────────────────────────────────────────────
 *
 * The volumetric march is the sky's dominant fragment cost: every pixel that
 * sees sky walks the layer, and each step evaluates a phase function per
 * scattering octave.  The step count is therefore the one knob that decides
 * whether clouds are affordable on a given machine.
 *
 * Steps scale with the hardware tier, NOT with coverage: a thin overcast costs
 * the same per ray as a thick one, because the ray traverses the same slab
 * either way -- what changes is how much of it is dense.  Scaling on coverage
 * would make the frame time jump around as weather animated, which is worse
 * than being uniformly slower.
 *
 * `tier` is a JceGpuTier; out-of-range values are treated as the lowest, so a
 * caps query that failed degrades to cheap rather than to expensive.
 *
 * The returned value is always <= JCE_CLOUD_MARCH_MAX_STEPS, which is the
 * shader's compile-time loop bound -- a dynamic loop bound would not compile
 * on the oldest profile the sky shader still targets, so the shader loops to
 * the maximum and breaks on this budget. */
#define JCE_CLOUD_MARCH_MAX_STEPS 24

uint32_t jce_cloud_march_steps(int tier);

#ifdef __cplusplus
}
#endif
#endif /* JCE_CLOUD_NOISE_H */
