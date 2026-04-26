/*
 * jce_space_partition.c  Spatial partitioning — stub implementation.
 *
 * STATUS: Architecture stub.  All functions return safe defaults
 *         (NULL / 0 / false / UINT32_MAX) until spatial indexing is built.
 */

#include <jce/middleware/scene/jce_space_partition.h>
#include <stddef.h>

JceSpaceIndex *jce_space_create(const JceSpaceConfig *config)              { (void)config; return NULL; }
void           jce_space_destroy(JceSpaceIndex *idx)                       { (void)idx; }

uint32_t jce_space_insert(JceSpaceIndex *idx, JceAABB bounds,
                           uint32_t user_id)                               { (void)idx; (void)bounds; (void)user_id; return 0; }
void     jce_space_update(JceSpaceIndex *idx, uint32_t handle,
                           JceAABB new_bounds)                             { (void)idx; (void)handle; (void)new_bounds; }
void     jce_space_remove(JceSpaceIndex *idx, uint32_t handle)             { (void)idx; (void)handle; }

uint32_t jce_space_query_frustum(const JceSpaceIndex *idx,
                                  const jce_vec4 planes[6],
                                  uint32_t *out_ids, uint32_t max)         { (void)idx; (void)planes; (void)out_ids; (void)max; return 0; }
uint32_t jce_space_query_aabb(const JceSpaceIndex *idx, JceAABB region,
                               uint32_t *out_ids, uint32_t max)            { (void)idx; (void)region; (void)out_ids; (void)max; return 0; }
uint32_t jce_space_query_sphere(const JceSpaceIndex *idx,
                                 jce_vec3 center, float radius,
                                 uint32_t *out_ids, uint32_t max)          { (void)idx; (void)center; (void)radius; (void)out_ids; (void)max; return 0; }

bool jce_space_raycast(const JceSpaceIndex *idx, jce_vec3 origin,
                        jce_vec3 direction, float max_dist,
                        JceSpaceRayHit *out_hit)                           { (void)idx; (void)origin; (void)direction; (void)max_dist; (void)out_hit; return false; }

uint32_t jce_space_object_count(const JceSpaceIndex *idx)                  { (void)idx; return 0; }
