/*
 * jce_terrain_collision_stream.h -- per-tile terrain collision, paged.
 *
 * WHY THIS EXISTS
 * ---------------
 * Terrain collision was spawned once, at level load, from a monolithic height
 * grid.  A tiled or procedural terrain has no such grid -- its heights live in
 * tiles that page in and out -- so it got **no collision at all**.  Not a
 * degraded collider: none.  A multi-kilometre world was walkable only where a
 * designer had also authored a monolithic terrain over it.
 *
 * This pages colliders the same way the renderer pages meshes: tiles within a
 * radius of a focus point get a btHeightfieldTerrainShape body; tiles that fall
 * out of range have theirs destroyed.  A player only ever collides with ground
 * near them, so residency by distance is not an approximation here -- it is the
 * exact requirement.
 *
 * WHY THE HEIGHTS ARE COPIED
 * --------------------------
 * The physics bridge copies its samples (convention 4 in jce_physics.h), so a
 * tile can be unpinned the instant its body exists.  Holding the pin instead
 * would couple physics residency to the store's byte budget and let a distant
 * body keep a tile resident forever.
 *
 * Layer: Application (L5) -- it is the only layer that may see both physics and
 * the resource store.
 */

#ifndef JCE_TERRAIN_COLLISION_STREAM_H
#define JCE_TERRAIN_COLLISION_STREAM_H

#include <jce/middleware/physics/jce_physics.h>

#include "resource/jce_terrain_store.h"

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

typedef struct JceTerrainCollisionStream JceTerrainCollisionStream;

/* Fill one tile's heights, in the same local space the heightfield body will
 * use.  `span` is the sample count per side; write span*span floats.
 *
 * This exists because the engine has TWO tiled terrains: the cooked v3 store,
 * and JceTerrain's own tile cache (which is what a .terrain.json with a
 * procedural or tiled source produces).  The second is what shipped content
 * actually uses, and it was the one with no collision at all -- so the stream
 * takes a sampler rather than binding to the store, and both can feed it. */
typedef bool (*JceTerrainCollisionSampleFn)(void *ctx,
                                            uint32_t tile_x, uint32_t tile_z,
                                            uint32_t span,
                                            float origin_x, float origin_z,
                                            float tile_world_size,
                                            float *out_heights);

typedef struct JceTerrainCollisionStreamDesc {
    JcePhysicsWorld  *world;

    /* EITHER a cooked store... */
    JceTerrainStore  *store;
    JceTerrainHandle  handle;
    const char       *virtual_path;   /* passed through to the loader */

    /* ...OR a sampler.  When `sample_fn` is set it wins, and `store` may be
     * NULL.  `sample_span` is the samples-per-side each tile is built at;
     * 0 selects 33, which is a 32-cell tile -- fine ground resolution for
     * collision without making each body enormous. */
    JceTerrainCollisionSampleFn sample_fn;
    void                       *sample_ctx;
    uint32_t                    sample_span;

    /* Tile grid extent, so the stream knows which keys exist. */
    uint32_t tiles_x;
    uint32_t tiles_z;

    /* World placement.  `origin` is the MIN corner of tile (0,0); each tile
     * spans tile_world_size on X and Z. */
    jce_vec3 origin;
    float    tile_world_size;

    /* Vertical bounds of the whole terrain, in the same local space as the
     * tile heights.  Passed straight to the heightfield body, which needs a
     * conservative interval rather than a per-tile one. */
    float    min_height;
    float    max_height;

    /* Tiles whose centre is within this distance of the focus point get a
     * body.  0 selects a default of 1.5 tiles. */
    float    radius;

    /* Hysteresis band, as a fraction of `radius`, applied when REMOVING.  A
     * body is destroyed only past radius*(1+hysteresis), so a focus point
     * hovering exactly on the boundary does not rebuild the same collider
     * every frame.  0 selects 0.25. */
    float    hysteresis;

    float    friction;
    float    restitution;
    JceHeightfieldDiagonal diagonal;
    bool     smooth_internal_edges;

    /* Hard cap on simultaneously live bodies.  0 selects 64. */
    uint32_t max_bodies;
} JceTerrainCollisionStreamDesc;

JceTerrainCollisionStream *jce_terrain_collision_stream_create(
    const JceTerrainCollisionStreamDesc *desc);

/* Destroys every live body first, so the stream can be torn down before or
 * after the physics world without ordering surprises. */
void jce_terrain_collision_stream_destroy(JceTerrainCollisionStream *s);

/* Bring residency in line with `focus`.  Creates bodies for tiles that entered
 * range and destroys those that left.  Idempotent: calling twice with the same
 * focus does no work and creates no bodies, which is what lets it be called
 * every frame. */
void jce_terrain_collision_stream_update(JceTerrainCollisionStream *s,
                                         jce_vec3 focus);

/* Live body count, and whether a specific tile currently has one. */
uint32_t jce_terrain_collision_stream_active(const JceTerrainCollisionStream *s);
bool jce_terrain_collision_stream_has_tile(const JceTerrainCollisionStream *s,
                                           JceTerrainTileKey key);

/* Total bodies created since creation.  Exists so a test can prove that a
 * steady focus point does NOT churn colliders -- a stream that silently
 * rebuilt every tile each frame would look identical by every other measure. */
uint64_t jce_terrain_collision_stream_created(
    const JceTerrainCollisionStream *s);

#ifdef __cplusplus
}
#endif

#endif /* JCE_TERRAIN_COLLISION_STREAM_H */
