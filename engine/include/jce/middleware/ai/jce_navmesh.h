/*
 * jce_navmesh.h -- NavMesh runtime (P1-M backend).
 *
 * Loads .navmesh.json files authored by the editor's NavMesh panel and
 * exposes a 2D grid walkability query plus an A* path-find that
 * returns world-space waypoints.  This is the grid backend; a future
 * Recast/Detour backend will replace the implementation while keeping
 * this same public API surface.
 *
 * Layer: AI / Navigation (Layer 3) — public.
 */

#ifndef JCE_NAVMESH_PUBLIC_H
#define JCE_NAVMESH_PUBLIC_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceNavMesh JceNavMesh;

/* -- Lifecycle --------------------------------------------------- */

JCE_API JceNavMesh *jce_navmesh_load_file(const char *path);
JCE_API JceNavMesh *jce_navmesh_load_text(const char *text, size_t len);
JCE_API void        jce_navmesh_free(JceNavMesh *nm);

/* -- Grid info --------------------------------------------------- */

JCE_API int   jce_navmesh_grid_x(const JceNavMesh *nm);
JCE_API int   jce_navmesh_grid_z(const JceNavMesh *nm);
JCE_API float jce_navmesh_cell  (const JceNavMesh *nm);

/* The CLEARANCE this mesh was carved for, in metres -- the walkable_height the
 * bake handed Recast, recorded in the file's settings block.
 *
 * 0 means the file does not say, which is UNKNOWN and not zero: a mesh written
 * before this was read back must not start rejecting agents.  A caller
 * comparing an agent against it has to treat 0 as "no opinion".
 *
 * Why it matters: a navmesh belongs to the agent it was built for.  An agent
 * taller than this clearance is not slightly wrong on this mesh, it is on the
 * wrong mesh -- it will walk under geometry it cannot fit under, and nothing
 * about the steering will look broken while it does. */
JCE_API float jce_navmesh_agent_height(const JceNavMesh *nm);
JCE_API void  jce_navmesh_origin(const JceNavMesh *nm, float *out_x, float *out_z);

/* -- Queries ----------------------------------------------------- */

/* World-space (XZ plane) walkability query. */
JCE_API bool jce_navmesh_is_walkable(const JceNavMesh *nm, float wx, float wz);

/* Find a path from (sx, sz) to (gx, gz) on the grid.  Writes up to
 * `max_points` world-space waypoints (XZ; Y is left untouched at 0)
 * into out_xz[0..2*max_points-1] and returns the number of points
 * written.  Returns 0 when no path is found.
 *
 * The path is straight-line from start to first reachable cell, then
 * cell-centre waypoints (no LOS smoothing yet).  Coarse but functional. */
JCE_API int JCE_CALL jce_navmesh_find_path(const JceNavMesh *nm,
                                           float sx, float sz, float gx, float gz,
                                           float *out_xz, int max_points);

JCE_EXTERN_C_END

#endif /* JCE_NAVMESH_PUBLIC_H */
