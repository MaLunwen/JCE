/*
 * jce_gfx_types.h  Engine-owned opaque graphics handle types.
 *
 * These types wrap the underlying graphics API handles (currently bgfx)
 * so that NO bgfx headers leak into the public engine API.
 * Only renderer implementation files (.c) may convert between
 * JCE handles and bgfx handles.
 *
 * Layer: Graphics (Layer 3).
 */

#ifndef JCE_GFX_TYPES_H
#define JCE_GFX_TYPES_H


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* Opaque handle types                                                 */
/* ================================================================== */

/* All handles are { uint16_t idx }.  UINT16_MAX = invalid. */

typedef struct { uint16_t idx; } JceShaderHandle;
typedef struct { uint16_t idx; } JceUniformHandle;

/* JceTextureHandle is intentionally compatible with the existing
   JceTexture type in jce_texture_types.h for incremental migration.
   They are the same struct layout. */
typedef struct { uint16_t idx; } JceTextureHandle;

/* An offscreen render target.  Distinct from JceTextureHandle even though
 * both are a uint16_t index: a framebuffer and its colour attachment are two
 * different objects, and passing one where the other belongs compiled fine
 * for as long as both were raw. */
typedef struct { uint16_t idx; } JceFrameBufferHandle;

/* Vertex layout handle (wraps bgfx_vertex_layout_handle_t). */
typedef struct { uint16_t idx; } JceVertexLayoutHandle;

/* Composite model handle (wraps index into model registry). */
typedef struct { uint16_t idx; } JceModelHandle;

/* Skeleton handle (wraps index into skeleton registry). */
typedef struct { uint16_t idx; } JceSkeletonHandle;

/* ================================================================== */
/* Invalid sentinel                                                    */
/* ================================================================== */

#define JCE_INVALID_SHADER   ((JceShaderHandle)  { UINT16_MAX })
#define JCE_INVALID_UNIFORM  ((JceUniformHandle) { UINT16_MAX })
#define JCE_INVALID_TEXTURE  ((JceTextureHandle) { UINT16_MAX })
#define JCE_INVALID_LAYOUT   ((JceVertexLayoutHandle) { UINT16_MAX })
#define JCE_INVALID_MODEL    ((JceModelHandle)   { UINT16_MAX })
#define JCE_INVALID_SKELETON ((JceSkeletonHandle){ UINT16_MAX })

/* ================================================================== */
/* Validity checks                                                     */
/* ================================================================== */

static inline bool jce_shader_valid(JceShaderHandle h)    { return h.idx != UINT16_MAX; }
static inline bool jce_uniform_valid(JceUniformHandle h)  { return h.idx != UINT16_MAX; }
static inline bool jce_gfx_texture_valid(JceTextureHandle h) { return h.idx != UINT16_MAX; }
static inline bool jce_model_handle_valid(JceModelHandle h)  { return h.idx != UINT16_MAX; }
static inline bool jce_skeleton_handle_valid(JceSkeletonHandle h) { return h.idx != UINT16_MAX; }

/* ================================================================== */
/* Render state enums (engine-owned, maps to bgfx internally)          */
/* ================================================================== */

typedef enum {
    JCE_CULL_NONE  = 0,
    JCE_CULL_CW    = 1,   /* Clockwise */
    JCE_CULL_CCW   = 2    /* Counter-clockwise (default) */
} JceCullMode;

/* WHICH blend equation a transparent surface composites with.  Read by
 * jce_pbr_material_render_state() when the material's ALPHA MODE is BLEND;
 * an opaque or alpha-tested material never consults it.
 *
 * The two questions are separate on purpose and both engines this is measured
 * against separate them too: alpha_mode decides WHETHER a surface is
 * transparent (and therefore which pass it draws in, and whether it writes
 * depth), blend_mode decides HOW it combines with what is behind it.
 *
 * This enum was public and had ZERO consumers for a release -- the material
 * path emitted SRC_ALPHA/INV_SRC_ALPHA and nothing else -- so additive glass,
 * multiply decals and glowing VFX could not be authored at all. */
typedef enum {
    /* Not a choice under BLEND: a transparent surface that does not blend is
     * a contradiction.  Treated as ALPHA there, so a zeroed or unset field
     * lands on the behaviour every material had before this was read. */
    JCE_BLEND_NONE = 0,
    JCE_BLEND_ALPHA,       /* src*a + dst*(1-a) -- glass, foliage, UI */
    JCE_BLEND_ADD,         /* src*a + dst      -- fire, glow, energy */
    JCE_BLEND_MULTIPLY     /* src*dst          -- shadow decals, tint */
} JceBlendMode;

typedef enum JceRenderFormat {
    JCE_RENDER_FORMAT_RGBA8 = 0,
    JCE_RENDER_FORMAT_RGBA16F,
    JCE_RENDER_FORMAT_DEPTH24_STENCIL8,
    JCE_RENDER_FORMAT_DEPTH32F,
    JCE_RENDER_FORMAT_R32F,
    JCE_RENDER_FORMAT_R16F,
    JCE_RENDER_FORMAT_RG16F,
    JCE_RENDER_FORMAT_RG32F,
    JCE_RENDER_FORMAT_RGBA32F
} JceRenderFormat;

typedef enum JceSamplerAddress {
    JCE_SAMPLER_ADDRESS_CLAMP = 0,
    JCE_SAMPLER_ADDRESS_WRAP,
    JCE_SAMPLER_ADDRESS_MIRROR
} JceSamplerAddress;

typedef enum JceSamplerFilter {
    JCE_SAMPLER_FILTER_NEAREST = 0,
    JCE_SAMPLER_FILTER_LINEAR
} JceSamplerFilter;

typedef struct JceSamplerDesc {
    uint32_t struct_size;
    uint32_t address_u;
    uint32_t address_v;
    uint32_t filter_min;
    uint32_t filter_mag;
    uint32_t filter_mip;
} JceSamplerDesc;

JCE_EXTERN_C_END

#endif /* JCE_GFX_TYPES_H */
