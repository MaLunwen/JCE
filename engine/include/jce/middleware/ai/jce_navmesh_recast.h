/*
 * jce_navmesh_recast.h -- Recast/Detour navmesh backend.
 *
 * Builds a Detour navmesh from an arbitrary triangle soup using
 * Recast's voxelisation pipeline (heightfield → compact heightfield
 * → contours → polymesh → detail mesh), then exposes a thin C query
 * surface compatible with JceNavAgent's `JceNavAgentPathFn`.
 *
 * This complements jce_navmesh.h (grid + A*) — pick whichever
 * backend fits the source data:
 *   - jce_navmesh.h          : authored .navmesh.json grid (cheap, editor-friendly)
 *   - jce_navmesh_recast.h   : baked from collision geometry  (production)
 *
 * Layer: AI / Navigation (Layer 3) — public.
 */

#ifndef JCE_NAVMESH_RECAST_H
#define JCE_NAVMESH_RECAST_H

#include <jce/middleware/ai/jce_nav_agent.h>
#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceRecastNavMesh JceRecastNavMesh;

/* ── Build configuration ─────────────────────────────────────────── *
 *
 * Sensible defaults are filled by jce_recast_default_config().
 * Units are world-space metres unless noted.
 */
typedef struct {
    float cell_size;             /* xz voxel size (default 0.30) */
    float cell_height;           /* y voxel size (default 0.20) */

    float walkable_slope_deg;    /* max slope an agent can climb (default 45) */
    float walkable_height;       /* min ceiling above the floor (default 2.0) */
    float walkable_climb;        /* max step height (default 0.40) */
    float walkable_radius;       /* agent radius (default 0.40) */

    float edge_max_len;          /* max contour edge length     (default 12.0) */
    float edge_max_error;        /* simplification tolerance    (default 1.3) */

    int   region_min_size;       /* drop islands smaller than this (cells, default 8) */
    int   region_merge_size;     /* merge regions smaller than this (cells, default 20) */

    int   max_verts_per_poly;    /* 3..6 (default 6) */

    float detail_sample_dist;    /* multiple of cell_size (default 6.0) */
    float detail_sample_max_err; /* multiple of cell_height (default 1.0) */
} JceRecastConfig;

JCE_API void jce_recast_default_config(JceRecastConfig *out_cfg);

/* ── Build / destroy ─────────────────────────────────────────────── *
 *
 * Build a navmesh from triangle soup.  Vertices are tightly-packed
 * x,y,z floats (length = vertex_count * 3).  Indices are uint32 with
 * length = triangle_count * 3.  Returns NULL on build failure (and
 * logs the reason).
 */
JCE_API JceRecastNavMesh *jce_recast_build(const float    *vertices,
                                            uint32_t        vertex_count,
                                            const uint32_t *indices,
                                            uint32_t        triangle_count,
                                            const JceRecastConfig *cfg);

JCE_API void jce_recast_destroy(JceRecastNavMesh *nm);

/* ── Query API ───────────────────────────────────────────────────── *
 *
 * `out_xz` receives waypoint pairs (x,z) — y is dropped to match
 * JceNavAgent's XZ convention.  Returns 0 if no path exists.
 */
JCE_API int  jce_recast_find_path(const JceRecastNavMesh *nm,
                                   float start_x, float start_z,
                                   float goal_x,  float goal_z,
                                   float *out_xz, int max_pairs);

/* Adapter matching JceNavAgentPathFn — pass the navmesh as `user`:
 *
 *   jce_nav_agent_set_path_fn(set, jce_recast_path_fn, recast_nm);
 */
JCE_API int  jce_recast_path_fn(void *user,
                                 float start_x, float start_z,
                                 float goal_x,  float goal_z,
                                 float *out_xz, int max_pairs);

/* Snap an arbitrary world point to the nearest walkable polygon.
 * Returns true if a polygon was found within ~2 m of the input. */
JCE_API bool jce_recast_snap_to_navmesh(const JceRecastNavMesh *nm,
                                         float wx, float wz,
                                         float *out_x, float *out_y, float *out_z);

/* Coarse stats exposed for the editor / profiler. */
typedef struct {
    int polygon_count;
    int vertex_count;
    int detail_triangle_count;
    int build_time_ms;
} JceRecastStats;

JCE_API void jce_recast_get_stats(const JceRecastNavMesh *nm,
                                    JceRecastStats *out_stats);

JCE_EXTERN_C_END

#endif /* JCE_NAVMESH_RECAST_H */
