/*
 * jce_texture.h  Cross-platform texture loading and management.
 *
 * Loads images from PAK archive memory buffers into GPU textures.
 * Supports PNG, JPG, BMP, and other common image formats.
 */

#ifndef JCE_TEXTURE_H
#define JCE_TEXTURE_H


#include <jce/os/core/jce_defs.h>
#include <jce/renderer/jce_texture_types.h>
#include <jce/resource/jce_asset_format.h>  /* JceAssetTexInfo */

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive  JcePakArchive;

/* Load a texture from a PAK asset path (e.g. "textures/chalet.jpg").
   Returns JCE_TEXTURE_INVALID on failure. Uses clamp sampling. */
JCE_API JceTexture jce_texture_load(const JcePakArchive *pak, const char *asset_path);

/* Load a texture with explicit sampler flags (JCE_TEX_CLAMP/WRAP/MIRROR). */
JceTexture jce_texture_load_ex(const JcePakArchive *pak, const char *asset_path,
                                int sampler_mode);

/* Create a GPU texture from a pre-decoded RGBA8 surface (SDL_Surface*).
   The opaque pointer must be a valid SDL_Surface*.
   Caller retains ownership of the surface. */
JCE_API JceTexture jce_texture_load_from_surface(const void *surface, int sampler_mode);

/* Load a texture from raw pixel data (RGBA8, top-left origin).
   Caller retains ownership of data. */
JceTexture jce_texture_from_rgba(const void *data,
                                  uint32_t width, uint32_t height);

/* Create a GPU texture from a cooked .jceasset pixel payload, honoring the
   block-compressed format (BC1/BC3/BC5/BC7/ASTC4x4/ETC2A, default RGBA8) and
   the mip chain recorded in `info`. `pixels` points at the full TEX_PIXELS
   payload (all mips concatenated when info->mip_count > 1); `pixel_bytes` is
   its size. Used by the async texture finalize path so it matches the
   synchronous cooked loader. Caller retains ownership of `info`/`pixels`. */
JCE_API JceTexture jce_texture_from_cooked(const JceAssetTexInfo *info,
                                           const void *pixels,
                                           size_t pixel_bytes,
                                           int sampler_mode);

/* Update an existing RGBA8 texture in-place.
   Returns false when the handle is invalid, dimensions mismatch, or upload fails. */
bool       jce_texture_update_rgba(JceTexture tex, const void *data,
                                   uint32_t width, uint32_t height);

/* Update an existing RGBA8 texture in-place (zero-copy variant).
   bgfx takes a reference to `data`; caller guarantees data remains valid
   until bgfx_frame() is called (end of the current render frame).
   Returns false when the handle is invalid or dimensions mismatch. */
bool       jce_texture_update_rgba_ref(JceTexture tex, const void *data,
                                       uint32_t width, uint32_t height);
JCE_API void       jce_texture_get_size(JceTexture tex, uint32_t *w, uint32_t *h);

/* Destroy a texture. */
JCE_API void       jce_texture_destroy(JceTexture tex);

/* ================================================================== */
/* Runtime mip streaming (P3-A.2)                                      */
/* ================================================================== */
/*
 * Per-texture mip bias.  0 = full quality, +1 = drop top mip, +2 = drop
 * two top mips, etc.  Negative values are clamped to 0.
 *
 * The final bias actually applied on the GPU is the maximum of:
 *   - the global bias       (jce_texture_set_global_mip_bias),
 *   - the per-texture bias  (jce_texture_set_mip_bias),
 *   - the caps floor        (derived from jce_renderer_get_tier(); see
 *                            engine/src/renderer/jce_texture.c for the
 *                            Low/Mid/High → floor matrix).
 *
 * The smallest 4x4 mip-tail is always considered resident.
 */
JCE_API void   jce_texture_set_mip_bias(JceTextureId tex, int8_t bias);
JCE_API int8_t jce_texture_get_mip_bias(JceTextureId tex);

/*
 * Request a given top-mip residency (e.g. top_mip = 0 to upload the full
 * chain).  Honored on the next streaming tick; may be denied / clamped
 * upward under HARD streaming pressure or by the caps floor.
 */
JCE_API void    jce_texture_request_mip_residency(JceTextureId tex, uint8_t top_mip);
JCE_API uint8_t jce_texture_get_resident_top_mip(JceTextureId tex);

/*
 * Global mip bias.  Added on top of per-texture bias and the caps floor.
 * Driven automatically by the streaming pressure hook
 * (OK -> 0, SOFT -> 1, HARD -> 2) but also callable directly by tools
 * (editor quality slider, headless tests, etc.).
 */
JCE_API void   jce_texture_set_global_mip_bias(int8_t bias);
JCE_API int8_t jce_texture_get_global_mip_bias(void);

JCE_EXTERN_C_END

#endif /* JCE_TEXTURE_H */
