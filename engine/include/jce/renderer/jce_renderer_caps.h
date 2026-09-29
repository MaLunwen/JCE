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
    JCE_GPU_TIER_HIGH   = 2,   /* Desktop discrete / modern Metal/Vk  */
    JCE_GPU_TIER_ULTRA  = 3,   /* High-end discrete; reserved for     */
                               /* future use — currently gates the    */
                               /* same features as HIGH but is        */
                               /* exposed for forward compatibility.  */
    JCE_GPU_TIER_COUNT  = 4
} JceGpuTier;

/* ================================================================== */
/* Capability flags (bit field)                                        */
/* ================================================================== */

#define JCE_CAP_COMPUTE            (1u << 0)   /* Compute shaders        */
#define JCE_CAP_INSTANCING         (1u << 1)   /* Hardware instancing     */
#define JCE_CAP_TEXTURE_3D         (1u << 2)   /* 3D texture sampling     */
#define JCE_CAP_TEXTURE_FLOAT      (1u << 3)   /* Float textures (FP16+)  */
#define JCE_CAP_FRAMEBUFFER_FLOAT  (1u << 4)   /* Float framebuffers      */
/* RESERVED AND NEVER SET.  jce_renderer_get_caps() does not test for it,
 * because bgfx exposes no capability distinct from BGFX_CAPS_DRAW_INDIRECT
 * (-> JCE_CAP_DRAW_INDIRECT).  A caller reading this bit as 0 is reading a
 * question that was never asked, not a "no" -- the same trap as the
 * vertex-fetch format bits.  Use JCE_CAP_DRAW_INDIRECT. */
#define JCE_CAP_MULTI_DRAW         (1u << 5)
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

    /* ADVISORY ONLY - nothing in the engine reads these two.
     *
     * They describe what a tier "should" do, and the tier log used to print
     * them as if they were in effect ("pbr=off postfx=1" on the LOW tier while
     * PBR was running and the full chain was available).  That cost real
     * debugging time: a divergence hunt spent a pass on "PBR is off at LOW"
     * before a grep showed the field has zero consumers.  Either wire them or
     * delete them; until then the log must not claim they are applied.
     *
     * What DOES clamp the low tiers is the render-pipeline floor in
     * jce_render_pipeline.c (post_quality, cascades, TAA/SSR/fog/HDR) plus
     * shadow_map_size below, all of which are genuinely consumed. */
    uint32_t   max_postfx;      /* advisory: no consumer */
    bool       enable_pbr;      /* advisory: no consumer */

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

    /* Enable volumetric fog.  A fullscreen raymarch in a FRAGMENT shader --
       no compute, no 3D texture; it runs anywhere the backend can draw a
       quad, WebGL2 included.  Cost scales with step count, so what gates it
       is the tier, not a capability probe.

       NOTE: nothing reads this field.  The pass is actually gated by
       jce_render_pipeline's per-tier presets and by the LOW-tier floor. */
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
/* Editor / test tier override                                         */
/* ================================================================== */

/* Force jce_renderer_get_tier() (and therefore the recommendation
   pipeline) to return `tier` regardless of detected GPU capabilities.
   Used by the editor status-bar "GPU tier" widget so devs can preview
   how the engine behaves on the 512 MB / no-discrete-GPU baseline
   without actually owning that hardware.

   This is a runtime knob only — it does NOT alter the underlying
   bgfx caps queried by jce_renderer_get_caps(); features that probe
   raw hardware bits (e.g. compute, FP framebuffers) continue to do
   so.  Call jce_renderer_clear_tier_override() to restore detection. */
JCE_API void  jce_renderer_set_tier_override(JceGpuTier tier);
JCE_API void  jce_renderer_clear_tier_override(void);
JCE_API bool  jce_renderer_tier_is_overridden(void);

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
    JCE_BACKEND_METAL,
    /* Headless / no-op renderer.  Backed by BGFX_RENDERER_TYPE_NOOP:
       bgfx initialises but issues no GPU commands and never touches a
       driver, making it the canonical choice for dedicated servers,
       CI nodes and unit tests.  Intentionally excluded from
       jce_renderer_caps_preferred_chain() so JCE_BACKEND_AUTO never
       silently picks it on machines whose real GPU drivers all fail
       — those machines must still drop to the SDL software fallback
       (the blue/orange info-panel UI). */
    JCE_BACKEND_NOOP
} JceRendererBackend;

