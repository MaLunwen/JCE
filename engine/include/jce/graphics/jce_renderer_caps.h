/*
 * jce_renderer_caps.h  GPU capability tiering and queries.
 *
 * Provides a simple LOW / MEDIUM / HIGH tier classification based on
 * the runtime GPU capabilities reported by bgfx.  Subsystems like
 * PostFX and PBR materials can use the tier to auto-configure quality.
 *
 * Also exposes fine-grained capability flags that callers may query
 * directly for platform-adaptive rendering decisions.
 *
 * Layer: Graphics (Layer 2 — depends on renderer).
 */

#ifndef JCE_RENDERER_CAPS_H
#define JCE_RENDERER_CAPS_H

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* GPU quality tier                                                    */
/* ================================================================== */

typedef enum {
    JCE_GPU_TIER_LOW    = 0,   /* Mobile / GLES 3.0 / old integrated  */
    JCE_GPU_TIER_MEDIUM = 1,   /* Mid-range / GLES 3.1+ / integrated  */
    JCE_GPU_TIER_HIGH   = 2    /* Desktop discrete / modern Metal/Vk  */
} JceGpuTier;

/* ================================================================== */
/* Capability flags (bit field)                                        */
/* ================================================================== */

#define JCE_CAP_COMPUTE            (1u << 0)   /* Compute shaders        */
#define JCE_CAP_INSTANCING         (1u << 1)   /* Hardware instancing     */
#define JCE_CAP_TEXTURE_3D         (1u << 2)   /* 3D texture sampling     */
#define JCE_CAP_TEXTURE_FLOAT      (1u << 3)   /* Float textures (FP16+)  */
#define JCE_CAP_FRAMEBUFFER_FLOAT  (1u << 4)   /* Float framebuffers      */
#define JCE_CAP_MULTI_DRAW         (1u << 5)   /* Multi-draw indirect     */
#define JCE_CAP_TEXTURE_COMPARE    (1u << 6)   /* Shadow map PCF          */
#define JCE_CAP_VERTEX_ID          (1u << 7)   /* gl_VertexID support     */
#define JCE_CAP_DRAW_INDIRECT      (1u << 8)   /* Draw-indirect           */

/* ================================================================== */
/* Recommended render settings per tier                                */
/* ================================================================== */

typedef struct JceRenderRecommendation {
    JceGpuTier tier;

    /* Shadow map resolution (px). */
    uint32_t   shadow_map_size;

    /* Maximum number of post-processing effects to enable. */
    uint32_t   max_postfx;

    /* Enable PBR (IBL + multi-light) vs. simple Blinn-Phong. */
    bool       enable_pbr;

    /* Enable bloom post-processing. */
    bool       enable_bloom;

    /* Enable FXAA. */
    bool       enable_fxaa;

    /* Maximum texture resolution (e.g. 1024, 2048, 4096). */
    uint32_t   max_texture_size;
} JceRenderRecommendation;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Evaluate GPU capabilities and return the tier.
   Must be called AFTER jce_renderer_create() (bgfx must be initialized). */
JceGpuTier jce_renderer_get_tier(void);

/* Return a bitmask of JCE_CAP_* flags for the current GPU. */
uint32_t jce_renderer_get_caps(void);

/* Return recommended render settings based on the current tier. */
JceRenderRecommendation jce_renderer_get_recommendation(void);

/* Return a human-readable name for the tier. */
const char *jce_gpu_tier_name(JceGpuTier tier);

#ifdef __cplusplus
}
#endif

#endif /* JCE_RENDERER_CAPS_H */
