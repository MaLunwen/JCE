/*
 * jce_fracture.h  Voronoi shatter of a convex shape (CPU geometry core).
 *
 * Pure, deterministic geometry: given a convex source shape (start with an
 * axis-aligned box) and a set of interior seed points, this produces one
 * convex FRAGMENT cell per seed — the Voronoi cell of that seed clipped to
 * the source shape.  No physics, no ECS, no graphics: the output is owned
 * point clouds (one convex hull per cell) plus per-cell centroid and volume.
 *
 * The runtime turns each cell into a dynamic CONVEX_HULL rigid body when a
 * fracturable entity breaks (see jce_runtime_fracture_entity); that body-swap
 * is a separate, opt-in step.  This header is the reusable math kernel and is
 * unit-tested headless (tests/middleware/physics/test_jce_fracture.c).
 *
 * ── Algorithm ────────────────────────────────────────────────────────────
 *   Represent the source shape as a set of bounding HALF-SPACES.  For an
 *   AABB that is the 6 planes ±x, ±y, ±z (each plane: a point ON the plane
 *   and an inward normal; a point p is INSIDE the half-space when
 *   dot(normal, p - point) <= 0, i.e. it lies on the inward side).
 *
 *   Fragment i (the Voronoi cell of seed i) is the source half-space set
 *   intersected with, for every OTHER seed j, the bisector half-plane
 *   between seed i and seed j: the plane through the midpoint (seed_i+seed_j)/2
 *   with outward normal (seed_j - seed_i).  Keeping the side that contains
 *   seed_i means the cell is exactly the locus of points closer to seed_i
 *   than to any other seed — the Voronoi region — clipped to the box.
 *
 *   The vertices of that convex cell are enumerated by HALF-SPACE
 *   INTERSECTION: every triple of planes (p,q,r) in the cell's half-space
 *   set is solved as a 3x3 linear system for their common point; the point
 *   is kept iff it satisfies EVERY half-space of the cell (within epsilon).
 *   Coincident vertices are merged.  The surviving points are the cell's
 *   convex-hull vertices.  Cell volume is the signed volume of the convex
 *   hull (tetrahedra fanned from the cell centroid over the hull faces).
 *
 * ── Determinism ──────────────────────────────────────────────────────────
 *   The same inputs (box + seed array) always yield bit-identical cells:
 *   the plane build order, triple-enumeration order, and vertex-merge order
 *   are fixed and independent of any RNG.  The optional seed scatterer uses a
 *   small deterministic PRNG so a given (aabb, count, seed) is reproducible.
 *
 * Thread safety: the functions are pure (no global state); distinct output
 * structures may be produced on different threads concurrently.
 */

#ifndef JCE_FRACTURE_H
#define JCE_FRACTURE_H

#include <jce/os/core/jce_defs.h>

JCE_EXTERN_C_BEGIN

/* Max convex-hull vertices retained per fragment cell.  A Voronoi cell of a
 * box has at most ~ (box faces + neighbour bisectors) planes; 64 is generous
 * for the box-shatter slice and bounds the owned per-cell storage. */
#define JCE_FRACTURE_MAX_CELL_VERTS 64

/*
 * One convex fragment.  `verts` holds `vcount` xyz triplets (the convex-hull
 * vertices, in no particular winding — the runtime rebuilds a convex hull
 * from them).  `centroid` is the vertex average; `volume` is the cell's
 * signed volume (always >= 0 for a valid cell).
 */
typedef struct {
    float verts[JCE_FRACTURE_MAX_CELL_VERTS][3];
    int   vcount;
    float centroid[3];
    float volume;
} JceFractureCell;

/*
 * Owned result of a shatter.  `cells` is heap-allocated (jce_malloc) with
 * `cell_count` entries; release with jce_fracture_free.
 */
typedef struct {
    JceFractureCell *cells;
    int              cell_count;
} JceFractureResult;

/*
 * Voronoi-shatter an axis-aligned box into one convex cell per seed point.
 *
 *   aabb_min / aabb_max : the box bounds (min must be < max per axis).
 *   seed_points         : n_seeds xyz triplets, ideally INSIDE the box (a
 *                         seed outside is clamped into the box).
 *   n_seeds             : number of seeds.  <= 1 produces a single cell that
 *                         is the whole box (8 verts).
 *   out                 : receives the owned cell array (out->cells / count).
 *
 * Returns the number of cells produced (== n_seeds for a non-degenerate
 * input, 1 for n_seeds <= 1), or 0 on allocation failure / invalid args.
 * On success the caller owns `out` and must release it with jce_fracture_free.
 */
JCE_API int JCE_CALL jce_fracture_box(const float aabb_min[3],
                                      const float aabb_max[3],
                                      const float *seed_points,
                                      int n_seeds,
                                      JceFractureResult *out);

/* Release the cell array owned by a JceFractureResult.  Safe on NULL and on
 * an already-zeroed / never-populated result (idempotent). */
JCE_API void JCE_CALL jce_fracture_free(JceFractureResult *r);

/*
 * Deterministically scatter `n` seed points inside the AABB for convenience /
 * testing.  Writes n xyz triplets into `out_seeds` (which must hold n*3
 * floats).  The same (aabb, n, seed) always yields the same points.  Returns
 * the number of seeds written (n, clamped to >= 0), or 0 on invalid args.
 * Seeds are kept strictly interior (a small inset from the faces) so that
 * every cell is non-degenerate.
 */
JCE_API int JCE_CALL jce_fracture_scatter_seeds(const float aabb_min[3],
                                                const float aabb_max[3],
                                                int n, uint32_t seed,
                                                float *out_seeds);

JCE_EXTERN_C_END

#endif /* JCE_FRACTURE_H */
