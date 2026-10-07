/* Conservative validity of hardware-query depth history. */
#ifndef JCE_SR_OCCLUSION_HISTORY_H
#define JCE_SR_OCCLUSION_HISTORY_H
#include "jce_sr_metadata_key.h"
#include <string.h>

typedef struct {
    SrMetadataKey metadata;
    const void *culler;
    uint64_t transform_generation;
    uint64_t content_generation;
    uint32_t width, height, camera_mask;
    uint16_t frame_buffer, view_id;
    float view[16], projection[16];
} SrOcclusionHistoryKey;

typedef struct {
    SrOcclusionHistoryKey key;
    bool valid;
} SrOcclusionHistory;

static inline bool sr_occlusion_history_update(SrOcclusionHistory *history,
                                                const SrOcclusionHistoryKey *next)
{
    if (!history) return true;
    if (!next || !next->metadata.scene || !next->culler) {
        history->valid = false;
        return true;
    }
    const SrOcclusionHistoryKey *prior = &history->key;
    bool changed = !history->valid ||
        !sr_metadata_key_matches(&prior->metadata, &next->metadata) ||
        prior->culler != next->culler ||
        prior->transform_generation != next->transform_generation ||
        prior->content_generation != next->content_generation ||
        prior->width != next->width || prior->height != next->height ||
        prior->camera_mask != next->camera_mask ||
        prior->frame_buffer != next->frame_buffer || prior->view_id != next->view_id ||
        memcmp(prior->view, next->view, sizeof(next->view)) ||
        memcmp(prior->projection, next->projection, sizeof(next->projection));
    history->key = *next;
    history->valid = true;
    return changed;
}

/* Unnotified deformation has no reusable depth identity. Invalidate it so
 * stopping animation still needs a fresh depth producer before reuse. */
static inline bool sr_depth_history_reusable(SrOcclusionHistory *history,
    const SrOcclusionHistoryKey *next, bool deformed)
{
    bool changed = sr_occlusion_history_update(history, next);
    if (deformed && history) history->valid = false;
    return !changed && !deformed;
}
#endif
