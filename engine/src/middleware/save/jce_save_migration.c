/*
 * jce_save_migration.c — Central save-migration registry implementation.
 *
 * Ordered single-step transforms over a section's parsed JSON, keyed by
 * (section id, from_version).  jce_save_migrate() walks the chain from the
 * loaded version up to the provider's current version.  See the header for
 * the contract (no-op at current version, hard failure on a missing step).
 */
#include <jce/middleware/save/jce_save_migration.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <string.h>

#define LOG_TAG "save_migrate"

/* ================================================================== */
/* Registry                                                            */
/* ================================================================== */
typedef struct {
    char            *id;
    uint32_t         from_version;
    uint32_t         to_version;
    JceSaveMigrateFn fn;
    void            *user;
} MigStep;

struct JceSaveMigrationRegistry {
    MigStep *items;
    uint32_t count;
    uint32_t cap;
};

JceSaveMigrationRegistry *jce_save_migration_registry_create(void)
{
    JceSaveMigrationRegistry *r =
        (JceSaveMigrationRegistry *)JCE_MALLOC(sizeof(*r));
    if (!r) return NULL;
    memset(r, 0, sizeof(*r));
    return r;
}

void jce_save_migration_registry_destroy(JceSaveMigrationRegistry *r)
{
    if (!r) return;
    for (uint32_t i = 0; i < r->count; ++i) JCE_FREE(r->items[i].id);
    JCE_FREE(r->items);
    JCE_FREE(r);
}

/* Locate the unique step for (id, from_version), or -1. */
static int find_step(const JceSaveMigrationRegistry *r,
                     const char *id, uint32_t from_version)
{
    for (uint32_t i = 0; i < r->count; ++i)
        if (r->items[i].from_version == from_version &&
            strcmp(r->items[i].id, id) == 0)
            return (int)i;
    return -1;
}

bool jce_save_migration_register(JceSaveMigrationRegistry *r,
                                 const char       *id,
                                 uint32_t          from_version,
                                 uint32_t          to_version,
                                 JceSaveMigrateFn  fn,
                                 void             *user)
{
    if (!r || !id || !fn) return false;
    if (to_version <= from_version) {
        LOG_ERROR(LOG_TAG, " '%s': step v%u->v%u does not advance the version",
                  id, from_version, to_version);
        return false;
    }
    if (find_step(r, id, from_version) >= 0) {
        LOG_ERROR(LOG_TAG, " '%s': duplicate migration step for from-version %u",
                  id, from_version);
        return false;
    }

    if (r->count == r->cap) {
        uint32_t nc = r->cap ? r->cap * 2 : 8;
        MigStep *ni = (MigStep *)JCE_REALLOC(r->items, sizeof(MigStep) * nc);
        if (!ni) return false;
        r->items = ni;
        r->cap   = nc;
    }
    MigStep *st = &r->items[r->count++];
    size_t n = strlen(id);
    st->id = (char *)JCE_MALLOC(n + 1);
    if (!st->id) { r->count--; return false; }
    memcpy(st->id, id, n + 1);
    st->from_version = from_version;
    st->to_version   = to_version;
    st->fn           = fn;
    st->user         = user;
    return true;
}

bool jce_save_migrate(JceSaveMigrationRegistry *r,
                      const char *id,
                      uint32_t    from_version,
                      uint32_t    to_version,
                      JceJson    *json)
{
    if (!id || !json) return false;

    /* Current-version load: strict no-op, zero lookups. */
    if (from_version == to_version) return true;

    if (from_version > to_version) {
        LOG_ERROR(LOG_TAG,
                  " '%s': cannot downgrade save v%u to v%u (future save?)",
                  id, from_version, to_version);
        return false;
    }

    /* No registry at all but data is older than current → cannot migrate. */
    if (!r) {
        LOG_ERROR(LOG_TAG,
                  " '%s': no migration registry; cannot upgrade v%u to v%u",
                  id, from_version, to_version);
        return false;
    }

    /* Walk the chain one step at a time. */
    uint32_t cur = from_version;
    while (cur < to_version) {
        int idx = find_step(r, id, cur);
        if (idx < 0) {
            LOG_ERROR(LOG_TAG,
                      " '%s': missing migration step from version %u "
                      "(target %u) — refusing to load",
                      id, cur, to_version);
            return false;
        }
        MigStep *st = &r->items[idx];
        if (st->to_version > to_version) {
            LOG_ERROR(LOG_TAG,
                      " '%s': step v%u->v%u overshoots target v%u",
                      id, cur, st->to_version, to_version);
            return false;
        }
        if (!st->fn(json, st->user)) {
            LOG_ERROR(LOG_TAG,
                      " '%s': migration step v%u->v%u failed",
                      id, cur, st->to_version);
            return false;
        }
        cur = st->to_version;
    }

    return cur == to_version;
}
