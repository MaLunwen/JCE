/* Transform-independent entity classification cache identity. */
#ifndef JCE_SR_METADATA_KEY_H
#define JCE_SR_METADATA_KEY_H

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    const void *scene;
    uint64_t list_generation;
    uint64_t structural_generation;
    uint64_t enable_generation;
    uint64_t component_generation;
} SrMetadataKey;

static inline bool sr_metadata_key_matches(const SrMetadataKey *a,
                                           const SrMetadataKey *b)
{
    return a && b && a->scene && a->scene == b->scene &&
           a->list_generation == b->list_generation &&
           a->structural_generation == b->structural_generation &&
           a->enable_generation == b->enable_generation &&
           a->component_generation == b->component_generation;
}

#endif
