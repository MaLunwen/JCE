/*
 * jce_world_origin.c -- Floating-origin large-world coordinate core.
 *
 * Pure math (math.h only).  No bgfx, no physics, no flecs, no allocation, no
 * RNG, no globals.  See jce_world_origin.h for the design and the load-bearing
 * invariant  absolute = origin + local  preserved across every rebase.
 */

#include <jce/middleware/scene/jce_world_origin.h>

#include <math.h>

JceWorldOrigin jce_world_origin_default(float threshold)
{
    JceWorldOrigin wo;
    wo.origin[0] = 0.0;
    wo.origin[1] = 0.0;
    wo.origin[2] = 0.0;
    /* Clamp to a small positive value so a zero/negative threshold cannot turn
     * every frame into a rebase (which would thrash + defeat determinism). */
    wo.rebase_threshold = (threshold > 0.0f) ? threshold : 1.0f;
    return wo;
}

/* Floor a signed value to the nearest grid multiple toward negative infinity.
 * floorf gives a deterministic, grid-aligned result for both signs (e.g.
 *   5000 / 256 -> floor 19.53 -> 19 -> 4864
 *  -5000 / 256 -> floor -19.53 -> -20 -> -5120 ).
 * Grid alignment keeps the accumulated double origin on exact multiples. */
static float wo_quantize(float v, float quantum)
{
    return floorf(v / quantum) * quantum;
}

int jce_world_origin_update(JceWorldOrigin *wo,
                            const float camera_local[3],
                            float out_shift[3])
{
    if (out_shift) {
        out_shift[0] = 0.0f;
        out_shift[1] = 0.0f;
        out_shift[2] = 0.0f;
    }
    if (!wo || !camera_local)
        return 0;

    float dist2 = camera_local[0] * camera_local[0]
                + camera_local[1] * camera_local[1]
                + camera_local[2] * camera_local[2];
    float thr   = wo->rebase_threshold;
    if (dist2 <= thr * thr)
        return 0;                       /* within the comfortable float range */

    /* Chosen shift = camera position quantized to the rebase grid. */
    float shift[3];
    shift[0] = wo_quantize(camera_local[0], JCE_WORLD_ORIGIN_QUANTUM);
    shift[1] = wo_quantize(camera_local[1], JCE_WORLD_ORIGIN_QUANTUM);
    shift[2] = wo_quantize(camera_local[2], JCE_WORLD_ORIGIN_QUANTUM);

    /* If quantization collapsed the shift to nothing (camera over threshold but
     * inside one quantum of the grid origin on every axis), do not rebase —
     * advancing the origin by zero would just churn the world cache. */
    if (shift[0] == 0.0f && shift[1] == 0.0f && shift[2] == 0.0f)
        return 0;

    /* Advance the double origin; emit out_shift = -shift so the caller adds it
     * to every local position, pulling the camera back toward the origin.
     * origin += shift and local += -shift keep absolute = origin + local. */
    wo->origin[0] += (double)shift[0];
    wo->origin[1] += (double)shift[1];
    wo->origin[2] += (double)shift[2];
    if (out_shift) {
        out_shift[0] = -shift[0];
        out_shift[1] = -shift[1];
        out_shift[2] = -shift[2];
    }
    return 1;
}

void jce_world_origin_to_absolute(const JceWorldOrigin *wo,
                                  const float local[3],
                                  double out_abs[3])
{
    if (!out_abs || !local)
        return;
    double ox = wo ? wo->origin[0] : 0.0;
    double oy = wo ? wo->origin[1] : 0.0;
    double oz = wo ? wo->origin[2] : 0.0;
    out_abs[0] = ox + (double)local[0];
    out_abs[1] = oy + (double)local[1];
    out_abs[2] = oz + (double)local[2];
}

void jce_world_origin_to_local(const JceWorldOrigin *wo,
                               const double abs[3],
                               float out_local[3])
{
    if (!out_local || !abs)
        return;
    double ox = wo ? wo->origin[0] : 0.0;
    double oy = wo ? wo->origin[1] : 0.0;
    double oz = wo ? wo->origin[2] : 0.0;
    out_local[0] = (float)(abs[0] - ox);
    out_local[1] = (float)(abs[1] - oy);
    out_local[2] = (float)(abs[2] - oz);
}
