/*
 * jce_reflection_probe_apply.c  Frame-budgeted probe capture scheduler.
 *
 * Iterates the bake queue, picks the first probe with a pending
 * face, emits a capture job per face up to the per-frame budget.
 * Prioritisation: probes closer to completion (fewer pending faces)
 * are emitted first so they vacate the queue quickly.
 */

#include <jce/renderer/jce_reflection_probe_apply.h>

#include <string.h>

static uint32_t popcount8(uint8_t v)
{
    v = (uint8_t)((v & 0x55u) + ((v >> 1) & 0x55u));
    v = (uint8_t)((v & 0x33u) + ((v >> 2) & 0x33u));
    v = (uint8_t)((v & 0x0Fu) + ((v >> 4) & 0x0Fu));
    return v;
}

uint32_t jce_reflection_probe_apply_emit(uint32_t budget,
                                          JceReflectionCaptureBatch *out)
{
    if (!out) return 0;
    memset(out, 0, sizeof(*out));
    if (budget == 0) return 0;
    if (budget > JCE_REFL_APPLY_PER_FRAME_MAX)
        budget = JCE_REFL_APPLY_PER_FRAME_MAX;

    /* Snapshot probe order by pending-face count (ascending). */
    uint32_t total = jce_reflection_probe_bake_count();
    /* Two-pass: scan all queue entries, emit per-face jobs in order
     * of (popcount(pending_faces_mask), enqueue order). */
    /* Bounded: total ≤ JCE_REFLECTION_PROBE_QUEUE_MAX = 16. */
    JceReflectionProbeBake *snap[JCE_REFLECTION_PROBE_QUEUE_MAX];
    uint32_t snap_n = 0;
    for (uint32_t i = 0; i < total; ++i) {
        JceReflectionProbeBake *p = jce_reflection_probe_bake_at(i);
        if (!p || p->pending_faces_mask == 0) continue;
        snap[snap_n++] = p;
    }
    /* Insertion sort by popcount(mask) ascending. */
    for (uint32_t i = 1; i < snap_n; ++i) {
        JceReflectionProbeBake *cur = snap[i];
        uint32_t cur_pop = popcount8(cur->pending_faces_mask);
        uint32_t j = i;
        while (j > 0 &&
                popcount8(snap[j - 1]->pending_faces_mask) > cur_pop) {
            snap[j] = snap[j - 1];
            j--;
        }
        snap[j] = cur;
    }
    /* Emit. */
    for (uint32_t i = 0; i < snap_n && out->count < budget; ++i) {
        JceReflectionProbeBake *p = snap[i];
        for (uint8_t f = 0; f < 6 && out->count < budget; ++f) {
            if (!(p->pending_faces_mask & (1u << f))) continue;
            JceReflectionCaptureJob *job = &out->jobs[out->count++];
            job->probe_id        = p->probe_id;
            job->face_index      = f;
            job->face_resolution = p->face_resolution;
            job->near_z          = p->near_plane;
            job->far_z           = p->far_plane;
            memcpy(job->view_mat4, p->view[f], sizeof(job->view_mat4));
            memcpy(job->proj_mat4, p->proj,   sizeof(job->proj_mat4));
        }
    }
    return out->count;
}

bool jce_reflection_probe_apply_face_done(uint32_t probe_id, uint8_t face)
{
    return jce_reflection_probe_bake_mark_face_done(probe_id, face);
}

void jce_reflection_probe_apply_drain_done(void)
{
    jce_reflection_probe_bake_drain_done();
}
