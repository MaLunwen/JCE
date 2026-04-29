/*
 * jce_spawn_manager.c -- distance-driven population manager.
 *
 * Each tick:
 *   1. Despawn slots farther than max_radius + pad.
 *   2. If under cap and spawn timer elapsed, attempt one new spawn.
 *
 * Vehicles use the road network: pick a random segment near the viewer,
 * sample a random t in [0,1], orient along forward direction, randomize
 * lane direction (50/50 between AB / BA when both exist).
 *
 * Peds use the caller-provided sampler (navmesh callback).
 */

#include <jce/middleware/world/jce_spawn_manager.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "spawn_mgr"

typedef struct {
    bool         alive;
    JceSpawnKind kind;
    uint64_t     cookie;
    jce_vec3     position;
} SpawnSlot;

struct JceSpawnManager {
    JceSpawnManagerDesc desc;

    SpawnSlot          *peds;
    SpawnSlot          *vehicles;

    jce_vec3            viewer;
    bool                viewer_valid;

    float               accum;
    uint64_t            rng;

    JceSpawnStats       stats;
};

static uint32_t rng_next(uint64_t *s)
{
    uint64_t x = *s ? *s : 0xA0761D6478BD642FULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return (uint32_t)(x & 0xFFFFFFFFu);
}

