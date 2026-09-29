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

/* The CLEARANCE this mesh was built for, in metres -- the walkable_height the
 * bake handed Recast, which dtCreateNavMeshData writes into the tile header
 * and which therefore survives into the .navmesh.bin.
 *
 * 0 when the mesh is empty or has no tiles, which is UNKNOWN and not zero: a
 * check that fires on missing data rejects every scene authored before it. */
JCE_API float jce_recast_agent_height(const JceRecastNavMesh *nav);

/* The CLEARANCE this mesh was built for, in metres -- the walkable_height the
 * bake handed Recast, which dtCreateNavMeshData writes into the tile header
 * and which therefore survives into the .navmesh.bin.
 *
 * 0 when the mesh is empty or has no tiles, which is UNKNOWN and not zero: a
 * check that fires on missing data rejects every scene authored before it. */
JCE_API float jce_recast_agent_height(const JceRecastNavMesh *nav);

/* The CLEARANCE this mesh was built for, in metres -- the walkable_height the
 * bake handed Recast, which dtCreateNavMeshData writes into the tile header
 * and which therefore survives into the .navmesh.bin.
 *
 * 0 when the mesh is empty or has no tiles, which is UNKNOWN and not zero: a
 * check that fires on missing data rejects every scene authored before it. */
JCE_API float jce_recast_agent_height(const JceRecastNavMesh *nav);

/* Coarse stats exposed for the editor / profiler. */
typedef struct {
    int polygon_count;
    int vertex_count;
    int detail_triangle_count;
    int build_time_ms;
} JceRecastStats;

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

/* ── Persistence (editor bake -> runtime load) ──────────────────── *
 *
 * jce_recast_build_to_file builds a navmesh from the same triangle
 * soup as jce_recast_build and serialises the resulting single-tile
 * Detour navmesh to `path` (a small JCE header + the raw dtNavMesh
 * tile blob).  Optional `out_stats` receives the build stats.  Returns
 * true on success.  This is the editor/cooker bake entry-point.
 *
 * jce_recast_load_file reads a file written by jce_recast_build_to_file
 * and returns a ready-to-query JceRecastNavMesh (NULL on failure).
 * This is the runtime load entry-point. */
JCE_API bool jce_recast_build_to_file(const char     *path,
                                      const float    *vertices,
                                      uint32_t        vertex_count,
                                      const uint32_t *indices,
                                      uint32_t        triangle_count,
                                      const JceRecastConfig *cfg,
                                      JceRecastStats *out_stats);

JCE_API JceRecastNavMesh *jce_recast_load_file(const char *path);

/* ── Query API ───────────────────────────────────────────────────── *
 *
 * `out_xz` receives waypoint pairs (x,z) — y is dropped to match
 * JceNavAgent's XZ convention.  Returns 0 if no path exists.
 */
JCE_API int  jce_recast_find_path(const JceRecastNavMesh *nm,
                                   float start_x, float start_z,
                                   float goal_x,  float goal_z,
                                   float *out_xz, int max_pairs);

/* 3D variant of jce_recast_find_path: emits XYZ triples instead of XZ pairs,
 * so multi-floor navigation gets a real per-waypoint Y.
 *
 * `out_xyz` receives waypoint triples (x,y,z); its length must be at least
 * max_pts*3 floats.  Start/goal Y is honoured (the start/goal are snapped to
 * the nearest walkable polygon in 3D, not forced to Y=0).  Each emitted
 * waypoint's Y is resolved on the polygon it lies on via
 * dtNavMeshQuery::getPolyHeight, falling back to the straight-path height
 * (and a findNearestPoly+getPolyHeight probe) when the per-point polyRef is
 * unavailable.  Returns the number of waypoints written, 0 if no path exists.
 *
 * The 2D jce_recast_find_path is unchanged and remains the JceNavAgent path. */
