/*
 * jce_foliage.h -- Deterministic vegetation scatter (foliage / grass / trees).
 *
 * Pure, side-effect-free placement math: given scatter parameters, an optional
 * terrain heightfield, and a world-space origin, it fills a caller buffer with
 * per-instance transforms (position + Y rotation + uniform scale).  The scene
 * renderer turns that buffer into a single GPU-instanced draw; keeping the
 * scatter here (decoupled from the scene/ECS and the renderer) makes it unit
 * testable and reusable.
 *
 * Determinism: identical params + terrain + origin always yield identical
 * output (xorshift32 seeded from `seed`), so the renderer can cache the buffer
 * and rebuild only when a parameter changes.
 *
 * Layer: Middleware / Scene (Layer 4).
 */

#ifndef JCE_FOLIAGE_H
#define JCE_FOLIAGE_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceTerrain JceTerrain;

/* One scattered instance in world space. */
typedef struct {
    float pos[3];     /* world position (Y from terrain when present) */
    float rot_y;      /* random yaw in radians */
    float scale;      /* uniform scale */
    /* Terrain surface normal at pos, unit length; (0,1,0) with no terrain or
     * when normals were not requested.  This exists because align_to_normal
     * was authorable, serialized and round-trip tested for a long time while
     * doing nothing: the scatter computed a terrain gradient for its slope
     * test and then threw it away, and the instance matrix only ever built a
     * yaw rotation.  Carrying the normal is what makes the toggle real. */
    float normal[3];
} JceFoliageInstance;

/* Scatter inputs (mirrors the authorable fields of JceVegetationScatterComponent
 * but stays decoupled from the scene component for testability). */
typedef struct {
    uint32_t seed;          /* RNG seed (0 is remapped to a fixed non-zero) */
    float    density;       /* instances per square world unit (clamped >= 0) */
    float    area_x;        /* scatter rectangle size on X (world units) */
    float    area_z;        /* scatter rectangle size on Z (world units) */
    float    max_slope_deg; /* skip terrain steeper than this; >=90 disables */
    float    scale_min;     /* per-instance uniform scale range (min<=max)   */
    float    scale_max;

    /* Optional density mask (large-world #8a foliage brush): a mask_dim×mask_dim
     * row-major grid of [0,1] values over the area rect (origin±area/2 mapped to
     * [0,1]²).  Each candidate is kept with probability = its mask cell value, so
     * brush-painted sparse regions thin out and dense regions stay full.  NULL /
     * mask_dim<=0 means uniform full density (byte-identical to the maskless
     * scatter — no extra RNG draw). */
    const float *density_mask;
    int          mask_dim;
    /* When > 0, the mask is sampled in WORLD space: the mask's [0,1]² UV spans
     * a mask_world_size × mask_world_size world square centred on the origin,
     * so a mask shared by several scatter rects (e.g. grass strips ringing a
     * pond) stays aligned to the same world texture.  0 = legacy local-rect UV
     * (candidate position within its own area rect). */
    float        mask_world_size;

    /* Fill JceFoliageInstance.normal from the terrain surface.
     *
     * Off by default so existing callers pay nothing: deriving the normal costs
     * four extra height samples per candidate.  When max_slope_deg already
     * enables the slope test those same four samples are reused, so turning
     * this on alongside slope limiting is free.
     *
     * Does NOT consume randomness, so the RNG draw order -- and therefore the
     * exact set of kept instances -- is byte-identical whether or not it is
     * enabled.  That invariant is what keeps scatter cookable. */
    bool         want_normals;

    /* ── Surface-type rules ────────────────────────────────────────────
     *
     * Slope alone cannot express where a plant actually grows.  These add the
     * two remaining axes an artist reaches for: how HIGH it is, and whether it
     * sits in a crease or on an exposed ridge.  Together with max_slope_deg
     * they replace most hand-painted density masks -- a treeline, a scree
     * band, willows in the gullies -- with rules that survive the terrain
     * being regenerated underneath them.
     *
     * CRITICAL INVARIANT: none of these consume randomness.  They are pure
     * threshold rejections applied to a candidate that has already been drawn,
     * so the RNG sequence is byte-identical whether they are on or off, and a
     * ruled scatter is a strict SUBSET of the unruled one at the same seed.
     * That is what keeps scatter cookable -- an offline bake and a runtime
     * scatter must agree instance for instance. */

    /* Altitude band in WORLD Y.  Candidates outside are skipped.  min >= max
     * disables the test, so a zeroed struct keeps the old behaviour. */
    float        height_min;
    float        height_max;

    /* Ridge/crease preference, driven by the cooked ridge channel
     * (jce_terrain_apply_erosion / the v3 CH_RIDGE plane): -1 deep in a crease,
     * +1 on a ridge.  The field is sampled like density_mask -- a
     * ridge_dim x ridge_dim grid over the same world square, or over the area
     * rect when mask_world_size is 0.
     *
     * A candidate is kept when its ridge value lies in [ridge_min, ridge_max].
     * NULL / ridge_dim <= 0 / min >= max disables the test. */
    const float *ridge_field;
    int          ridge_dim;
    float        ridge_min;
    float        ridge_max;
} JceFoliageScatterParams;

