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

JCE_EXTERN_C_BEGIN

typedef struct JcePakArchive  JcePakArchive;

/* Load a texture from a PAK asset path (e.g. "textures/chalet.jpg").
   Returns JCE_TEXTURE_INVALID on failure. Uses clamp sampling. */
JceTexture jce_texture_load(const JcePakArchive *pak, const char *asset_path);

/* Load a texture with explicit sampler flags (JCE_TEX_CLAMP/WRAP/MIRROR). */
JceTexture jce_texture_load_ex(const JcePakArchive *pak, const char *asset_path,
                                int sampler_mode);

/* Create a GPU texture from a pre-decoded RGBA8 surface (SDL_Surface*).
   The opaque pointer must be a valid SDL_Surface*.
   Caller retains ownership of the surface. */
JceTexture jce_texture_load_from_surface(const void *surface, int sampler_mode);

/* Load a texture from raw pixel data (RGBA8, top-left origin).
   Caller retains ownership of data. */
JceTexture jce_texture_from_rgba(const void *data,
                                  uint32_t width, uint32_t height);

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
void       jce_texture_get_size(JceTexture tex, uint32_t *w, uint32_t *h);

/* Destroy a texture. */
void       jce_texture_destroy(JceTexture tex);

JCE_EXTERN_C_END

#endif /* JCE_TEXTURE_H */
