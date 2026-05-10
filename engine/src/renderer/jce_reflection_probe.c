/*
 * jce_reflection_probe.c  Pick the best 1–2 reflection probes.
 *
 * Algorithm:
 *   1. Walk each probe and compute "influence weight" at world_pos:
 *      - Inside-volume distance →  1 / (1 + dist/blend_distance) so
 *        weight peaks near the surface and falls off smoothly.
 *      - Outside the volume by more than blend_distance → 0.
 *   2. Pick the highest-priority probe with weight > 0 as primary.
 *   3. Pick the next-best (different priority OR equal-priority with
 *      different cubemap) as secondary.
 *   4. Normalise the two weights so they sum to 1.
 *
 * This is the same approach Unity uses (in spirit) for its blend-
 * between-probes feature.  Real engines do per-pixel weighted blends
 * via screen-space anchors; here we pick a single pair per object and
 * let the shader lerp.
 */

#include <jce/renderer/jce_reflection_probe.h>

#include <math.h>

static float compute_weight(jce_vec3 world_pos, const JceReflProbe *p)
{
    if (p->priority < 0) return 0.0f;
    if (p->blend_distance <= 0.0f) return 0.0f;

    float dist;  /* signed — negative inside, positive outside */
    if (p->shape == JCE_REFL_PROBE_SPHERE) {
        float dx = world_pos.x - p->center.x;
        float dy = world_pos.y - p->center.y;
        float dz = world_pos.z - p->center.z;
        float d  = sqrtf(dx*dx + dy*dy + dz*dz);
        float r  = p->half_extents.x;
        if (r <= 0.0f) return 0.0f;
        dist = d - r;
    } else {
        /* Box: convert to local-aligned distance to AABB surface. */
        float lx = fabsf(world_pos.x - p->center.x) - p->half_extents.x;
        float ly = fabsf(world_pos.y - p->center.y) - p->half_extents.y;
        float lz = fabsf(world_pos.z - p->center.z) - p->half_extents.z;
        if (lx < 0 && ly < 0 && lz < 0) {
            /* Inside box — choose the deepest negative axis distance. */
            float deepest = lx;
            if (ly > deepest) deepest = ly;
            if (lz > deepest) deepest = lz;
            dist = deepest;
        } else {
            float ox = lx > 0 ? lx : 0.0f;
            float oy = ly > 0 ? ly : 0.0f;
            float oz = lz > 0 ? lz : 0.0f;
            dist = sqrtf(ox*ox + oy*oy + oz*oz);
        }
    }

    if (dist >= p->blend_distance) return 0.0f;
    /* Inside or within blend distance.  Map to 1 at center → 0 at
     * blend_distance away. */
    if (dist < 0.0f) {
        /* Inside: deeper inside = higher confidence. */
        return 1.0f;
    }
    float t = dist / p->blend_distance;  /* 0..1 */
    /* Smoothstep so transitions don't pop. */
    return 1.0f - (t * t * (3.0f - 2.0f * t));
}

JceReflProbeBlend jce_reflection_probe_select(jce_vec3 wp,
                                               const JceReflProbe *probes,
                                               uint32_t n)
{
    JceReflProbeBlend out = { 0, {0, 0}, {0.0f, 0.0f} };
    if (!probes || n == 0) return out;

    int best_a = -1, best_b = -1;
    float wa = 0.0f, wb = 0.0f;

    for (uint32_t i = 0; i < n; ++i) {
        float w = compute_weight(wp, &probes[i]);
        if (w <= 0.0f) continue;
        /* Prioritised insertion: higher priority always wins; for ties,
         * higher weight wins. */
        if (best_a < 0
            || probes[i].priority > probes[best_a].priority
            || (probes[i].priority == probes[best_a].priority && w > wa)) {
            best_b = best_a;  wb = wa;
            best_a = (int)i;  wa = w;
        } else if (best_b < 0
                   || probes[i].priority > probes[best_b].priority
                   || (probes[i].priority == probes[best_b].priority && w > wb)) {
            best_b = (int)i;  wb = w;
        }
    }

    if (best_a < 0) return out;

    if (best_b < 0) {
        out.count = 1;
        out.cubemap[0] = probes[best_a].cubemap_handle;
        out.weight[0]  = 1.0f;
        return out;
    }

    /* Normalise. */
    float total = wa + wb;
    if (total <= 0.0f) total = 1.0f;
    out.count = 2;
    out.cubemap[0] = probes[best_a].cubemap_handle;
    out.cubemap[1] = probes[best_b].cubemap_handle;
    out.weight[0]  = wa / total;
    out.weight[1]  = wb / total;
    return out;
}
