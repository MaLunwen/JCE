#include "jce_sr_model_deps.h"

#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_hashmap.h>

#include <stddef.h>
#include <string.h>

#if defined(SR_MODEL_DEPS_TEST)
void *sr_model_deps_test_malloc(size_t bytes);
void *sr_model_deps_test_calloc(size_t count, size_t bytes);
void *sr_model_deps_test_realloc(void *ptr, size_t bytes);
void sr_model_deps_test_free(void *ptr);
void sr_model_deps_test_row_examined(uint64_t entity);
#define JCE_MALLOC sr_model_deps_test_malloc
#define JCE_CALLOC sr_model_deps_test_calloc
#define JCE_REALLOC sr_model_deps_test_realloc
#define JCE_FREE sr_model_deps_test_free
#else
#include "os/core/jce_memory.h"
#endif

#ifndef SR_MODEL_DEP_MAX_PATHS
#define SR_MODEL_DEP_MAX_PATHS 8192u
#endif
#ifndef SR_MODEL_DEP_MAX_ROWS
#define SR_MODEL_DEP_MAX_ROWS 1048576u
#endif

struct SrModelDepPath {
    char *key;
    uint32_t hash, queue_next, row_head;
    SrModelDepEvent event;
    bool queued;
};

struct SrModelDepRow {
    uint64_t entity;
    uint32_t path, previous, next;
};

static void *grow(void *items, uint32_t **index, uint32_t *capacity,
                  uint32_t limit, size_t width)
{
    if (*capacity >= limit) return NULL;
    uint32_t next = jce_hashmap_cap_pow2(*capacity + 1u, 8u);
    if (next > limit || next > UINT32_MAX / 2u || next > SIZE_MAX / width)
        return NULL;
#if SIZE_MAX <= UINT32_MAX
    if (2u * next > SIZE_MAX / sizeof(uint32_t)) return NULL;
#endif
    uint32_t *new_index = JCE_CALLOC(2u * next, sizeof(*new_index));
    if (!new_index) return NULL;
    void *new_items = JCE_REALLOC(items, (size_t)next * width);
    if (!new_items) {
        JCE_FREE(new_index);
        return NULL;
    }
    JCE_FREE(*index);
    *index = new_index;
    *capacity = next;
    return new_items;
}

static uint32_t path_bucket(const SrModelDeps *deps, const char *key, uint32_t hash)
{
    const uint32_t mask = 2u * deps->path_capacity - 1u;
    uint32_t slot = hash & mask;
    while (deps->path_index[slot]) {
        const struct SrModelDepPath *entry = &deps->paths[deps->path_index[slot] - 1u];
        if (entry->hash == hash && strcmp(entry->key, key) == 0) break;
        slot = (slot + 1u) & mask;
    }
    return slot;
}

static uint32_t row_home(uint64_t entity, uint32_t index_capacity)
{
    return (uint32_t)jce_hash_fmix64(entity) & (index_capacity - 1u);
}

static uint32_t row_bucket(const SrModelDeps *deps, uint64_t entity)
{
    const uint32_t capacity = 2u * deps->row_capacity;
    uint32_t slot = row_home(entity, capacity);
    while (deps->row_index[slot] &&
           deps->rows[deps->row_index[slot] - 1u].entity != entity)
        slot = (slot + 1u) & (capacity - 1u);
    return slot;
}

/* Stable, one-based path IDs for one owner epoch. No model slots are borrowed. */
static uint32_t intern_path(SrModelDeps *deps, const char *key)
{
    const uint32_t hash = jce_fnv1a32_str(key);
    if (deps->path_capacity) {
        uint32_t id = deps->path_index[path_bucket(deps, key, hash)];
        if (id) return id;
    }
    if (deps->path_count == deps->path_capacity) {
        struct SrModelDepPath *paths = grow(deps->paths, &deps->path_index,
            &deps->path_capacity, SR_MODEL_DEP_MAX_PATHS, sizeof(*paths));
        if (!paths) return 0;
        deps->paths = paths;
        for (uint32_t i = 0; i < deps->path_count; ++i) {
            const struct SrModelDepPath *entry = &paths[i];
            deps->path_index[path_bucket(deps, entry->key, entry->hash)] = i + 1u;
        }
    }
    const size_t bytes = strlen(key) + 1u;
    if (bytes > 256u) return 0; /* Current model-cache key limit. */
    char *copy = JCE_MALLOC(bytes);
    if (!copy) return 0;
    memcpy(copy, key, bytes);
    deps->paths[deps->path_count] = (struct SrModelDepPath){.key = copy, .hash = hash};
    const uint32_t slot = path_bucket(deps, key, hash);
    deps->path_index[slot] = ++deps->path_count;
    return deps->path_count;
}

