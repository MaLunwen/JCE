/*
 * jce_spawn_manager.h -- distance-driven spawn density manager.
 *
 * Maintains a population of "spawn slots" of two flavours:
 *
 *   - Pedestrians: spawned anywhere on the navmesh, density falls off
 *     with distance from the viewer.  Caller provides a callback to
 *     pick a navmesh-valid point.
 *
 *   - Vehicles: spawned along the road network (jce_road_network),
 *     oriented along the local road direction.
 *
 * The manager doesn't own entities — it issues spawn / despawn callbacks
 * that the caller binds to gameplay code.  Slots store opaque cookies
 * (uint64) that the callback can use to identify entities.
 *
 * Policy:
 *   - Soft cap per category.
 *   - Spawn ring (min / max radius from viewer):
 *       - Inside min radius: too close, skip.
 *       - Inside [min,max]: candidates.
 *       - Outside max + cull_pad: despawn.
 *
 * Layer: middleware/world (Layer 4) — public.
 */
#ifndef JCE_SPAWN_MANAGER_H
#define JCE_SPAWN_MANAGER_H

#include <jce/middleware/world/jce_road_network.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef enum {
    JCE_SPAWN_KIND_PED     = 0,
    JCE_SPAWN_KIND_VEHICLE = 1
} JceSpawnKind;

typedef struct {
    JceSpawnKind kind;
    jce_vec3     position;
    jce_vec3     forward;          /* unit direction */
    uint32_t     archetype;        /* user-defined: ped/vehicle type id */
} JceSpawnRequest;

/* Caller-supplied callbacks — return a non-zero cookie to claim the
 * spawn slot, or 0 to abort.  The despawn callback is invoked when the
 * manager evicts the slot. */
typedef uint64_t (*JceSpawnCreateFn)  (const JceSpawnRequest *req, void *user);
typedef void     (*JceSpawnDestroyFn) (uint64_t cookie, void *user);

/* Optional: caller picks a random ped position (e.g. via navmesh
 * sampler).  If unset, peds will be skipped. */
typedef bool     (*JceSpawnPedSampleFn)(jce_vec3 viewer_pos, float min_r,
                                          float max_r, jce_vec3 *out_pos,
                                          void *user);

typedef struct {
    /* Populations. */
    uint32_t                max_peds;
    uint32_t                max_vehicles;

    /* Geometry. */
    float                   min_spawn_radius;
    float                   max_spawn_radius;
    float                   despawn_pad;       /* despawn at max_spawn_radius + pad */

    /* Pacing. */
    float                   spawn_interval;    /* seconds between spawn attempts */

    /* References. */
    const JceRoadNetwork   *road_network;      /* required for vehicles */

    /* Callbacks. */
    JceSpawnCreateFn        on_create;
    JceSpawnDestroyFn       on_destroy;
    JceSpawnPedSampleFn     ped_sampler;
    void                   *user;

    /* Archetype distributions: caller can pre-fill arrays of valid
     * archetype ids.  If null, archetype 0 is used. */
    const uint32_t         *ped_archetypes;
    uint32_t                ped_archetype_count;
    const uint32_t         *vehicle_archetypes;
    uint32_t                vehicle_archetype_count;

    /* RNG seed (0 → pick a default). */
    uint64_t                rng_seed;
} JceSpawnManagerDesc;

typedef struct JceSpawnManager JceSpawnManager;

typedef struct {
    uint32_t live_peds;
    uint32_t live_vehicles;
    uint32_t spawn_attempts;
    uint32_t spawn_succeeded;
    uint32_t despawned;
} JceSpawnStats;

JCE_API JceSpawnManager *jce_spawn_manager_create(const JceSpawnManagerDesc *desc);
JCE_API void             jce_spawn_manager_destroy(JceSpawnManager *m);

JCE_API void             jce_spawn_manager_set_viewer(JceSpawnManager *m, jce_vec3 pos);
JCE_API void             jce_spawn_manager_update(JceSpawnManager *m, float dt);
JCE_API void             jce_spawn_manager_clear(JceSpawnManager *m);

JCE_API JceSpawnStats    jce_spawn_manager_get_stats(const JceSpawnManager *m);

#ifdef __cplusplus
}
#endif

#endif /* JCE_SPAWN_MANAGER_H */
