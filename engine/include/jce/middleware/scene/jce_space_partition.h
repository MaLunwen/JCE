/*
 * jce_space_partition.h  Spatial partitioning for scene queries.
 *
 * Provides broadphase spatial indexing (BVH / octree / grid) for
 * fast frustum culling, ray queries, and proximity searches.
 *
 * Layer: Scene (Layer 5).
 *
 * STATUS: Architecture stub — API surface defined, implementation pending.
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

JceSpaceIndex *jce_space_create(const JceSpaceConfig *config);
void           jce_space_destroy(JceSpaceIndex *idx);

/* ================================================================== */
/* Object management                                                   */
/* ================================================================== */

/* Insert an object with the given AABB.  Returns an opaque handle.
 * user_id is stored and returned in query results. */
uint32_t jce_space_insert(JceSpaceIndex *idx, JceAABB bounds,
                           uint32_t user_id);

/* Update the AABB of an existing object. */
void jce_space_update(JceSpaceIndex *idx, uint32_t handle,
                       JceAABB new_bounds);

/* Remove an object. */
void jce_space_remove(JceSpaceIndex *idx, uint32_t handle);

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Frustum culling: fill out_ids with user_ids of objects inside the
 * frustum (6 planes).  Returns the number of results (capped at max). */
uint32_t jce_space_query_frustum(const JceSpaceIndex *idx,
                                  const jce_vec4 planes[6],
                                  uint32_t *out_ids, uint32_t max);

/* AABB overlap query. */
uint32_t jce_space_query_aabb(const JceSpaceIndex *idx, JceAABB region,
                               uint32_t *out_ids, uint32_t max);

/* Sphere query. */
uint32_t jce_space_query_sphere(const JceSpaceIndex *idx,
                                 jce_vec3 center, float radius,
                                 uint32_t *out_ids, uint32_t max);

/* Ray query: returns user_id of closest hit, or UINT32_MAX on miss. */
typedef struct {
    uint32_t user_id;
    float    distance;
    jce_vec3 point;
} JceSpaceRayHit;

bool jce_space_raycast(const JceSpaceIndex *idx, jce_vec3 origin,
                        jce_vec3 direction, float max_dist,
                        JceSpaceRayHit *out_hit);

/* ================================================================== */
/* Statistics                                                          */
/* ================================================================== */

uint32_t jce_space_object_count(const JceSpaceIndex *idx);

JCE_EXTERN_C_END

#endif /* JCE_SPACE_PARTITION_H */
