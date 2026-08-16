/*
 * jce_space_partition.h  Spatial partitioning for scene queries.
 *
 * Provides broadphase spatial indexing (BVH / octree / grid) for
 * fast frustum culling, ray queries, and proximity searches.
 *
 * Layer: Scene (Layer 5).
 *
 * STATUS: Implemented — uniform grid backend (used for all index types
 *         until BVH / octree variants land).
 */

#ifndef JCE_SPACE_PARTITION_H
#define JCE_SPACE_PARTITION_H


#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceSpaceIndex JceSpaceIndex;

/* ================================================================== */
/* AABB                                                                */
/* ================================================================== */

typedef struct {
    jce_vec3 min;
    jce_vec3 max;
} JceAABB;

/* ================================================================== */
/* Configuration                                                       */
/* ================================================================== */

typedef enum {
    JCE_SPACE_BVH,      /* bounding volume hierarchy (dynamic) */
    JCE_SPACE_OCTREE,   /* octree (static / semi-static) */
    JCE_SPACE_GRID,     /* uniform grid (2D / flat worlds) */
} JceSpaceType;

typedef struct {
    JceSpaceType type;
    JceAABB      world_bounds;   /* only used for octree/grid */
    uint32_t     max_objects;
} JceSpaceConfig;

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JCE_API JceSpaceIndex *jce_space_create(const JceSpaceConfig *config);
JCE_API void           jce_space_destroy(JceSpaceIndex *idx);

/* Clear all objects and (optionally) re-set world bounds without freeing the
 * index allocation.  The per-cell list capacity is preserved across resets
 * (only occupied cells are zeroed, so the clear is proportional to live
 * occupancy, not res³).  When new_bounds changes the derived resolution the
 * cell grid is reallocated.  Prefer the persistent insert/update/remove path
 * over a per-frame reset+reinsert-all; reset stays for full rebuilds and the
 * shadow-caster grid.  Pass new_bounds = NULL to keep current bounds. */
JCE_API void           jce_space_reset(JceSpaceIndex *idx,
                                        const JceAABB *new_bounds);

/* ================================================================== */
/* Object management                                                   */
/* ================================================================== */

/* Insert an object with the given AABB.  Returns an opaque handle
 * (0 on failure). user_id is stored and returned in query results. */
JCE_API uint32_t jce_space_insert(JceSpaceIndex *idx, JceAABB bounds,
                                   uint32_t user_id);

/* Update the AABB of an existing object.  Re-buckets ONLY when the new AABB
 * spans a different cell range — a static object that never moves costs ~0, so
 * a persistent broad-phase can call this every frame for every object and pay
 * only for the ones that actually moved. */
JCE_API void jce_space_update(JceSpaceIndex *idx, uint32_t handle,
                               JceAABB new_bounds);

/* Refresh the user_id returned by queries WITHOUT touching the cell lists.
 * Lets a persistent index (object inserted once, kept across frames) re-map a
 * stable handle to a per-frame payload (e.g. this frame's render-list index)
 * for free. */
JCE_API void jce_space_set_user_id(JceSpaceIndex *idx, uint32_t handle,
                                    uint32_t user_id);

/* Remove an object. */
JCE_API void jce_space_remove(JceSpaceIndex *idx, uint32_t handle);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Frustum culling: fill out_ids with user_ids of objects inside the
 * frustum (6 planes).  Returns the number of results (capped at max). */
JCE_API uint32_t jce_space_query_frustum(const JceSpaceIndex *idx,
                                  const jce_vec4 planes[6],
                                  uint32_t *out_ids, uint32_t max);

/* AABB overlap query. */
JCE_API uint32_t jce_space_query_aabb(const JceSpaceIndex *idx, JceAABB region,
                               uint32_t *out_ids, uint32_t max);

/* Sphere query. */
JCE_API uint32_t jce_space_query_sphere(const JceSpaceIndex *idx,
                                 jce_vec3 center, float radius,
                                 uint32_t *out_ids, uint32_t max);

/* Ray query: returns user_id of closest hit, or UINT32_MAX on miss. */
typedef struct {
    uint32_t user_id;
    float    distance;
    jce_vec3 point;
} JceSpaceRayHit;

JCE_API bool jce_space_raycast(const JceSpaceIndex *idx, jce_vec3 origin,
                                jce_vec3 direction, float max_dist,
                                JceSpaceRayHit *out_hit);

/* ================================================================== */
/* Statistics                                                          */
/* ================================================================== */

JCE_API uint32_t jce_space_object_count(const JceSpaceIndex *idx);

/* Total cell count (res[0]*res[1]*res[2]). */
JCE_API uint32_t jce_space_cell_count(const JceSpaceIndex *idx);

/* Number of currently non-empty cells (what a full scan now iterates). */
JCE_API uint32_t jce_space_occupied_cell_count(const JceSpaceIndex *idx);

/* Per-axis grid resolution (cells per axis), derived from the world extent. */
JCE_API void jce_space_resolution(const JceSpaceIndex *idx, uint32_t out_res[3]);

/* Work counters from the most recent jce_space_query_frustum call: how many
 * occupied cells it walked, how many survived the coarse reject, how many
 * object slots those cells listed, and how many distinct objects were actually
 * tested after epoch dedup.  Out-params rather than a struct on purpose -- a
 * caller-allocated stats struct breaks the moment the two sides disagree about
 * its size.  Any pointer may be NULL. */
JCE_API void jce_space_last_query_stats(const JceSpaceIndex *idx,
                                         uint32_t *out_cells_walked,
                                         uint32_t *out_cells_accepted,
                                         uint32_t *out_objs_visited,
                                         uint32_t *out_objs_tested);

JCE_EXTERN_C_END

#endif /* JCE_SPACE_PARTITION_H */
