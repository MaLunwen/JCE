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

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

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

typedef enum {
    JCE_BLEND_NONE = 0,
    JCE_BLEND_ALPHA,       /* Standard alpha blending */
    JCE_BLEND_ADD,         /* Additive */
    JCE_BLEND_MULTIPLY     /* Multiply */
} JceBlendMode;

#ifdef __cplusplus
}
#endif

#endif /* JCE_GFX_TYPES_H */
