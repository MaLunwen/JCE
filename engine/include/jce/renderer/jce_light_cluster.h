/*
 * jce_light_cluster.h  Clustered light culling — CPU reference.
 *
 * Builds a froxel grid (uniform XY × log-Z slices) over the camera
 * frustum and assigns lights to the froxels they touch.  This is the
 * data-structure foundation of clustered forward / clustered deferred
 * shading; a future GPU compute backend will produce the same per-
 * froxel light lists in parallel without changing the public API.
 *
 * Why CPU first:
 *   - Lets gameplay/editor visualize culling results on hosts without a
 *     compute-capable backend.
 *   - Provides a known-good reference to validate the GPU compute pass
 *     against, slot-for-slot.
 *   - Performance is acceptable up to a few hundred lights.
 *
 * Recommended grid sizes:
 *   - Mobile / GLES 3.0: 16 × 9 × 16  (≈ 2300 froxels, fits SSBO-less
 *     UBO budgets).
 *   - Desktop:           24 × 16 × 24 (≈ 9200 froxels).
 *
 * Layer: Renderer (Layer 3).
 */

#ifndef JCE_LIGHT_CLUSTER_H
#define JCE_LIGHT_CLUSTER_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceLightCluster JceLightCluster;

/* Grid descriptor.  All slice counts must be > 0. */
typedef struct {
    uint32_t cells_x;       /* tiles across the screen */
    uint32_t cells_y;       /* tiles down the screen */
    uint32_t slices_z;      /* logarithmic depth slices */
    uint32_t max_lights;    /* upper bound on simultaneously-active lights */
    uint32_t max_per_cell;  /* cap on lights any single froxel can store */
} JceLightClusterDesc;

/* Per-light input — generic point/spot bounding-sphere description.
 * Spot lights collapse to a sphere for the broad-phase; the shading
 * pass evaluates the cone afterwards. */
typedef struct {
    jce_vec3 position_ws;   /* world-space center */
    float    radius;        /* effective influence radius */
    uint32_t user_id;       /* opaque caller id, returned in the cell list */
} JceLightProxy;

/* Read-only result of a build — pointer to per-cell counts and indices.
 * Layout:
 *   counts[cells_x * cells_y * slices_z]
 *   indices[cells_x * cells_y * slices_z * max_per_cell]
 * For cell C, indices[C * max_per_cell .. + counts[C]] are the lights
 * touching that cell, encoded by their build-call array index. */
typedef struct {
    const uint32_t      *cell_counts;
    const uint32_t      *cell_indices;
    uint32_t             cells_x, cells_y, slices_z;
    uint32_t             max_per_cell;
    uint32_t             total_lights_in;     /* lights submitted */
    uint32_t             total_assignments;   /* sum of cell_counts */
    uint32_t             overflow_cells;      /* cells that hit max_per_cell */
} JceLightClusterResult;

/* Lifecycle.  Returns NULL on OOM or invalid desc. */
JCE_API JceLightCluster *jce_light_cluster_create(const JceLightClusterDesc *desc);
JCE_API void             jce_light_cluster_destroy(JceLightCluster *lc);

/* Configure the camera & frustum for the next build.
 *   inv_view: world-from-view (used to translate light AABB to view space)
 *   proj:     view-to-clip projection
 *   near, far: positive scalar near/far planes (must satisfy 0 < n < f). */
JCE_API void jce_light_cluster_set_camera(JceLightCluster *lc,
                                          const jce_mat4  *inv_view,
                                          const jce_mat4  *proj,
                                          float            near_plane,
                                          float            far_plane);

/* Build the per-cell light lists for the given proxy array.
 * Resets internal counts each call.  Returns false if the desc was
 * invalid or `lights` was NULL with `count > 0`. */
JCE_API bool jce_light_cluster_build(JceLightCluster      *lc,
                                     const JceLightProxy  *lights,
                                     uint32_t              count);

/* Read the most recent build result.  Pointers are valid until the
 * next jce_light_cluster_build() / _destroy() call. */
JCE_API JceLightClusterResult jce_light_cluster_get_result(const JceLightCluster *lc);

/* Helper: project a view-space Z value into the [0, slices_z) slice
 * index used internally.  Logarithmic distribution between near/far. */
JCE_API uint32_t jce_light_cluster_slice_for_view_z(const JceLightCluster *lc,
                                                     float view_z);

JCE_EXTERN_C_END

#endif /* JCE_LIGHT_CLUSTER_H */
