/*
 * jce_volume_stack_manager.c  Multi-volume PostFX resolver.
 *
 * Resolution algorithm (matches HDRP Volume.Resolve):
 *   1. Sort overlapping volumes by priority ascending (lower first
 *      so higher-priority lands on top).
 *   2. For each, compute weight = falloff(distance_to_volume) ∈ [0,1].
 *   3. result = blend(result, volume.stack, weight).
 *
 * Global volumes always contribute with weight 1.0 first.
 */

#include <jce/middleware/scene/jce_volume_stack_manager.h>

#include <math.h>
#include <stdlib.h>
#include <string.h>

static JceVolumeStackEntry s_entries[JCE_VOLUME_STACK_MAX];

void jce_volume_stack_clear(void)
{
    memset(s_entries, 0, sizeof(s_entries));
}

uint32_t jce_volume_stack_register(const JceVolumeStackEntry *e)
{
    if (!e) return UINT32_MAX;
    for (int i = 0; i < JCE_VOLUME_STACK_MAX; ++i) {
        if (!s_entries[i].active) {
            s_entries[i] = *e;
            s_entries[i].active = true;
            return (uint32_t)i;
        }
    }
    return UINT32_MAX;
}

bool jce_volume_stack_remove(uint32_t idx)
{
    if (idx >= JCE_VOLUME_STACK_MAX) return false;
    if (!s_entries[idx].active) return false;
    memset(&s_entries[idx], 0, sizeof(s_entries[idx]));
    return true;
}

uint32_t jce_volume_stack_count(void)
{
    uint32_t n = 0;
    for (int i = 0; i < JCE_VOLUME_STACK_MAX; ++i)
        if (s_entries[i].active) n++;
    return n;
}

const JceVolumeStackEntry *jce_volume_stack_at(uint32_t idx)
{
    if (idx >= JCE_VOLUME_STACK_MAX) return NULL;
    return s_entries[idx].active ? &s_entries[idx] : NULL;
}

static float volume_weight(const JceVolumeStackEntry *e, const float p[3])
{
    if (e->shape == JCE_VOLUME_SHAPE_GLOBAL) return 1.0f;
    if (e->shape == JCE_VOLUME_SHAPE_SPHERE) {
        float dx = p[0] - e->position[0];
        float dy = p[1] - e->position[1];
        float dz = p[2] - e->position[2];
        float d  = sqrtf(dx*dx + dy*dy + dz*dz);
        float inner = e->radius - e->blend_distance;
        if (inner < 0) inner = 0;
        if (d <= inner) return 1.0f;
        if (d >= e->radius) return 0.0f;
        float blend = e->radius - inner;
        if (blend <= 0.0001f) return 1.0f;
        return 1.0f - (d - inner) / blend;
    }
    /* Box */
    float dx[3];
    for (int i = 0; i < 3; ++i)
        dx[i] = fabsf(p[i] - e->position[i]) - e->half_extents[i];
    /* Distance outside box (max-axis, clamped to >= 0). */
    float worst = dx[0];
    if (dx[1] > worst) worst = dx[1];
    if (dx[2] > worst) worst = dx[2];
    if (worst <= -e->blend_distance) return 1.0f;
    if (worst >= 0.0f) return 0.0f;
    if (e->blend_distance <= 0.0001f) return 1.0f;
    return -worst / e->blend_distance;
}

static int cmp_priority(const void *a, const void *b)
{
    const JceVolumeStackEntry *ea = *(const JceVolumeStackEntry *const *)a;
    const JceVolumeStackEntry *eb = *(const JceVolumeStackEntry *const *)b;
    if (ea->priority < eb->priority) return -1;
    if (ea->priority > eb->priority) return  1;
    return 0;
}

uint32_t jce_volume_resolve_at(const float p[3],
                                 const JcePostFxStackComponent *def,
                                 JcePostFxStackComponent       *out)
{
    if (!out) return 0;
    if (def) *out = *def;
    else     *out = jce_postfx_stack_default();
    if (!p) return 0;

    /* Snapshot active list pointers. */
    const JceVolumeStackEntry *list[JCE_VOLUME_STACK_MAX];
    uint32_t n = 0;
    for (int i = 0; i < JCE_VOLUME_STACK_MAX; ++i)
        if (s_entries[i].active) list[n++] = &s_entries[i];
    if (n == 0) return 0;
    qsort(list, n, sizeof(list[0]), cmp_priority);

    uint32_t contributors = 0;
    for (uint32_t i = 0; i < n; ++i) {
        float w = volume_weight(list[i], p);
        if (w <= 0.0001f) continue;
        if (w > 1.0f) w = 1.0f;
        *out = jce_postfx_stack_blend(*out, list[i]->stack, w);
        contributors++;
    }
    return contributors;
}
