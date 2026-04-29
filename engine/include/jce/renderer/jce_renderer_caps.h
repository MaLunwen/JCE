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


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

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

    /* Enable screen-space reflections (SSR).
       OFF on LOW/MEDIUM by default — heavy ALU + extra pass.
       Old / integrated GPUs can OOM or stutter; gate on tier + has_discrete_gpu. */
    bool       enable_ssr;

    /* Enable screen-space ambient occlusion (SSAO).
       OFF on LOW by default — sampling ALU dominates fragment cost
       on mobile / 2008-era GPUs. */
    bool       enable_ssao;

    /* Enable temporal anti-aliasing (TAA).
       OFF on LOW by default — needs FP16 framebuffer + history target,
       memory-prohibitive on 512MB GPUs. */
    bool       enable_taa;

    /* Enable volumetric fog (3D-texture based).
       OFF unless the GPU supports compute shaders + 3D textures. */
    bool       enable_volumetric_fog;

    /* Enable GPU particle simulation (compute-driven).
       OFF when BGFX_CAPS_COMPUTE is not present. */
    bool       enable_gpu_particles;

    /* True if the GPU is a discrete (NVIDIA / AMD desktop) part rather
       than integrated / mobile.  Used to gate VRAM-heavy features. */
    bool       has_discrete_gpu;

    /* Maximum texture resolution (e.g. 1024, 2048, 4096). */
    uint32_t   max_texture_size;
} JceRenderRecommendation;

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

/* Evaluate GPU capabilities and return the tier.
   Must be called AFTER jce_renderer_create() (bgfx must be initialized). */
JCE_API JceGpuTier jce_renderer_get_tier(void);

/* Return a bitmask of JCE_CAP_* flags for the current GPU. */
JCE_API uint32_t jce_renderer_get_caps(void);

/* Return recommended render settings based on the current tier. */
JCE_API JceRenderRecommendation jce_renderer_get_recommendation(void);

/* Return a human-readable name for the tier. */
JCE_API const char *jce_gpu_tier_name(JceGpuTier tier);

/* ================================================================== */
/* Backend enumeration                                                 */
/* ================================================================== */

/* JceRendererBackend — canonical home is the renderer layer.
   Defined here so that:
     - jce_renderer_caps.h has no need for a forward decl
     - app/middleware code can include just this header (which
       already pulls in the renderer caps API anyway)
     - resource/asset layers can use the enum without depending
       on application/. */
typedef enum JceRendererBackend {
    JCE_BACKEND_AUTO = 0,
    JCE_BACKEND_D3D11,
    JCE_BACKEND_D3D12,
    JCE_BACKEND_VULKAN,
    JCE_BACKEND_OPENGL,
    JCE_BACKEND_OPENGLES,
    JCE_BACKEND_METAL
} JceRendererBackend;

/* Return a stable human-readable name for a backend.  Never NULL. */
JCE_API const char *jce_renderer_backend_name(JceRendererBackend b);

/* Fill `out` with the list of renderer backends supported by the
   compiled-in bgfx build (queried via bgfx::getSupportedRenderers).
   The first slot is always JCE_BACKEND_AUTO.  Returns the number of
   entries written (<= max).  Pass max=0 / out=NULL to query the
   needed count. */
JCE_API int jce_renderer_caps_list_backends(JceRendererBackend *out, int max);

/* Platform-preferred backend order (without the JCE_BACKEND_AUTO leader).
   This is the SINGLE SOURCE OF TRUTH used by:
     - jce_renderer.c          (runtime auto-init fallback chain)
     - the preferences UI      (dropdown order, prepended with Auto)
   Stays in lock-step with JCE_SHADER_PROFILES in the top-level CMakeLists,
   so we never offer a backend whose .bin shaders weren't built.
   Returns the count of entries written (<= max). */
JCE_API int jce_renderer_caps_preferred_chain(JceRendererBackend *out, int max);

JCE_EXTERN_C_END

#endif /* JCE_RENDERER_CAPS_H */
