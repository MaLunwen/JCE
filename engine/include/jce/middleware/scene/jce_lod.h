/*
 * jce_lod.h  Distance-based Level-of-Detail (LOD) selection.
 *
 * A JceLodGroup stores up to JCE_LOD_MAX_LEVELS mesh variants ordered
 * from highest to lowest detail, each with an upper distance bound.
 * jce_lod_pick() returns the level index a given camera distance maps
 * to, applying hysteresis around level boundaries to prevent visual
 * popping when the distance fluctuates near a threshold.
 *
 * Selection rule
 *   Forward (away from camera, prev_level → prev_level+1) requires
 *     distance > thr * (1 + hysteresis).
 *   Backward (toward camera, prev_level → prev_level-1) requires
 *     distance < thr_prev * (1 - hysteresis).
 *   Otherwise the previous level is preserved (dead-zone).
 *
 * Returns
 *   0..count-1 selected level, or -1 if distance exceeds the last
 *   level's threshold (caller treats as "cull").
 *
 * Layer: Scene (Layer 5).
 */

#ifndef JCE_LOD_H
#define JCE_LOD_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_LOD_MAX_LEVELS 8

typedef struct JceMesh JceMesh;

typedef struct {
    JceMesh *mesh;       /* mesh for this LOD; NULL = invisible (cull) */
    float    distance;   /* upper bound (world meters); use FLT_MAX for last level */
} JceLodLevel;

typedef struct JceLodGroup {
    JceLodLevel levels[JCE_LOD_MAX_LEVELS];
    int         count;       /* number of populated levels (0..MAX) */
    float       hysteresis;  /* 0..0.5; recommend 0.05 = 5% dead-zone */
} JceLodGroup;

/* Zero out the group and set hysteresis to a sane default (0.05). */
JCE_API void jce_lod_init(JceLodGroup *g);

/* Convenience: configure 1, 2, or 3 levels in one call.
 * Pass NULL for unused mid/low slots. Distances must be ascending. */
JCE_API void jce_lod_setup(JceLodGroup *g,
                            JceMesh *high, float d_high,
                            JceMesh *mid,  float d_mid,
                            JceMesh *low,  float d_low);

/* Pick a level for the given distance.
 * prev_level: pass -1 on first call; otherwise feed back the previous
 *             return value to honour hysteresis.
 * Returns 0..count-1 on hit; -1 on cull (distance past last level). */
JCE_API int jce_lod_pick(const JceLodGroup *g, float distance, int prev_level);

JCE_EXTERN_C_END

#endif /* JCE_LOD_H */
