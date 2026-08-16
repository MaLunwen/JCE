/*
 * jce_horizon.h -- terrain sky occlusion by height-field horizon sweep.
 *
 * Sky ambient applied unoccluded lights the inside of a canyon exactly like
 * open sky.  This bakes, per height-field cell, how much sky that cell can
 * actually see, plus the direction that sky arrives from.  Cook-time pass:
 * pure CPU, single-threaded, no allocation, deterministic.
 *
 * ALGORITHM.  For each azimuth direction the grid is decomposed into parallel
 * sweep lines and each line is walked once, far end first, maintaining a stack
 * that is the upper convex hull of the samples already passed.  The maximum
 * elevation angle visible from a cell is a vertex of that hull, and the same
 * pop test that finds it also restores the hull, so the cost is amortised O(1)
 * per cell -- O(cells) per direction, O(directions * cells) overall.  The walk
 * is inherently sequential along a line; that is fine at cook time.
 *
 * OUTPUTS.
 *   visibility   in [0,1] -- mean over directions of cos^2(horizon elevation).
 *                cos^2 is the closed form of the cosine-weighted hemisphere
 *                integral over one azimuth slice, which is what diffuse
 *                irradiance actually wants; an unweighted angular fraction
 *                over-darkens because it treats grazing sky as fully valuable.
 *   bent normal  unit vector toward the least-occluded part of the hemisphere.
 *                Strictly more useful than the scalar: it also fixes the
 *                DIRECTION of incoming sky light, not just its magnitude.
 *                Always in the upper hemisphere (y > 0).
 *
 * A height field cannot express overhangs, so the result is the visibility of
 * the upper hemisphere at that cell -- it is not clipped to any surface normal.
 * Clip against the shading normal at use time if the shader wants that.
 *
 * LIMITS.  Both dimensions must be <= JCE_HORIZON_MAX_DIM: the hull stack is a
 * fixed-size local (~32 KB of stack) because this module allocates nothing and
 * the desc carries no scratch buffer.  Worst-case hull depth is a full sweep
 * line (a smooth dome makes every sample a hull vertex), so the bound cannot
 * be lowered without losing exactness.
 *
 * Layer: Scene / Terrain (Layer 3) -- public, engine-independent.
 */

#ifndef JCE_HORIZON_H
#define JCE_HORIZON_H

#include <stdbool.h>
#include <stdint.h>

/* Define to the engine's export macro (JCE_API) when compiling this into the
 * shared library; empty keeps the module standalone-buildable. */
#ifndef JCE_HORIZON_API
#define JCE_HORIZON_API
#endif

#ifdef __cplusplus
extern "C" {
#endif

#define JCE_HORIZON_MAX_DIM            4096u
#define JCE_HORIZON_MIN_DIRECTIONS        4u
#define JCE_HORIZON_MAX_DIRECTIONS       64u
#define JCE_HORIZON_DEFAULT_DIRECTIONS   16u

typedef struct JceHorizonDesc {
    const float *heights;      /* w*h, row-major, X fastest, world Y units */
    uint32_t     w, h;         /* 1 .. JCE_HORIZON_MAX_DIM */
    float        cell_size_x;  /* world spacing along X, > 0 */
    float        cell_size_z;  /* world spacing along Z, > 0 */
    uint32_t     directions;   /* azimuth samples, clamped to [4,64]; 0 = 16 */
} JceHorizonDesc;

/*
 * Bake sky occlusion for the whole field.  Caller owns all storage.
 *
 *   out_visibility    w*h floats in [0,1].         May be NULL.
 *   out_bent_normals  w*h * 3 floats, unit length. May be NULL.
 *
 * Both buffers are fully overwritten (no read-modify-write of prior content).
 * Returns false, touching nothing, on degenerate input: NULL desc or heights,
 * both outputs NULL, zero or over-large dimensions, or a cell size that is not
 * finite and positive.  Output is always finite for finite input; non-finite
 * heights degrade to "unoccluded" rather than propagating NaN.
 */
JCE_HORIZON_API bool jce_horizon_bake(const JceHorizonDesc *desc,
                                      float *out_visibility,
                                      float *out_bent_normals);

#ifdef __cplusplus
}
#endif

#endif /* JCE_HORIZON_H */