static bool grow_rows(SrModelDeps *deps)
{
    struct SrModelDepRow *rows = grow(deps->rows, &deps->row_index,
        &deps->row_capacity, SR_MODEL_DEP_MAX_ROWS, sizeof(*rows));
    if (!rows) return false;
    deps->rows = rows;
    for (uint32_t i = 0; i < deps->row_count; ++i)
        deps->row_index[row_bucket(deps, rows[i].entity)] = i + 1u;
    return true;
}

static bool admits(const SrModelDeps *deps, uint64_t epoch)
{
    return deps && epoch && deps->epoch == epoch;
}

static SrModelDepResult fallback(SrModelDeps *deps)
{
    deps->fallback = true;
    return SR_MODEL_DEP_FALLBACK;
}

void sr_model_deps_destroy(SrModelDeps *deps)
{
    if (!deps) return;
    for (uint32_t i = 0; i < deps->path_count; ++i)
        JCE_FREE(deps->paths[i].key);
    JCE_FREE(deps->paths);
    JCE_FREE(deps->rows);
    JCE_FREE(deps->path_index);
    JCE_FREE(deps->row_index);
    *deps = (SrModelDeps){0};
}

bool sr_model_deps_reset(SrModelDeps *deps, uint64_t epoch)
{
    if (!deps || epoch <= deps->epoch) return false;
    sr_model_deps_destroy(deps);
    deps->epoch = epoch;
    return true;
}

/* Link IDs are one-based dense indexes. Rebinding and removing update only the
 * affected chain. Swap-removal repairs links to the relocated last row. */
static void relink_neighbors(SrModelDeps *deps, const struct SrModelDepRow *row,
                             uint32_t id)
{
    if (row->previous)
        deps->rows[row->previous - 1u].next = id ? id : row->next;
    else
        deps->paths[row->path - 1u].row_head = id ? id : row->next;
    if (row->next)
        deps->rows[row->next - 1u].previous = id ? id : row->previous;
}

static void link_row(SrModelDeps *deps, uint32_t id, uint32_t path)
{
    struct SrModelDepRow *row = &deps->rows[id - 1u];
    row->path = path;
    row->previous = 0;
    row->next = deps->paths[path - 1u].row_head;
    relink_neighbors(deps, row, id);
}

SrModelDepResult sr_model_deps_bind(SrModelDeps *deps, uint64_t epoch,
                                   uint64_t entity, const char *path)
{
    if (!admits(deps, epoch) || !entity || !path || !path[0])
        return SR_MODEL_DEP_REJECTED;
    if (deps->fallback) return SR_MODEL_DEP_FALLBACK;

    const uint32_t existing = deps->row_capacity
        ? deps->row_index[row_bucket(deps, entity)] : 0u;
    if (existing) {
        const struct SrModelDepRow *row = &deps->rows[existing - 1u];
        if (strcmp(deps->paths[row->path - 1u].key, path) == 0)
            return SR_MODEL_DEP_UNCHANGED;
    }
    const uint32_t path_id = intern_path(deps, path);
    if (!path_id) return fallback(deps);
    if (existing) {
        relink_neighbors(deps, &deps->rows[existing - 1u], 0);
        link_row(deps, existing, path_id);
        return SR_MODEL_DEP_CHANGED;
    }
    if (deps->row_count == deps->row_capacity && !grow_rows(deps))
        return fallback(deps);
    const uint32_t slot = row_bucket(deps, entity);
    deps->rows[deps->row_count] = (struct SrModelDepRow){.entity = entity};
    deps->row_index[slot] = ++deps->row_count;
    link_row(deps, deps->row_count, path_id);
    return SR_MODEL_DEP_CHANGED;
}

SrModelDepResult sr_model_deps_remove(SrModelDeps *deps, uint64_t epoch, uint64_t entity)
{
    if (!admits(deps, epoch) || !entity) return SR_MODEL_DEP_REJECTED;
    if (deps->fallback) return SR_MODEL_DEP_FALLBACK;
    if (!deps->row_capacity) return SR_MODEL_DEP_UNCHANGED;
    uint32_t hole = row_bucket(deps, entity);
    if (!deps->row_index[hole]) return SR_MODEL_DEP_UNCHANGED;
    const uint32_t removed = deps->row_index[hole] - 1u;
    const uint32_t capacity = 2u * deps->row_capacity;
    const uint32_t mask = capacity - 1u;
    deps->row_index[hole] = 0;
    for (uint32_t probe = (hole + 1u) & mask; deps->row_index[probe];
         probe = (probe + 1u) & mask) {
        const uint32_t row = deps->row_index[probe];
        if (jce_hashmap_shift_back(row_home(deps->rows[row - 1u].entity, capacity),
                                   hole, probe, capacity)) {
            deps->row_index[hole] = row;
            deps->row_index[probe] = 0;
            hole = probe;
        }
    }
    relink_neighbors(deps, &deps->rows[removed], 0);
    const uint32_t last = --deps->row_count;
    if (removed != last) {
        const uint32_t slot = row_bucket(deps, deps->rows[last].entity);
        deps->rows[removed] = deps->rows[last];
        relink_neighbors(deps, &deps->rows[removed], removed + 1u);
        deps->row_index[slot] = removed + 1u;
    }
    return SR_MODEL_DEP_CHANGED;
}

