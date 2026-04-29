/*
 * jce_subsystem.c  Subsystem registry implementation.
 *
 * Maintains a flat array of descriptors, sorted by priority
 * before init_all().
 */

#include <jce/application/jce_subsystem.h>
#include <jce/os/core/jce_allocator.h>
#include <jce/os/core/jce_log.h>

#include <string.h>

#define LOG_TAG "subsystem"

#define INITIAL_CAP 16u

struct jce_subsystem_registry {
    jce_allocator_t      alloc;
    jce_subsystem_desc_t *descs;
    uint32_t              count;
    uint32_t              capacity;
    bool                  initialised;  /* true after init_all succeeded */
};

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

jce_subsystem_registry_t *jce_subsystem_registry_create(jce_allocator_t alloc)
{
    jce_subsystem_registry_t *r = (jce_subsystem_registry_t *)
        alloc.alloc(sizeof(jce_subsystem_registry_t), alloc.ctx);
    if (!r) return NULL;

    r->alloc       = alloc;
    r->count       = 0;
    r->capacity    = INITIAL_CAP;
    r->initialised = false;

    r->descs = (jce_subsystem_desc_t *)alloc.alloc(
        sizeof(jce_subsystem_desc_t) * INITIAL_CAP, alloc.ctx);
    if (!r->descs) {
        alloc.free(r, alloc.ctx);
        return NULL;
    }

    return r;
}

void jce_subsystem_registry_destroy(jce_subsystem_registry_t *reg)
{
    if (!reg) return;
    jce_allocator_t a = reg->alloc;
    a.free(reg->descs, a.ctx);
    a.free(reg, a.ctx);
}

/* ================================================================== */
/* Register                                                            */
/* ================================================================== */

bool jce_subsystem_register(jce_subsystem_registry_t *reg,
                            const jce_subsystem_desc_t *desc)
{
    if (!reg || !desc || !desc->name) return false;
    if (reg->initialised) {
        LOG_WARN(LOG_TAG, "cannot register '%s' — already initialised", desc->name);
        return false;
    }

    /* Grow if needed. */
    if (reg->count == reg->capacity) {
        uint32_t new_cap = reg->capacity * 2;
        jce_subsystem_desc_t *tmp = (jce_subsystem_desc_t *)
            reg->alloc.realloc(reg->descs,
                sizeof(jce_subsystem_desc_t) * new_cap,
                reg->alloc.ctx);
        if (!tmp) return false;
        reg->descs    = tmp;
        reg->capacity = new_cap;
    }

    reg->descs[reg->count++] = *desc;
    LOG_INFO(LOG_TAG, "registered '%s' (priority %d)", desc->name, desc->priority);
    return true;
}

/* ================================================================== */
/* Insertion-sort by priority (small N, stable)                        */
/* ================================================================== */

static void sort_by_priority(jce_subsystem_desc_t *arr, uint32_t n)
{
    for (uint32_t i = 1; i < n; i++) {
        jce_subsystem_desc_t tmp = arr[i];
        uint32_t j = i;
        while (j > 0 && arr[j - 1].priority > tmp.priority) {
            arr[j] = arr[j - 1];
            j--;
        }
        arr[j] = tmp;
    }
}

/* ================================================================== */
/* Init / Update / Shutdown                                            */
/* ================================================================== */

bool jce_subsystem_init_all(jce_subsystem_registry_t *reg,
                            const JceServices *svc)
{
    if (!reg || reg->initialised) return false;

    sort_by_priority(reg->descs, reg->count);

    for (uint32_t i = 0; i < reg->count; i++) {
        jce_subsystem_desc_t *d = &reg->descs[i];
        LOG_INFO(LOG_TAG, "init '%s' ...", d->name);
        if (d->init && !d->init(svc, d->ctx)) {
            LOG_ERROR(LOG_TAG, "'%s' init failed — aborting", d->name);
            /* Quiesce + shutdown already-initialised subsystems in reverse. */
            for (uint32_t j = i; j-- > 0; ) {
                if (reg->descs[j].quiesce)
                    reg->descs[j].quiesce(reg->descs[j].ctx);
            }
            for (uint32_t j = i; j-- > 0; ) {
                if (reg->descs[j].shutdown)
                    reg->descs[j].shutdown(reg->descs[j].ctx);
            }
            return false;
        }
    }

    reg->initialised = true;
    LOG_INFO(LOG_TAG, "all %u subsystems initialised", reg->count);
    return true;
}

void jce_subsystem_update_all(jce_subsystem_registry_t *reg, float dt)
{
    if (!reg || !reg->initialised) return;
    for (uint32_t i = 0; i < reg->count; i++) {
        if (reg->descs[i].update)
            reg->descs[i].update(dt, reg->descs[i].ctx);
    }
}

void jce_subsystem_shutdown_all(jce_subsystem_registry_t *reg)
{
    if (!reg || !reg->initialised) return;

    /* Quiesce all (reverse priority) so no new work spawns mid-teardown. */
    for (uint32_t i = reg->count; i-- > 0; ) {
        jce_subsystem_desc_t *d = &reg->descs[i];
        if (d->quiesce) d->quiesce(d->ctx);
    }

    /* Reverse priority order. */
    for (uint32_t i = reg->count; i-- > 0; ) {
        jce_subsystem_desc_t *d = &reg->descs[i];
        LOG_INFO(LOG_TAG, "shutdown '%s' ...", d->name);
        if (d->shutdown)
            d->shutdown(d->ctx);
    }

    reg->initialised = false;
}
