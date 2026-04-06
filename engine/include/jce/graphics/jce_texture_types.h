/*
 * jce_texture_types.h  Lightweight texture type definitions.
 *
 * Provides only the JceTexture handle type and sampler constants.
 * Include this instead of jce_texture.h when you only need the types
 * (e.g. in headers that want to avoid pulling in the full texture API).
 */

#ifndef JCE_TEXTURE_TYPES_H
#define JCE_TEXTURE_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* Opaque texture handle wrapping bgfx_texture_handle_t. */
typedef struct { uint16_t idx; } JceTexture;

#define JCE_TEXTURE_INVALID ((JceTexture){ UINT16_MAX })

/* Sampler flags for texture loading. */
#define JCE_TEX_CLAMP   0   /* U/V clamp (default) */
#define JCE_TEX_WRAP    1   /* U/V repeat/wrap */
#define JCE_TEX_MIRROR  2   /* U/V mirror */

static inline bool jce_texture_valid(JceTexture tex) {
    return tex.idx != UINT16_MAX;
}

#ifdef __cplusplus
}
#endif

#endif /* JCE_TEXTURE_TYPES_H */