/* Upper bound on instances produced by one scatter call (≈4 MB of mat4). */
#define JCE_FOLIAGE_MAX_INSTANCES 65536u

/* ── Cooked placement ──────────────────────────────────────────────────
 *
 * Scatter is deterministic, so a level can either compute its instances at
 * load time or ship them precomputed.  Shipping them is what a large world
 * wants: the cost stops scaling with instance count, and -- more usefully --
 * an artist can bake, inspect, and know that what shipped is exactly what was
 * reviewed.
 *
 * This is only sound because the surface rules consume no randomness (see
 * JceFoliageScatterParams).  A cooked list and a live scatter at the same seed
 * agree instance for instance; if a rule ever drew from the RNG the two would
 * diverge silently, and no amount of hashing HERE would catch it, because both
 * halves would be internally consistent.
 *
 * The container records the instance STRIDE alongside the count.
 * JceFoliageInstance has grown once already (the surface normal was added long
 * after the type existed), and a stale file read at the wrong stride yields
 * plausible garbage rather than an error -- so the stride is checked, not
 * assumed. */

JCE_API size_t jce_foliage_cook_size(uint32_t count);

/* Serialise `count` instances.  `seed` is recorded so a consumer can tell
 * which scatter the list was baked from.  Returns false without writing when
 * the destination is too small. */
JCE_API bool jce_foliage_cook(const JceFoliageInstance *instances, uint32_t count,
                              uint32_t seed, void *dst, size_t dst_size,
                              size_t *out_written);

/* Instance count / seed recorded in a cooked blob.  0 for anything that is not
 * a readable blob of this version. */
JCE_API uint32_t jce_foliage_cooked_count(const void *data, size_t size);
JCE_API uint32_t jce_foliage_cooked_seed(const void *data, size_t size);

/* Load into `out` (capacity `out_cap`), returning the number written.
 *
 * Returns 0 -- writing nothing -- for a wrong magic or version, a mismatched
 * instance stride, a truncated buffer, or a hash mismatch.  A partially
 * decoded placement is worse than none: it would scatter a forest with a hole
 * in it and report success. */
JCE_API uint32_t jce_foliage_load_cooked(const void *data, size_t size,
                                         JceFoliageInstance *out, uint32_t out_cap);


/* Fill `out` (capacity `out_cap`) with scattered instances and return the count
 * written.  Candidates are drawn uniformly in the rectangle
 * [origin ± area/2] on XZ; when `terrain` is non-NULL each instance's Y is the
 * terrain height at its XZ and candidates on slopes steeper than max_slope_deg
 * are skipped (so the returned count may be < density*area).  With terrain
 * NULL, instances sit flat at origin->y.  Returns 0 for degenerate params. */
JCE_API uint32_t jce_foliage_scatter(const JceFoliageScatterParams *params,
                                     const JceTerrain              *terrain,
                                     const jce_vec3                *origin,
                                     JceFoliageInstance            *out,
                                     uint32_t                       out_cap);

JCE_EXTERN_C_END

#endif /* JCE_FOLIAGE_H */
