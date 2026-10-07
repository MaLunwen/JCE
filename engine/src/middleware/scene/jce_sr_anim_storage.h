/* Renderer-owned animation records. Payload addresses survive table growth. */
#ifndef JCE_SR_ANIM_STORAGE_H
#define JCE_SR_ANIM_STORAGE_H
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

typedef struct {
    uint64_t entity;
    void *value;
} SrAnimRecord;

typedef struct {
    SrAnimRecord *records;
    uint32_t count;
    uint32_t capacity;
    uint32_t *index;
    uint32_t index_capacity;
} SrAnimStorage;

typedef void (*SrAnimRelease)(void *value);

void *sr_anim_storage_find(const SrAnimStorage *storage, uint64_t entity);
void *sr_anim_storage_add(SrAnimStorage *storage, uint64_t entity, size_t size);
bool sr_anim_storage_remove(SrAnimStorage *storage, uint64_t entity, SrAnimRelease release);
void sr_anim_storage_destroy(SrAnimStorage *storage, SrAnimRelease release);
#endif