SrModelDepResult sr_model_deps_prune(SrModelDeps *deps, uint64_t epoch,
                                     SrModelDepAlive is_alive, void *user)
{
    if (!admits(deps, epoch)) return SR_MODEL_DEP_REJECTED;
    if (deps->fallback) return SR_MODEL_DEP_FALLBACK;
    if (!is_alive) return SR_MODEL_DEP_REJECTED;

    bool changed = false;
    for (uint32_t i = 0; i < deps->row_count;) {
        const uint64_t entity = deps->rows[i].entity;
        if (is_alive(user, entity)) {
            ++i;
            continue;
        }
        if (sr_model_deps_remove(deps, epoch, entity) != SR_MODEL_DEP_CHANGED)
            return fallback(deps);
        /* Swap-removal may have moved an unchecked tail into this index. */
        changed = true;
    }
    return changed ? SR_MODEL_DEP_CHANGED : SR_MODEL_DEP_UNCHANGED;
}

static bool event_equal(const SrModelDepEvent *a, const SrModelDepEvent *b)
{
    return a->ready == b->ready && a->has_aabb == b->has_aabb &&
        (!a->has_aabb ||
         (memcmp(a->min, b->min, sizeof(a->min)) == 0 &&
          memcmp(a->max, b->max, sizeof(a->max)) == 0));
}

SrModelDepResult sr_model_deps_publish(SrModelDeps *deps, uint64_t epoch,
                                      const char *path, const SrModelDepEvent *event)
{
    if (!admits(deps, epoch) || !path || !path[0] || !event ||
        !event->revision || (event->has_aabb && !event->ready))
        return SR_MODEL_DEP_REJECTED;
    if (deps->fallback) return SR_MODEL_DEP_FALLBACK;
    if (!deps->path_capacity) return SR_MODEL_DEP_UNCHANGED;
    const uint32_t id = deps->path_index[path_bucket(deps, path, jce_fnv1a32_str(path))];
    if (!id) return SR_MODEL_DEP_UNCHANGED;
    struct SrModelDepPath *entry = &deps->paths[id - 1u];
    if (event->revision < entry->event.revision) return SR_MODEL_DEP_REJECTED;
    if (event->revision == entry->event.revision)
        return event_equal(event, &entry->event) ? SR_MODEL_DEP_UNCHANGED : fallback(deps);
    entry->event = (SrModelDepEvent){
        .revision = event->revision, .ready = event->ready, .has_aabb = event->has_aabb
    };
    if (event->has_aabb) {
        memcpy(entry->event.min, event->min, sizeof(event->min));
        memcpy(entry->event.max, event->max, sizeof(event->max));
    }
    if (!entry->queued) {
        entry->queued = true;
        entry->queue_next = deps->queue_head;
        deps->queue_head = id;
    }
    return SR_MODEL_DEP_CHANGED;
}

SrModelDepResult sr_model_deps_drain(SrModelDeps *deps, uint64_t epoch,
                                   SrModelDepVisit visit, void *user)
{
    if (!admits(deps, epoch)) return SR_MODEL_DEP_REJECTED;
    if (deps->fallback) return SR_MODEL_DEP_FALLBACK;
    if (!deps->queue_head) return SR_MODEL_DEP_UNCHANGED;
    if (!visit) return SR_MODEL_DEP_REJECTED;
    for (uint32_t path = deps->queue_head; path; path = deps->paths[path - 1u].queue_next) {
        const struct SrModelDepPath *entry = &deps->paths[path - 1u];
        for (uint32_t id = entry->row_head; id; id = deps->rows[id - 1u].next) {
            const struct SrModelDepRow *row = &deps->rows[id - 1u];
#if defined(SR_MODEL_DEPS_TEST)
            sr_model_deps_test_row_examined(row->entity);
#endif
            if (!visit(user, row->entity, entry->key, &entry->event))
                return fallback(deps);
        }
    }
    while (deps->queue_head) {
        struct SrModelDepPath *entry = &deps->paths[deps->queue_head - 1u];
        deps->queue_head = entry->queue_next;
        entry->queued = false;
        entry->queue_next = 0;
    }
    return SR_MODEL_DEP_CHANGED;
}
