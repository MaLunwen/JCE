/*
 * jce_occlusion_culler.h  GPU-query occlusion culling.
 *
 * Provides coherence-based, two-pass occlusion culling using bgfx
 * hardware occlusion queries.  Designed to work alongside the existing
 * frustum culling and render queue auto-instancing passes.
 *
 * Algorithm overview (per frame)
 * ─────────────────────────────
 * 1. begin_frame()   — poll all in-flight bgfx query results from the
 *                      PREVIOUS frame.
 * 2. entity_visible() — return whether entity was visible last frame.
 *                      New / unknown entities return true (warm-up frame).
 * 3. After submitting the main entity draw, call submit_query() to queue
 *    a bounding-box proxy draw for this entity for the CURRENT frame.
 * 4. The proxy draw runs in a dedicated depth-only view (no color write)
 *    and attaches a hardware occlusion query.  Results land in step 1
 *    of the next frame.
 *
 * Integration with the render queue
 * ──────────────────────────────────
 * The occlusion pre-pass runs BEFORE the main render queue flush.
 * Entities are tested before being submitted to the render queue:
 *   - Failed test (occluded, previous frame): skip entity this frame,
 *     but still submit the proxy query so visibility can recover.
 *   - Passed test / warm-up: submit entity to the render queue normally.
 *
 * Platform notes
 * ──────────────
 * • Occlusion queries require `supports_occlusion_query` from
 *   `jce_renderer_caps_get_gpu_caps()`.  When the cap is absent, this
 *   module is a no-op (every entity is considered visible).
 * • D3D11 / D3D12 / Vulkan / Metal: hardware queries.
 * • OpenGL ES 2.0 / WebGL 1.0: no support — falls back to always-visible.
 *
 * Layer: Render (Layer 4).
 */

#ifndef JCE_OCCLUSION_CULLER_H
#define JCE_OCCLUSION_CULLER_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>
#include <jce/renderer/jce_shaders.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceOcclusionCuller JceOcclusionCuller;

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

typedef struct {
    /* Maximum number of entities tracked simultaneously.
       Each entity needs one bgfx OcclusionQueryHandle.
       bgfx's hard cap is BGFX_CONFIG_MAX_OCCLUSION_QUERIES (default 4096). */
    uint32_t max_entities;

    /* bgfx view ID for the depth-only proxy pre-pass.
       Must not conflict with other engine views.  Default: 254. */
    uint16_t view_id;

    /* Conservative factor: pixels_visible threshold below which an entity
       is considered occluded.  0 = any visible pixel keeps entity alive.
       Default: 0. */
    int32_t min_pixels;
} JceOcclusionConfig;

/* Return a sensible default config. */
static inline JceOcclusionConfig jce_occlusion_config_default(void)
{
    JceOcclusionConfig c;
    c.max_entities = 2048;
    c.view_id      = 254;
    c.min_pixels   = 0;
    return c;
}

/* ================================================================== */
/* Stats                                                               */
/* ================================================================== */

typedef struct {
    uint32_t total_entities;    /* entities tested this frame */
    uint32_t visible;           /* query result: visible      */
    uint32_t occluded;          /* query result: occluded     */
    uint32_t warm_up;           /* first frame for new entity */
    uint32_t no_result;         /* query in flight / pending  */
} JceOcclusionStats;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

/* Create an occlusion culler.
   shaders must contain the loaded JceShaderSet (needs 'color' program for
   the AABB proxy draw).  Returns NULL if occlusion queries are not
   supported on the current GPU. */
JCE_API JceOcclusionCuller *jce_occlusion_culler_create(
    const JceOcclusionConfig *config,
    const JceShaderSet       *shaders);

JCE_API void jce_occlusion_culler_destroy(JceOcclusionCuller *oc);

/* ================================================================== */
/* Per-frame API                                                       */
/* ================================================================== */

/* Call once at the start of each frame BEFORE entity submission.
   Polls all pending hardware query results from the previous frame. */
JCE_API void jce_occlusion_culler_begin_frame(JceOcclusionCuller *oc,
                                               const float        *view_mtx,
                                               const float        *proj_mtx);

/* Returns true if the entity should be rendered this frame.
   entity_id  — unique 64-bit entity handle
   center     — world-space AABB centre
   radius     — bounding sphere radius (max(half_extents))
   Returns true unconditionally if hardware queries are unsupported
   or if this is the entity's warm-up frame. */
JCE_API bool jce_occlusion_culler_entity_visible(JceOcclusionCuller *oc,
                                                  uint64_t            entity_id,
                                                  jce_vec3            center,
                                                  float               radius);

/* Submit the AABB proxy draw + occlusion query for the current frame.
   Call this after (or instead of) submitting the real entity draw.
   ALWAYS call this — even for occluded entities — so visibility can
   recover when the entity becomes unoccluded. */
JCE_API void jce_occlusion_culler_submit_query(JceOcclusionCuller *oc,
                                                uint64_t            entity_id,
                                                jce_vec3            center,
                                                float               radius);

/* Retrieve per-frame stats (call after entity loop). */
JCE_API JceOcclusionStats jce_occlusion_culler_get_stats(
    const JceOcclusionCuller *oc);

/* Returns true if this culler is operational (hardware queries supported).
   When false, entity_visible() always returns true. */
JCE_API bool jce_occlusion_culler_is_active(const JceOcclusionCuller *oc);

JCE_EXTERN_C_END

#endif /* JCE_OCCLUSION_CULLER_H */