/* Graphics API compatibility tier selected when the bgfx package and JCE
   renderer are built.  This is deliberately separate from JceGpuTier:
   graphics API tier is a compatibility floor, while GPU tier is a runtime
   quality recommendation.  Changing this value requires rebuilding bgfx and
   JCE; it is not a live editor toggle. */
/* WHAT A HIGHER TIER DOES AND DOES NOT BUY, since the obvious reading is now
 * wrong.  Before the renderer laddered its own context, the tier WAS the
 * version you ran at, so picking a higher one really did get you newer GL.  It
 * no longer does: the renderer builds the newest core context the driver
 * grants at or above the floor, so a stable-tier build already runs at 4.6 on
 * hardware that offers it.
 *
 * What the higher tiers still change, measured against bgfx's source:
 *   >= 40  two texture-format defines (RED_INTEGER / RG_INTEGER)
 *   >= 41  two uniform-vector limit queries, which otherwise use 16 / 128
 *   >= 43  twelve sites, eleven of them extension-table seeds the runtime
 *          GL_EXTENSIONS scan turns on anyway, the twelfth falling back to
 *          KHR_debug detection
 * and, unavoidably, they RAISE THE MINIMUM -- a modern-tier build refuses
 * every GPU below GL 4.3.
 *
 * So the tier is now a support-matrix decision, not a performance one.  Pick a
 * higher one to narrow what you ship to, never to go faster. */
typedef enum JceGraphicsApiTier {
    JCE_GRAPHICS_API_TIER_STABLE  = 0,
    JCE_GRAPHICS_API_TIER_MODERN  = 1,
    JCE_GRAPHICS_API_TIER_CURRENT = 2,
    JCE_GRAPHICS_API_TIER_COUNT   = 3
} JceGraphicsApiTier;

typedef struct JceGraphicsApiVersion {
    uint16_t major;
    uint16_t minor;
    uint16_t patch;
} JceGraphicsApiVersion;

/* Build policy and negotiated runtime version for the active backend.
   A false *_verified flag means the backend did not report that value; a
   zero version must never be interpreted as a real API version. */
typedef struct JceRendererApiInfo {
    JceRendererBackend    backend;
    JceGraphicsApiTier    build_tier;
    JceGraphicsApiVersion minimum_version;
    JceGraphicsApiVersion runtime_version;
    JceGraphicsApiVersion shader_language_version;
    bool                  runtime_version_verified;
    bool                  shader_language_version_verified;
} JceRendererApiInfo;

/* Return a stable human-readable name for a backend.  Never NULL. */
JCE_API const char *jce_renderer_backend_name(JceRendererBackend b);

/* Stable human-readable tier name.  Never NULL. */
JCE_API const char *jce_graphics_api_tier_name(JceGraphicsApiTier tier);

/* Minimum API version required by a backend at a selected build tier.
   Backends without a versioned public API in this policy return 0.0.0. */
JCE_API JceGraphicsApiVersion jce_renderer_api_tier_minimum(
    JceRendererBackend backend, JceGraphicsApiTier tier);

/* Return the active backend's build floor and negotiated version snapshot.
   Safe to call before renderer creation; backend is AUTO and versions are
   unverified in that state. */
JCE_API JceRendererApiInfo jce_renderer_get_api_info(void);

/* The backend as it is RUNNING, e.g. "OpenGL 4.6" -- for log lines and UI.
 *
 * bgfx_get_renderer_name() cannot be used for this.  Its OpenGL string is
 * BGFX_RENDERER_OPENGL_NAME, a compile-time constant built from the build
 * floor, so a session that laddered up to a 4.6 core context still printed
 * "OpenGL 3.1" in three separate places -- which is exactly the reading that
 * cost an hour of this renderer's development, taken as a measurement when it
 * never was one.
 *
 * Falls back to bgfx's name when no version was observed (every non-GL
 * backend, and GL where the driver would not say).  Returns a pointer to
 * static storage, valid until the next call; never NULL. */
JCE_API const char *jce_renderer_running_backend_name(void);

/* Active bgfx backend as a JceRendererBackend (no-arg; JCE_BACKEND_AUTO
 * before bgfx init).  For middleware that must branch per backend without
 * depending on the renderer instance or bgfx headers. */
JCE_API JceRendererBackend jce_renderer_get_active_backend(void);

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
