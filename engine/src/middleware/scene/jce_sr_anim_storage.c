#include "jce_sr_anim_storage.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_hashmap.h>
#include <jce/os/core/jce_hash.h>
#include <string.h>

static uint32_t home(uint64_t entity, uint32_t capacity)
{
    return (uint32_t)jce_hash_fmix64(entity) & (capacity - 1u);
}

static uint32_t bucket(const SrAnimStorage *s, uint64_t entity)
{
    uint32_t h = home(entity, s->index_capacity);
    for (uint32_t n = 0; n < s->index_capacity; ++n) {
        uint32_t slot = s->index[h];
        if (!slot || s->records[slot - 1u].entity == entity) return h;
        h = (h + 1u) & (s->index_capacity - 1u);
    }
    return UINT32_MAX;
}

void *sr_anim_storage_find(const SrAnimStorage *s, uint64_t entity)
{
    if (!s || !s->index_capacity) return NULL;
    uint32_t h = bucket(s, entity);
    return h != UINT32_MAX && s->index[h] ? s->records[s->index[h] - 1u].value : NULL;
}

static bool reserve(SrAnimStorage *s)
{
    if (s->count < s->capacity) return true;
    /* Keep at most half the index occupied and guard all doubling/byte math. */
    if (s->capacity >= 0x40000000u) return false;
    uint32_t cap = jce_hashmap_cap_pow2(s->count + 1u, 16u);
    uint32_t index_cap = cap * 2u;
    if ((uint64_t)cap * sizeof(SrAnimRecord) > SIZE_MAX ||
        (uint64_t)index_cap * sizeof(uint32_t) > SIZE_MAX) return false;
    uint32_t *index = JCE_CALLOC(index_cap, sizeof(*index));
    if (!index) return false;
    SrAnimRecord *records = JCE_REALLOC(s->records, (size_t)cap * sizeof(*records));
    if (!records) { JCE_FREE(index); return false; }
    JCE_FREE(s->index);
    s->records = records;
    s->capacity = cap;
    s->index = index;
    s->index_capacity = index_cap;
    for (uint32_t i = 0; i < s->count; ++i)
        s->index[bucket(s, s->records[i].entity)] = i + 1u;
    return true;
}

void *sr_anim_storage_add(SrAnimStorage *s, uint64_t entity, size_t size)
{
    if (!s || !size) return NULL;
    void *value = sr_anim_storage_find(s, entity);
    if (value) return value;
    if (!reserve(s)) return NULL;
    value = JCE_CALLOC(1u, size);
    if (!value) return NULL;
    uint32_t h = bucket(s, entity);
    if (h == UINT32_MAX) { JCE_FREE(value); return NULL; }
    s->records[s->count] = (SrAnimRecord){entity, value};
    s->index[h] = ++s->count;
    return value;
}

bool sr_anim_storage_remove(SrAnimStorage *s, uint64_t entity, SrAnimRelease release)
{
    if (!s || !s->index_capacity) return false;
    uint32_t hole = bucket(s, entity);
    if (hole == UINT32_MAX || !s->index[hole]) return false;
    uint32_t slot = s->index[hole] - 1u;
    void *value = s->records[slot].value;
    if (release) release(value);
    JCE_FREE(value);
    s->index[hole] = 0;
    /* Repair the probe chain BEFORE compacting dense records: bucket() still
     * resolves every live index against the original record positions. */
    uint32_t probe = (hole + 1u) & (s->index_capacity - 1u);
    while (s->index[probe]) {
        uint64_t key = s->records[s->index[probe] - 1u].entity;
        if (jce_hashmap_shift_back(home(key, s->index_capacity), hole,
                                   probe, s->index_capacity)) {
            s->index[hole] = s->index[probe];
            s->index[probe] = 0;
            hole = probe;
        }
        probe = (probe + 1u) & (s->index_capacity - 1u);
    }
    uint32_t last = --s->count;
    if (slot != last) {
        uint32_t h = bucket(s, s->records[last].entity);
        s->records[slot] = s->records[last];
        s->index[h] = slot + 1u;
    }
    return true;
}

void sr_anim_storage_destroy(SrAnimStorage *s, SrAnimRelease release)
{
    if (!s) return;
    for (uint32_t i = 0; i < s->count; ++i) {
        if (release) release(s->records[i].value);
        JCE_FREE(s->records[i].value);
    }
    JCE_FREE(s->records);
    JCE_FREE(s->index);
    memset(s, 0, sizeof(*s));
}
