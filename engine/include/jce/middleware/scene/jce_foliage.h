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
} JceFoliageScatterParams;

/* Upper bound on instances produced by one scatter call (≈4 MB of mat4). */
#define JCE_FOLIAGE_MAX_INSTANCES 65536u

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
