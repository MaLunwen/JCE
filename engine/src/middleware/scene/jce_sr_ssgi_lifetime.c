/* Retire only after complete, observed, unused renderer CPU frames. */
#include "jce_sr_internal.h"

void sr_ssgi_reset(JceSceneRenderer *sr)
{
    if (!sr) return;
    sr->ssgi_active_frame = false;
    sr->ssgi_result_idx = UINT16_MAX;
    sr->ssgi_idle = (SrSsgiIdle){0};
}

void sr_ssgi_release(JceSceneRenderer *sr)
{
    if (!sr) return;
    JceSsgi *module = sr->ssgi;
    sr->ssgi = NULL;
    sr_ssgi_reset(sr);
    if (module) jce_ssgi_destroy(module);
}

void sr_ssgi_retire_unused(JceSceneRenderer *sr, bool reduced, bool wanted)
{
    /* Reflection/probe passes suppress SSGI locally, not for its main view.
     * They must not clear another view's request or advance its idle clock. */
    if (!sr || reduced) return;
    if (!sr->ssgi) {
        sr->ssgi_idle = (SrSsgiIdle){0};
        return;
    }

    SrSsgiIdle *idle = &sr->ssgi_idle;
    const uint32_t frame = jce_renderer_get_frame_index(sr->renderer);
    if (!idle->valid || frame != idle->frame) {
        /* Finalize the PREVIOUS complete observed frame. Requests from all
         * ordinary views in it are ORed. Unknown gaps, reversal and uint32
         * wrap reset the proof; none is evidence of unused frames. */
        if (idle->valid && frame > idle->frame &&
            frame - idle->frame == 1u && !idle->wanted) {
            if (idle->frames < 120u) ++idle->frames;
        } else {
            idle->frames = 0u;
        }
        idle->frame = frame;
        idle->valid = true;
        idle->wanted = false;
    }
    if (wanted) {
        idle->wanted = true;
        idle->frames = 0u;
        return;
    }
    if (idle->frames >= 120u) sr_ssgi_release(sr);
}