JCE_API int  jce_recast_find_path_3d(const JceRecastNavMesh *nm,
                                     float start_x, float start_y, float start_z,
                                     float goal_x,  float goal_y,  float goal_z,
                                     float *out_xyz, int max_pts);

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

/* Debug visualisation: invoke `fn` once per polygon edge of every ground
 * polygon in the navmesh (a/b are world-space endpoints; `boundary` is
 * true for outer edges with no neighbouring polygon).  Returns the number
 * of edges emitted. */
typedef void (*JceRecastEdgeFn)(void *user, const float a[3], const float b[3], bool boundary);
JCE_API int jce_recast_debug_edges(const JceRecastNavMesh *nm, JceRecastEdgeFn fn, void *user);

JCE_API void jce_recast_get_stats(const JceRecastNavMesh *nm,
                                    JceRecastStats *out_stats);

/* ── Dynamic obstacles (DetourTileCache) ─────────────────────────── *
 *
 * The builders above produce a *static* single-tile navmesh: fast to bake
 * and query, but the walkable set is frozen at bake time.  For runtime
 * obstacles (a crate dropped on a corridor, a door that closes) we need a
 * navmesh whose tiles can be re-cut on the fly.  jce_recast_build_tiled
 * bakes the same triangle soup into a *tile-cache-backed* navmesh: the
 * geometry is sliced into a grid of compressed tiles managed by a live
 * dtTileCache, and obstacles can be stamped into / removed from it at
 * runtime, which rebuilds only the affected tiles so subsequent path
 * queries route around them.
 *
 * The returned handle is the *same* JceRecastNavMesh type used by the
 * static path, so every query function above (jce_recast_find_path,
 * _3d, _snap_to_navmesh, _debug_edges, _get_stats, _path_fn) works
 * unchanged on a tiled navmesh.  jce_recast_destroy frees it either way.
 *
 * The obstacle functions below are no-ops (return 0 / false) on a navmesh
 * that was NOT built with jce_recast_build_tiled — the static path is
 * fully preserved and additive.
 */
JCE_API JceRecastNavMesh *jce_recast_build_tiled(const float    *vertices,
                                                 uint32_t        vertex_count,
                                                 const uint32_t *indices,
                                                 uint32_t        triangle_count,
                                                 const JceRecastConfig *cfg);

/* Opaque obstacle handle.  0 is the invalid/null obstacle ref. */
typedef uint32_t JceRecastObstacleRef;

/* Stamp a vertical cylinder obstacle into the tile cache and rebuild the
 * tiles it touches so queries route around it.  `pos` is the centre of the
 * cylinder *base* (x,y,z); the cylinder extends upward by `height`.
 * Returns a non-zero obstacle ref on success, 0 on failure (no tile cache,
 * obstacle pool full, or out of bounds). */
JCE_API JceRecastObstacleRef jce_recast_add_obstacle(JceRecastNavMesh *nm,
                                                     float pos_x, float pos_y, float pos_z,
                                                     float radius, float height);

/* Stamp an axis-aligned box obstacle (world-space min/max corners) into the
 * tile cache.  Returns a non-zero obstacle ref on success, 0 on failure. */
JCE_API JceRecastObstacleRef jce_recast_add_box_obstacle(JceRecastNavMesh *nm,
                                                         float min_x, float min_y, float min_z,
                                                         float max_x, float max_y, float max_z);

/* Remove a previously added obstacle and rebuild the tiles it touched, so
 * queries route through the freed area again.  Returns true on success. */
JCE_API bool jce_recast_remove_obstacle(JceRecastNavMesh *nm,
                                        JceRecastObstacleRef ref);

/* True if this navmesh carries a live dtTileCache (built via
 * jce_recast_build_tiled) and therefore supports dynamic obstacles. */
JCE_API bool jce_recast_has_tile_cache(const JceRecastNavMesh *nm);

JCE_EXTERN_C_END

#endif /* JCE_NAVMESH_RECAST_H */
