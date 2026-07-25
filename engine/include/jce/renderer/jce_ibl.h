/*
 * jce_ibl.h  Image-Based Lighting generation.
 *
 * Generates IBL textures from an HDR equirectangular/cubemap source:
 * - Diffuse irradiance cubemap
 * - Specular prefiltered environment cubemap (with roughness mips)
 * - BRDF integration LUT (pre-baked 2D texture)
 */

#ifndef JCE_IBL_H
#define JCE_IBL_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceIblData JceIblData;

/*
 * Generate IBL data from an equirectangular HDR texture.
 * This is a CPU-side precomputation (spherical harmonics for irradiance,
 * importance-sampled prefilter for specular).
 *
 * Returns NULL if generation fails.
 *
 * @param equirect_tex  Equirectangular RGBA16F texture handle.
 * @param irradiance_size   Size of each irradiance cubemap face (e.g., 32).
 * @param prefilter_size    Size of the prefilter cubemap face at mip 0 (e.g., 128).
 * @param brdf_lut_size     Size of the BRDF LUT (e.g., 256).
 */
JCE_API JceIblData *jce_ibl_generate(JceTexture equirect_tex,
                              uint32_t irradiance_size,
                              uint32_t prefilter_size,
                              uint32_t brdf_lut_size);

/*
 * Generate IBL data directly from a CPU-side equirectangular HDR pixel buffer
 * (RGBA32F, w*h*4 floats). This is the working path: bgfx GPU textures cannot
 * be read back, so the irradiance + specular-prefilter convolutions consume
 * the already-decoded CPU pixels (e.g. from jce_skybox_get_equirect_pixels()).
 *
 * The BRDF LUT is NOT created here — callers share a single pre-baked LUT from
 * jce_ibl_create_brdf_lut(). Use jce_ibl_get_prefilter_mips() to drive the
 * shader's u_iblParams.y (max prefilter mip level).
 *
 * Returns NULL if generation fails (bad args / allocation / unsupported caps).
 *
 * @param pixels          RGBA32F equirect pixels, w*h*4 floats.
 * @param width           Equirect width  in pixels.
 * @param height          Equirect height in pixels.
 * @param irradiance_size Size of each irradiance cubemap face (e.g., 32).
 * @param prefilter_size  Size of the prefilter cubemap face at mip 0 (e.g., 128).
 */
JCE_API JceIblData *jce_ibl_generate_from_pixels(const float *pixels,
                                                 uint32_t width,
                                                 uint32_t height,
                                                 uint32_t irradiance_size,
                                                 uint32_t prefilter_size);

JCE_API void jce_ibl_destroy(JceIblData *ibl);

/*
 * Asynchronous IBL: split the expensive CPU convolution from the GPU upload so
 * the bake can run on a worker thread while the main thread stays responsive.
 *
 *   1. jce_ibl_bake_cpu()   — worker thread: cache-load OR CPU-convolve the
 *                             irradiance + prefilter cubemaps into a CPU buffer.
 *                             Touches NO bgfx state, so it is safe off-thread.
 *   2. jce_ibl_upload_cpu() — MAIN/render thread: upload that CPU buffer to GPU
 *                             cubemaps, returning a JceIblData. Consumes (frees)
 *                             the JceIblCpuData on both success and failure.
 *   jce_ibl_cpu_free()      — discard a CPU bake without uploading (stale bake).
 *
 * jce_ibl_generate_from_pixels() above is the synchronous convenience wrapper
 * (bake_cpu + upload_cpu on the calling thread).
 */
typedef struct JceIblCpuData JceIblCpuData;

JCE_API JceIblCpuData *jce_ibl_bake_cpu(const float *pixels,
                                        uint32_t width, uint32_t height,
                                        uint32_t irradiance_size,
                                        uint32_t prefilter_size);

JCE_API JceIblData *jce_ibl_upload_cpu(JceIblCpuData *cpu);

JCE_API void jce_ibl_cpu_free(JceIblCpuData *cpu);

/*
 * Number of mip levels in the prefiltered specular cubemap. The shader's
 * max prefilter mip level is (count - 1). Returns 0 for a NULL/invalid ibl.
 */
JCE_API uint32_t jce_ibl_get_prefilter_mips(const JceIblData *ibl);

/* Get the irradiance cubemap texture. */
JCE_API JceTexture jce_ibl_get_irradiance(const JceIblData *ibl);

/* Get the prefiltered specular cubemap texture. */
JCE_API JceTexture jce_ibl_get_prefilter(const JceIblData *ibl);

/* Get the BRDF integration LUT texture. */
JCE_API JceTexture jce_ibl_get_brdf_lut(const JceIblData *ibl);

/*
 * Create a pre-baked BRDF LUT (does not require an HDR source).
 * Can be called once at init time and shared across all IBL environments.
 *
 * @param size  LUT resolution (e.g., 256).
 */
JCE_API JceTexture jce_ibl_create_brdf_lut(uint32_t size);

JCE_EXTERN_C_END

#endif /* JCE_IBL_H */
