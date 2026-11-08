#ifndef JCE_SR_MODEL_DEPS_H
#define JCE_SR_MODEL_DEPS_H

#include <stdbool.h>
#include <stdint.h>

struct SrModelDepPath;
struct SrModelDepRow;

typedef struct {
    struct SrModelDepPath *paths;
    struct SrModelDepRow *rows;
    uint32_t *path_index;
    uint32_t *row_index;
    uint32_t path_count, path_capacity;
    uint32_t row_count, row_capacity;
    uint32_t queue_head;
    uint64_t epoch;
    bool fallback;
} SrModelDeps;

typedef struct {
    uint64_t revision;
    bool ready, has_aabb;
    float min[3], max[3];
} SrModelDepEvent;

typedef enum {
    SR_MODEL_DEP_REJECTED,
    SR_MODEL_DEP_UNCHANGED,
    SR_MODEL_DEP_CHANGED,
    SR_MODEL_DEP_FALLBACK
} SrModelDepResult;

typedef bool (*SrModelDepVisit)(void *user, uint64_t entity, const char *path,
                                const SrModelDepEvent *event);

/* Zero-init once. The caller gives this renderer's successive world/content
 * scopes strictly increasing nonzero epochs. Reset drops subscriptions and
 * pending events. Destroy is idempotent and ends the owner lifetime. */
bool sr_model_deps_reset(SrModelDeps *deps, uint64_t epoch);
void sr_model_deps_destroy(SrModelDeps *deps);

/* Owner-thread only. Entity keys include their generation. Path identity is
 * the exact model-cache input bytes, independently owned by this roster. */
SrModelDepResult sr_model_deps_bind(SrModelDeps *deps, uint64_t epoch,
                                    uint64_t entity, const char *path);
SrModelDepResult sr_model_deps_remove(SrModelDeps *deps, uint64_t epoch,
                                      uint64_t entity);

typedef bool (*SrModelDepAlive)(void *user, uint64_t entity);

/* Call on scene roster epoch change, before draining pending model events.
 * This owner-thread sweep has no allocation. The predicate checks the full
 * entity key and must not reenter the owner. Path records and pending events
 * are retained. */
SrModelDepResult sr_model_deps_prune(SrModelDeps *deps, uint64_t epoch,
                                     SrModelDepAlive is_alive, void *user);

/* No allocation. Only already tracked paths can receive events. The current
 * model lookup still supplies initial state to a newly bound row. Revisions
 * are ordered per path. Same-revision disagreement is an owner error. */
SrModelDepResult sr_model_deps_publish(SrModelDeps *deps, uint64_t epoch,
                                      const char *path, const SrModelDepEvent *event);

/* Visit only rows linked to queued paths. A row receives the newest event
 * for its current path at most once in this drain. Visit order is unspecified.
 * The visitor must
 * not reenter this owner or retain pointers. Any visitor refusal, capacity
 * failure or inconsistent publication latches fallback until owner reset.
 * The renderer must invalidate/rebuild if any partial updates were applied. */
SrModelDepResult sr_model_deps_drain(SrModelDeps *deps, uint64_t epoch,
                                    SrModelDepVisit visit, void *user);

#endif
