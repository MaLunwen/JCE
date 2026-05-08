/*
 * jce_material_property_block.h  Per-instance material override.
 *
 * A property block is a sparse map of (uniform_name → value) overrides
 * that the renderer applies AFTER binding the base material.  Mirrors
 * Unity's MaterialPropertyBlock — lets you draw 100 identical meshes
 * with different colours / metallic / roughness without allocating
 * 100 full materials.
 *
 * Storage is small (cap on number of overrides) and trivially
 * copy-able so blocks can live as inline values on entity components.
 *
 * Layer: renderer (Layer 5) — public.
 */

#ifndef JCE_MATERIAL_PROPERTY_BLOCK_H
#define JCE_MATERIAL_PROPERTY_BLOCK_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_MPB_NAME_MAX     32
#define JCE_MPB_MAX_FLOAT4   8   /* up to 8 vec4-shaped overrides */
#define JCE_MPB_MAX_TEXTURE  4   /* up to 4 texture-handle overrides */

typedef struct {
    char     name[JCE_MPB_NAME_MAX]; /* uniform name without u_ prefix, e.g. "BaseColor" */
    float    value[4];               /* vec4 — float/vec3 use prefix entries */
    uint8_t  components;             /* 1..4 — how many components are meaningful */
} JceMpbFloat4;

typedef struct {
    char     name[JCE_MPB_NAME_MAX];
    uint16_t bgfx_handle;            /* bgfx::TextureHandle::idx; UINT16_MAX = unset */
    uint8_t  stage;                  /* shader sampler stage 0..15 */
} JceMpbTexture;

/* Plain-old-data property block, embeddable in components.  Default-
 * initialised state is "no overrides" (count fields are zero). */
typedef struct {
    JceMpbFloat4   floats[JCE_MPB_MAX_FLOAT4];
    uint8_t        float_count;
    JceMpbTexture  textures[JCE_MPB_MAX_TEXTURE];
    uint8_t        texture_count;
} JceMaterialPropertyBlock;

/* Initialise to empty.  Equivalent to memset(0). */
JCE_API void jce_mpb_clear(JceMaterialPropertyBlock *b);

/* Set a float / vec2 / vec3 / vec4 override.  Components is clamped to
 * [1,4].  If `name` already exists, its value is replaced.  Returns
 * false if the float-slot pool is full. */
JCE_API bool jce_mpb_set_float (JceMaterialPropertyBlock *b, const char *name, float v);
JCE_API bool jce_mpb_set_vec3  (JceMaterialPropertyBlock *b, const char *name, jce_vec3 v);
JCE_API bool jce_mpb_set_vec4  (JceMaterialPropertyBlock *b, const char *name, jce_vec4 v);
JCE_API bool jce_mpb_set_color (JceMaterialPropertyBlock *b, const char *name, float r, float g, float b_, float a);

/* Set a texture override.  `bgfx_handle` is the .idx field of a bgfx
 * texture handle; pass UINT16_MAX to clear. */
JCE_API bool jce_mpb_set_texture(JceMaterialPropertyBlock *b, const char *name,
                                 uint16_t bgfx_handle, uint8_t stage);

/* Lookup helpers (return false if name not present). */
JCE_API bool jce_mpb_get_float4(const JceMaterialPropertyBlock *b,
                                const char *name, float out[4]);

/* True if any overrides are configured. */
JCE_API bool jce_mpb_is_empty(const JceMaterialPropertyBlock *b);

JCE_EXTERN_C_END

#endif /* JCE_MATERIAL_PROPERTY_BLOCK_H */
