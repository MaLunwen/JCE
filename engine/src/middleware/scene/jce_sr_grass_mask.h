/* Private, synchronous grass mask memo. The opaque batch owns one entry. */
#ifndef JCE_SR_GRASS_MASK_H
#define JCE_SR_GRASS_MASK_H

#include <stdbool.h>
#include <stdint.h>

typedef struct SrGrassMaskBatch {
    struct SrGrassMaskEntry *entry;
} SrGrassMaskBatch;

typedef struct {
    const float *cells;
    int dim;
    uint32_t content_hash;
    bool owned;
} SrGrassMaskLease;

typedef enum {
    SR_GRASS_MASK_UNAVAILABLE = 0,
    SR_GRASS_MASK_ALLOCATION_FAILED,
    SR_GRASS_MASK_DECODED,
    SR_GRASS_MASK_REUSED,
    SR_GRASS_MASK_DECODED_UNCACHED
} SrGrassMaskStatus;

struct JceSceneRenderer;

bool sr_grass_mask_batch_enabled(void);
SrGrassMaskStatus sr_grass_mask_acquire(struct JceSceneRenderer *sr,
    SrGrassMaskBatch *batch, const char *path, float threshold, SrGrassMaskLease *lease);
void sr_grass_mask_release(SrGrassMaskLease *lease);
void sr_grass_mask_batch_end(SrGrassMaskBatch *batch);

#endif
