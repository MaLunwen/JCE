/*
 * jce_terrain_foliage.c  Foliage instance pool.
 *
 * Instance storage is a single contiguous array — fine for tens of
 * thousands of items; renderer-side instanced draws bucket on the
 * fly by reading `prototype`.  Brush scatter uses an LCG to keep
 * the result deterministic for a given seed.
 */

#include <jce/middleware/scene/jce_terrain_foliage.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

void jce_foliage_init(JceFoliagePool *p)
{
    if (!p) return;
    memset(p, 0, sizeof(*p));
}

void jce_foliage_dispose(JceFoliagePool *p)
{
    if (!p) return;
    free(p->instances);
    memset(p, 0, sizeof(*p));
}

void jce_foliage_clear(JceFoliagePool *p)
{
    if (!p) return;
    p->instance_count = 0;
}

bool jce_foliage_set_prototype(JceFoliagePool *p, uint16_t idx,
                                const JceFoliagePrototype *proto)
{
    if (!p || !proto || idx >= JCE_FOLIAGE_MAX_PROTOTYPES) return false;
    p->prototypes[idx] = *proto;
    p->prototypes[idx].active = true;
    return true;
}

static bool ensure_capacity(JceFoliagePool *p, uint32_t needed)
{
    if (p->instance_capacity >= needed) return true;
    uint32_t cap = p->instance_capacity ? p->instance_capacity : 64;
    while (cap < needed) cap *= 2;
    JceFoliageInstance *q = (JceFoliageInstance *)realloc(
        p->instances, cap * sizeof(JceFoliageInstance));
    if (!q) return false;
    p->instances        = q;
    p->instance_capacity = cap;
    return true;
}

uint32_t jce_foliage_add_instance(JceFoliagePool *p,
                                    const JceFoliageInstance *inst)
{
    if (!p || !inst) return UINT32_MAX;
    if (!ensure_capacity(p, p->instance_count + 1)) return UINT32_MAX;
    uint32_t idx = p->instance_count++;
    p->instances[idx] = *inst;
    return idx;
}

bool jce_foliage_remove_instance(JceFoliagePool *p, uint32_t idx)
{
    if (!p || idx >= p->instance_count) return false;
    p->instances[idx] = p->instances[p->instance_count - 1];
    p->instance_count--;
    return true;
}

uint32_t jce_foliage_count_for_prototype(const JceFoliagePool *p,
                                           uint16_t prototype)
{
    if (!p) return 0;
    uint32_t n = 0;
    for (uint32_t i = 0; i < p->instance_count; ++i)
        if (p->instances[i].prototype == prototype) n++;
    return n;
}

static uint32_t lcg_next(uint32_t *state)
{
    *state = (*state * 1103515245u) + 12345u;
    return *state;
}

static float lcg_float01(uint32_t *state)
{
    return (float)(lcg_next(state) & 0xFFFFFFu) / (float)0x1000000;
}

uint32_t jce_foliage_brush_scatter(JceFoliagePool *p, uint16_t prototype,
                                     float cx, float cz, float radius,
                                     uint32_t count,
                                     JceFoliageHeightFn height_fn,
                                     void *user_data,
                                     uint32_t random_seed)
{
    if (!p || prototype >= JCE_FOLIAGE_MAX_PROTOTYPES ||
        !p->prototypes[prototype].active) return 0;
    uint32_t state = random_seed ? random_seed : 0x9E3779B9u;
    uint32_t added = 0;
    const JceFoliagePrototype *proto = &p->prototypes[prototype];
    for (uint32_t i = 0; i < count; ++i) {
        /* Rejection-sample inside disk. */
        float u = lcg_float01(&state);
        float v = lcg_float01(&state);
        float ang = u * 6.2831853f;
        float r   = sqrtf(v) * radius;
        float x = cx + r * cosf(ang);
        float z = cz + r * sinf(ang);
        float y = height_fn ? height_fn(x, z, user_data) : 0.0f;

        JceFoliageInstance inst;
        memset(&inst, 0, sizeof(inst));
        inst.position[0] = x;
        inst.position[1] = y;
        inst.position[2] = z;
        inst.rotation_y  = lcg_float01(&state) * 6.2831853f;
        inst.scale = proto->min_scale +
                      lcg_float01(&state) * (proto->max_scale - proto->min_scale);
        inst.prototype  = prototype;
        inst.color_rgba = 0xFFFFFFFFu;
        if (jce_foliage_add_instance(p, &inst) != UINT32_MAX) added++;
    }
    return added;
}
