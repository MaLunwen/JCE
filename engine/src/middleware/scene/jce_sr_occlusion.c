/* Scene-owned occlusion history, proxy camera/target and portal preparation. */
#include "jce_sr_internal.h"
#include "jce_sr_portal.h"
#include <jce/os/core/jce_perf_phase.h>
#include <jce/os/core/jce_timer.h>

void sr_occlusion_begin(JceSceneRenderer *sr, JceScene *scene,
    const JceCamera *camera, uint16_t view_id, const JceSceneRenderConfig *cfg)
{
    /* Begin occlusion culler frame (reads last-frame query results).
     * We compute view/proj here regardless of frustum culling so the
     * proxy depth-only pass sees the same camera as the main scene. */
    sr->stat_occlusion_enabled = (cfg->occlusion_culler != NULL);
    memset(&sr->stat_occlusion, 0, sizeof(sr->stat_occlusion));
    /* occ_begin phase: begin_frame polls last-frame HW queries by scanning
     * the culler's FULL hash table (O(table_cap), not O(active queries <=
     * 256)) — whether that scan is worth an active-list rework is unmeasured;
     * this phase provides the number. */
    uint64_t _t0_occ = jce_time_perf_counter();
    /* Closed occlusion portals act as conservative CPU occluder volumes.  NOT
     * gated on cfg->occlusion_culler: this needs no hardware queries, so it is
     * the only occlusion a GL ES 2.0 / WebGL 1.0 target gets. */
    sr_portal_begin(sr, scene, camera,
                    (cfg->viewport_width > 0 && cfg->viewport_height > 0)
                        ? (float)cfg->viewport_width / (float)cfg->viewport_height
                        : (16.0f / 9.0f));
    if (cfg->occlusion_culler && camera) {
        const jce_mat4 oc_v  = jce_camera_view(camera);
        const float oc_aspect =
            (cfg->viewport_width > 0 && cfg->viewport_height > 0)
                ? (float)cfg->viewport_width / (float)cfg->viewport_height
                : (16.0f / 9.0f);
        const jce_mat4 oc_p  = jce_camera_proj(camera, oc_aspect,
                                                sr->homogeneous_depth);
        /* Query results describe an earlier depth buffer. Any notified scene
         * edit or camera/target change can reveal geometry immediately; never
         * apply that old verdict to a new arrangement. Reuse the culler's
         * pooled reset, not a consumer-specific reset or handle recreation. */
        SrOcclusionHistoryKey history = sr_depth_history_key(sr, scene,
            cfg->occlusion_culler, view_id, cfg->scene_frame_buffer,
            cfg->viewport_width, cfg->viewport_height, &oc_v, &oc_p);
        if (!sr_depth_history_reusable(&sr->occlusion_history, &history,
                                      sr->anim_storage.count != 0))
            jce_occlusion_culler_reset(cfg->occlusion_culler);
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler,
                                          JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
        /* Set the view transform for the proxy pre-pass view so the depth-only
         * proxy draws are in the correct camera space.  Use the culler's OWN
         * view id (default 254) — NOT a hard-coded 254 — so a second culler
         * sharing this engine renderer in the same bgfx frame (editor game-view
         * alongside the scene-view) can drive a distinct view and never clobber
         * the other path's proxy transform. */
        bgfx_set_view_transform(
            jce_occlusion_culler_get_view_id(cfg->occlusion_culler),
            JCE_M4_PTR(oc_v), JCE_M4_PTR(oc_p));
        /* Bind the proxy view to the SAME framebuffer the color/depth pass
         * renders into (cfg->scene_frame_buffer: editor bridge FBO, runtime
         * postfx offscreen FBO, or UINT16_MAX backbuffer).  Without this, every
         * OFFSCREEN path's proxy boxes test a stale/empty backbuffer depth →
         * occlusion either inert (passes everything) or false-culls visible
         * geometry.  The proxy view id is > the color view id, so bgfx orders it
         * AFTER the color pass; the depth it tests is the current frame's.  Use
         * the color pass's exact viewport rect so the rasterised proxies cover
         * the pixels the depth was written at. */
        {
            uint16_t oc_w = (cfg->viewport_width  > 0 && cfg->viewport_width  <= 0xFFFFu)
                            ? (uint16_t)cfg->viewport_width  : 0;
            uint16_t oc_h = (cfg->viewport_height > 0 && cfg->viewport_height <= 0xFFFFu)
                            ? (uint16_t)cfg->viewport_height : 0;
            jce_occlusion_culler_bind_target(cfg->occlusion_culler,
                                             cfg->scene_frame_buffer,
                                             0, 0, oc_w, oc_h);
        }
    } else if (cfg->occlusion_culler) {
        sr->occlusion_history.valid = false;
        jce_occlusion_culler_reset(cfg->occlusion_culler);
        jce_occlusion_culler_begin_frame(cfg->occlusion_culler, NULL, NULL);
    } else {
        sr->occlusion_history.valid = false;
    }
    if (cfg->occlusion_culler)
        jce_perf_phase_add("occ_begin", jce_time_perf_to_ms(_t0_occ,
                                            jce_time_perf_counter()));
}
