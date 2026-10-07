/* Dense-index membership for full builds and incremental renderer repairs. */
#ifndef JCE_SR_UNIQUE_INDEX_H
#define JCE_SR_UNIQUE_INDEX_H

#include <stdbool.h>
#include <stdint.h>

/* A failed append leaves the stamp unchanged, so a later retry can succeed.
 * The owner advances a nonzero epoch when rebuilding the indexed list. */
static inline bool sr_unique_index_append(uint32_t *indices, uint32_t *count,
                                         uint32_t capacity, uint32_t index,
                                         uint64_t epoch, uint64_t *stamp)
{
    if (!indices || !count || !stamp || epoch == 0 || *count > capacity)
        return false;
    if (*stamp == epoch)
        return true;
    if (*count == capacity)
        return false;
    indices[(*count)++] = index;
    *stamp = epoch;
    return true;
}

#endif /* JCE_SR_UNIQUE_INDEX_H */
