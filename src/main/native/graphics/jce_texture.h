/*
 * jce_texture.h  Cross-platform texture loading via SDL3_image + bgfx.
 *
 * Loads images from PAK archive memory buffers into bgfx textures.
 * Supports all formats SDL3_image supports (PNG, JPG, BMP, etc.).
 */

#ifndef JCE_TEXTURE_H
#define JCE_TEXTURE_H

#include "jce_texture_types.h"

typedef struct PakArchive  PakArchive;
typedef struct SDL_Surface SDL_Surface;

/* Load a texture from a PAK asset path (e.g. "textures/chalet.jpg").
   Returns JCE_TEXTURE_INVALID on failure. Uses clamp sampling. */
JceTexture jce_texture_load(const PakArchive *pak, const char *asset_path);

/* Load a texture with explicit sampler flags (JCE_TEX_CLAMP/WRAP/MIRROR). */
JceTexture jce_texture_load_ex(const PakArchive *pak, const char *asset_path,
                                int sampler_mode);

/* Create a bgfx texture from a pre-decoded RGBA8 SDL_Surface.
   Caller retains ownership of the surface. */
JceTexture jce_texture_load_from_surface(const SDL_Surface *surf, int sampler_mode);

/* Load a texture from raw pixel data (RGBA8, top-left origin).
   Caller retains ownership of data. */
JceTexture jce_texture_from_rgba(const void *data,
                                  uint32_t width, uint32_t height);

/* Get texture dimensions (0 on invalid handle). */
void       jce_texture_get_size(JceTexture tex, uint32_t *w, uint32_t *h);

/* Destroy a texture. */
void       jce_texture_destroy(JceTexture tex);

#endif /* JCE_TEXTURE_H */