static float v3_dist(jce_vec3 a, jce_vec3 b)
{
    float dx=a.x-b.x, dy=a.y-b.y, dz=a.z-b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

JceSpawnManager *jce_spawn_manager_create(const JceSpawnManagerDesc *desc)
{
    if (!desc) return NULL;
    if (desc->min_spawn_radius >= desc->max_spawn_radius) return NULL;

    JceSpawnManager *m = (JceSpawnManager *)JCE_CALLOC(1, sizeof(*m));
    if (!m) return NULL;
    m->desc = *desc;
    m->rng = desc->rng_seed ? desc->rng_seed : 0xC0FFEEULL;

    if (desc->max_peds) {
        m->peds = (SpawnSlot *)JCE_CALLOC(desc->max_peds, sizeof(SpawnSlot));
        if (!m->peds) { JCE_FREE(m); return NULL; }
    }
    if (desc->max_vehicles) {
        m->vehicles = (SpawnSlot *)JCE_CALLOC(desc->max_vehicles, sizeof(SpawnSlot));
        if (!m->vehicles) { JCE_FREE(m->peds); JCE_FREE(m); return NULL; }
    }
    return m;
}

void jce_spawn_manager_destroy(JceSpawnManager *m)
{
    if (!m) return;
    jce_spawn_manager_clear(m);
    JCE_FREE(m->peds);
    JCE_FREE(m->vehicles);
    JCE_FREE(m);
}

void jce_spawn_manager_clear(JceSpawnManager *m)
{
    if (!m) return;
    if (m->peds) {
        for (uint32_t i = 0; i < m->desc.max_peds; i++) {
            if (m->peds[i].alive && m->desc.on_destroy)
                m->desc.on_destroy(m->peds[i].cookie, m->desc.user);
            m->peds[i].alive = false;
        }
    }
    if (m->vehicles) {
        for (uint32_t i = 0; i < m->desc.max_vehicles; i++) {
            if (m->vehicles[i].alive && m->desc.on_destroy)
                m->desc.on_destroy(m->vehicles[i].cookie, m->desc.user);
            m->vehicles[i].alive = false;
        }
    }
    m->stats.live_peds = m->stats.live_vehicles = 0;
}

void jce_spawn_manager_set_viewer(JceSpawnManager *m, jce_vec3 pos)
{
    if (!m) return;
    m->viewer = pos;
    m->viewer_valid = true;
}

static int find_dead_slot(SpawnSlot *slots, uint32_t cap)
{
    for (uint32_t i = 0; i < cap; i++)
        if (!slots[i].alive) return (int)i;
    return -1;
}

static uint32_t pick_archetype(const uint32_t *list, uint32_t n, uint64_t *rng)
{
    if (!list || n == 0) return 0;
    return list[rng_next(rng) % n];
}

static void try_spawn_vehicle(JceSpawnManager *m)
{
    if (!m->desc.road_network || m->stats.live_vehicles >= m->desc.max_vehicles)
        return;
    int slot = find_dead_slot(m->vehicles, m->desc.max_vehicles);
    if (slot < 0) return;

    /* Pick a random near-by segment. */
    uint32_t seg = jce_road_network_pick_random_near(
        m->desc.road_network, m->viewer, m->desc.max_spawn_radius, &m->rng);
    if (seg == JCE_ROAD_INVALID_ID) return;

    float t = (float)(rng_next(&m->rng) & 0xFFFF) / 65535.0f;
    jce_vec3 pos, fwd;
    if (!jce_road_network_sample_segment(m->desc.road_network, seg, t, &pos, &fwd))
        return;

    /* Inside min radius? skip. */
    float d = v3_dist(pos, m->viewer);
    if (d < m->desc.min_spawn_radius || d > m->desc.max_spawn_radius) return;

    /* 50/50 reverse direction if BA lanes exist. */
    const JceRoadSegment *s = jce_road_network_get_segment(m->desc.road_network, seg);
    if (s && s->lanes_ba > 0 && s->lanes_ab > 0 && (rng_next(&m->rng) & 1)) {
        fwd.x = -fwd.x; fwd.y = -fwd.y; fwd.z = -fwd.z;
    } else if (s && s->lanes_ba > 0 && s->lanes_ab == 0) {
        fwd.x = -fwd.x; fwd.y = -fwd.y; fwd.z = -fwd.z;
    }

    JceSpawnRequest req;
    req.kind      = JCE_SPAWN_KIND_VEHICLE;
    req.position  = pos;
    req.forward   = fwd;
    req.archetype = pick_archetype(m->desc.vehicle_archetypes,
                                   m->desc.vehicle_archetype_count, &m->rng);

    m->stats.spawn_attempts++;
    if (!m->desc.on_create) return;
    uint64_t cookie = m->desc.on_create(&req, m->desc.user);
    if (!cookie) return;

    m->vehicles[slot].alive    = true;
    m->vehicles[slot].kind     = JCE_SPAWN_KIND_VEHICLE;
    m->vehicles[slot].cookie   = cookie;
    m->vehicles[slot].position = pos;
    m->stats.live_vehicles++;
    m->stats.spawn_succeeded++;
}

static void try_spawn_ped(JceSpawnManager *m)
{
    if (!m->desc.ped_sampler || m->stats.live_peds >= m->desc.max_peds) return;
    int slot = find_dead_slot(m->peds, m->desc.max_peds);
    if (slot < 0) return;

    jce_vec3 pos;
    if (!m->desc.ped_sampler(m->viewer, m->desc.min_spawn_radius,
                              m->desc.max_spawn_radius, &pos, m->desc.user))
        return;

    /* Random heading. */
    float ang = (float)(rng_next(&m->rng) & 0xFFFF) * (6.28318530718f / 65535.0f);
    jce_vec3 fwd = jce_v3(cosf(ang), 0.0f, sinf(ang));

    JceSpawnRequest req;
    req.kind      = JCE_SPAWN_KIND_PED;
    req.position  = pos;
    req.forward   = fwd;
    req.archetype = pick_archetype(m->desc.ped_archetypes,
                                   m->desc.ped_archetype_count, &m->rng);

    m->stats.spawn_attempts++;
    if (!m->desc.on_create) return;
    uint64_t cookie = m->desc.on_create(&req, m->desc.user);
    if (!cookie) return;

    m->peds[slot].alive    = true;
    m->peds[slot].kind     = JCE_SPAWN_KIND_PED;
    m->peds[slot].cookie   = cookie;
    m->peds[slot].position = pos;
    m->stats.live_peds++;
    m->stats.spawn_succeeded++;
}

static void cull_far(JceSpawnManager *m, SpawnSlot *slots, uint32_t cap, uint32_t *live_count)
{
    float cull = m->desc.max_spawn_radius + m->desc.despawn_pad;
    float cull2 = cull * cull;
    for (uint32_t i = 0; i < cap; i++) {
        if (!slots[i].alive) continue;
        float dx = slots[i].position.x - m->viewer.x;
        float dy = slots[i].position.y - m->viewer.y;
        float dz = slots[i].position.z - m->viewer.z;
        if (dx*dx + dy*dy + dz*dz > cull2) {
            if (m->desc.on_destroy) m->desc.on_destroy(slots[i].cookie, m->desc.user);
            slots[i].alive = false;
            (*live_count)--;
            m->stats.despawned++;
        }
    }
}

void jce_spawn_manager_update(JceSpawnManager *m, float dt)
{
    if (!m || !m->viewer_valid) return;

    if (m->peds)     cull_far(m, m->peds,     m->desc.max_peds,     &m->stats.live_peds);
    if (m->vehicles) cull_far(m, m->vehicles, m->desc.max_vehicles, &m->stats.live_vehicles);

    m->accum += dt;
    float interval = m->desc.spawn_interval > 0.0f ? m->desc.spawn_interval : 0.25f;
    while (m->accum >= interval) {
        m->accum -= interval;
        /* Alternate vehicle and ped attempts. */
        if (rng_next(&m->rng) & 1) {
            try_spawn_vehicle(m);
        } else {
            try_spawn_ped(m);
        }
    }
}

JceSpawnStats jce_spawn_manager_get_stats(const JceSpawnManager *m)
{
    JceSpawnStats z;
    if (!m) { memset(&z, 0, sizeof(z)); return z; }
    return m->stats;
}
