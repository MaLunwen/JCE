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
    /* Maximum number of entities tracked simultaneously.  The query budget
       may be lower; overflow entities stay visible rather than false-culling. */
    uint32_t max_entities;

    /* Number of independent cullers sharing bgfx's global query pool.
       Each culler owns floor(hardware_cap / share_count) handles.  Use 2 for
       an editor with simultaneous Scene and Game views; default: 1. */
    uint32_t query_pool_share_count;

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
    c.query_pool_share_count = 1;
    /* Dedicated proxy view (JCE_VIEW_OCCLUSION, jce_views.h). Never share a
     * view id with another pass: bgfx view state is last-write-wins, and
     * the previous default (254 == JCE_VIEW_UI) let the UI canvas's pixel
     * ortho clobber the culler's camera transform, permanently false-culling
     * any entity whose proxy clipped to zero samples. */
    c.view_id      = 252;
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
    uint32_t unqueried;         /* outside this view's query budget */
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

/* Drop every tracked entity and return query handles to this culler's reusable
 * pool (no bgfx destroy/recreate churn).
 * MUST be called whenever the scene the culler observes is destroyed and
 * recreated (undo/redo snapshot restore, scene open/switch, Play stop):
 * entity ids restart in the fresh world, so stale slots would both leak the
 * hard-capped query pool AND hand recreated entities a dead entity's cull
 * verdict.  Safe mid-frame (bgfx defers handle destruction). */
JCE_API void jce_occlusion_culler_reset(JceOcclusionCuller *oc);

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
   recover when the entity becomes unoccluded.

   center       — WORLD-space centre of the entity's AABB (model-local AABB
                  centre transformed by the entity world matrix).  NOT the
                  entity origin: a mesh whose geometry is offset from its
                  pivot (e.g. a building modelled +Y up from its base) would
                  otherwise get a proxy box floating off the real geometry.
   half_extents — WORLD-space AABB half-extents (per axis).  Derived from the
                  model's LOCAL AABB scaled by the entity scale, so the proxy
                  cube matches the real silhouette.  A flat cube sized off a
                  single scale value both misses tall/flat geometry (under-
                  covers → false occlusion of things peeking past it) and over-
                  covers thin geometry (the box pokes out → never occluded). */
JCE_API void jce_occlusion_culler_submit_query(JceOcclusionCuller *oc,
                                                uint64_t            entity_id,
                                                jce_vec3            center,
                                                jce_vec3            half_extents);

/* Retrieve per-frame stats (call after entity loop). */
JCE_API JceOcclusionStats jce_occlusion_culler_get_stats(
    const JceOcclusionCuller *oc);

/* Returns true if this culler is operational (hardware queries supported).
   When false, entity_visible() always returns true. */
JCE_API bool jce_occlusion_culler_is_active(const JceOcclusionCuller *oc);

/* The bgfx view ID this culler submits its depth-only proxy queries to.
   The scene renderer must drive (set_view_transform) THIS view so the proxy
   draws share the main camera.  Lets two cullers (e.g. editor scene-view +
   game-view, which share one engine renderer and one bgfx frame) use distinct
   views and never clobber each other's proxy pass.  Returns 254 (the default)
   when oc is NULL. */
JCE_API uint16_t jce_occlusion_culler_get_view_id(const JceOcclusionCuller *oc);

/* Bind the depth-only proxy view to the SAME framebuffer the scene's color +
   depth pass renders into, and give it the matching viewport rect.  This is THE
   correctness seam for offscreen render paths: the proxy boxes' depth test
   (DEPTH_TEST_LEQUAL, WRITE_Z) must run against the depth buffer the color pass
   actually filled.  When the scene draws into an offscreen target (editor
   scene-/game-view bridge FBO, or the runtime postfx offscreen target), the
   proxy view defaulting to the backbuffer tests a STALE/empty depth buffer →
   the queries either pass everything (occlusion inert) or drop visible geometry
   (false-culling).

   fbo_idx : bgfx_frame_buffer_handle_t .idx of the scene FBO.  Pass UINT16_MAX
             (BGFX_INVALID_HANDLE) for the direct-to-backbuffer path (runtime
             without postfx) — the proxy then tests the backbuffer depth, which
             is exactly where that path's color pass wrote.
   x,y,w,h : the proxy view rect, matching the color pass's viewport.  w/h of 0
             leaves the rect untouched (caller already set it).

   Call once per frame, BEFORE submit_query (which only encodes draws).  The
   proxy view id is > the color view id, so bgfx orders it AFTER the color pass
   and the depth it reads is the current frame's. */
JCE_API void jce_occlusion_culler_bind_target(JceOcclusionCuller *oc,
                                              uint16_t fbo_idx,
                                              uint16_t x, uint16_t y,
                                              uint16_t w, uint16_t h);

JCE_EXTERN_C_END

#endif /* JCE_OCCLUSION_CULLER_H */
