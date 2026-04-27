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
JceIblData *jce_ibl_generate(JceTexture equirect_tex,
                              uint32_t irradiance_size,
                              uint32_t prefilter_size,
                              uint32_t brdf_lut_size);

JCE_API void jce_ibl_destroy(JceIblData *ibl);

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
