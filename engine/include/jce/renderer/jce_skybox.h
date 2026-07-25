/*
 * jce_skybox.h  Cubemap skybox rendering and equirectangular HDR loading.
 *
 * Supports:
 * - Loading equirectangular HDR images (via SDL3_image)
 * - Converting equirect to cubemap (GPU multi-pass)
 * - Rendering cubemap skybox in the 3D scene
 */

#ifndef JCE_SKYBOX_H
#define JCE_SKYBOX_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_renderer.h>
#include <jce/renderer/jce_texture_types.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSkybox JceSkybox;

/*
 * Create a skybox from an equirectangular HDR image file.
 * Returns NULL on failure (unsupported format, missing float texture caps, etc.).
 *
 * @param path       Path to equirectangular HDR image (.hdr, .exr, etc.)
 * @param data       Raw file data (if path is NULL, use memory buffer).
 * @param data_size  Size of data buffer.
 * @param cubemap_size  Resolution of each cubemap face (e.g., 512, 1024).
 */
JCE_API JceSkybox *jce_skybox_create_from_hdr_file(const char *path,
                                            uint32_t cubemap_size);

JCE_API JceSkybox *jce_skybox_create_from_hdr_memory(const void *data, uint32_t data_size,
                                              uint32_t cubemap_size);

JCE_API void jce_skybox_destroy(JceSkybox *sky);

/*
 * Render the skybox into the specified view.
 * Should be called early (before opaque geometry) with depth write off.
 *
 * @param sky         Skybox instance.
 * @param view_id     bgfx view ID.
 * @param inv_vp      Inverse of (projection * view) matrix.
 * @param exposure    Exposure multiplier (default 1.0).
 */
JCE_API void jce_skybox_render(const JceSkybox *sky, uint16_t view_id,
                       const jce_mat4 *inv_vp, float exposure);

/*
 * Get the equirectangular texture handle (RGBA16F or RGBA32F).
 * Useful for IBL processing.
 */
JCE_API JceTexture jce_skybox_get_equirect_texture(const JceSkybox *sky);

/*
 * Get the decoded CPU-side equirectangular HDR pixels (RGBA32F, w*h*4 floats).
 * Retained from load time so IBL convolution can run CPU-side without a GPU
 * texture read-back. Returns NULL if pixels are not retained.
 *
 * @param sky    Skybox instance.
 * @param out_w  Receives equirect width  (may be NULL).
 * @param out_h  Receives equirect height (may be NULL).
 */
JCE_API const float *jce_skybox_get_equirect_pixels(const JceSkybox *sky,
                                                    uint32_t *out_w,
                                                    uint32_t *out_h);

/*
 * Get the cubemap texture handle (6-face cubemap).
 * Returns JCE_TEXTURE_INVALID if not yet converted.
 */
JCE_API JceTexture jce_skybox_get_cubemap(const JceSkybox *sky);

/* Check if the current GPU supports HDR skybox (float textures). */
JCE_API bool jce_skybox_supported(void);

JCE_EXTERN_C_END

#endif /* JCE_SKYBOX_H */
