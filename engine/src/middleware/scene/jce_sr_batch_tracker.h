/*
 * jce_sr_batch_tracker.h
 *
 * O(1) frame-local test for batches that contain exactly one draw-group key.
 * Kept pure so the renderer fast-path decision is unit-testable without bgfx.
 */
#ifndef JCE_SR_BATCH_TRACKER_H
#define JCE_SR_BATCH_TRACKER_H

#include <stdbool.h>
#include <stdint.h>

typedef struct JceSrBatchTracker {
    uintptr_t owner;
    uint32_t variant;
    uintptr_t last_owner;
    uint32_t last_variant;
    uint32_t flags;
    uint32_t count;
    bool single_group;
    bool ordered;
} JceSrBatchTracker;

static inline void jce_sr_batch_tracker_reset(JceSrBatchTracker *tracker)
{
    if (!tracker)
        return;

    tracker->owner = 0u;
    tracker->variant = 0u;
    tracker->last_owner = 0u;
    tracker->last_variant = 0u;
    tracker->flags = 0u;
    tracker->count = 0u;
    tracker->single_group = true;
    tracker->ordered = true;
}

static inline void jce_sr_batch_tracker_push(
    JceSrBatchTracker *tracker, uintptr_t owner, uint32_t variant,
    uint32_t flags)
{
    if (!tracker)
        return;

    if (tracker->count == 0u) {
        tracker->owner = owner;
        tracker->variant = variant;
        tracker->last_owner = owner;
        tracker->last_variant = variant;
        tracker->single_group = true;
    } else {
        if (tracker->owner != owner || tracker->variant != variant)
            tracker->single_group = false;
        if (owner < tracker->last_owner ||
            (owner == tracker->last_owner &&
             variant < tracker->last_variant)) {
            tracker->ordered = false;
        }
        tracker->last_owner = owner;
        tracker->last_variant = variant;
    }
    tracker->flags |= flags;
    tracker->count++;
}

static inline bool jce_sr_batch_tracker_single_group(
    const JceSrBatchTracker *tracker)
{
    return tracker && tracker->count > 0u && tracker->single_group;
}

static inline bool jce_sr_batch_tracker_ordered(
    const JceSrBatchTracker *tracker)
{
    return tracker && tracker->count > 0u && tracker->ordered;
}

static inline uint32_t jce_sr_batch_tracker_flags(
    const JceSrBatchTracker *tracker)
{
    return tracker ? tracker->flags : 0u;
}

#endif /* JCE_SR_BATCH_TRACKER_H */
