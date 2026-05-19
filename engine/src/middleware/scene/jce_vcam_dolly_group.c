/*
 * jce_vcam_dolly_group.c  DollyCart + Group Composer evaluators.
 */

#include <jce/middleware/scene/jce_vcam_dolly_group.h>

#include <math.h>
#include <string.h>

/* ── DollyCart ───────────────────────────────────────────────── */

static float segment_length(const float a[3], const float b[3])
{
    float dx = b[0] - a[0];
    float dy = b[1] - a[1];
    float dz = b[2] - a[2];
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

static float path_total_length(const JceVcamDollyCart *d)
{
    if (!d || d->waypoint_count < 2) return 0.0f;
    float L = 0;
    for (uint32_t i = 0; i + 1 < d->waypoint_count; ++i)
        L += segment_length(d->waypoints[i], d->waypoints[i + 1]);
    if (d->loop && d->waypoint_count >= 2)
        L += segment_length(d->waypoints[d->waypoint_count - 1], d->waypoints[0]);
    return L;
}

void jce_vcam_dolly_apply(JceVcamDollyCart *d, float dt, float out_pos[3])
{
    if (!d || !out_pos) return;
    out_pos[0] = out_pos[1] = out_pos[2] = 0.0f;
    if (d->waypoint_count == 0) return;
    if (d->waypoint_count == 1) {
        memcpy(out_pos, d->waypoints[0], 3 * sizeof(float));
        return;
    }

    if (d->auto_advance) {
        float total = path_total_length(d);
        if (total > 0.0f && d->speed != 0.0f) {
            d->position_along_path += (d->speed * dt) / total;
            if (d->loop) {
                while (d->position_along_path > 1.0f) d->position_along_path -= 1.0f;
                while (d->position_along_path < 0.0f) d->position_along_path += 1.0f;
            } else {
                if (d->position_along_path < 0.0f) d->position_along_path = 0.0f;
                if (d->position_along_path > 1.0f) d->position_along_path = 1.0f;
            }
        }
    }

    /* Sample at position_along_path ∈ [0,1] via accumulated arc length. */
    float total = path_total_length(d);
    if (total <= 0.0f) {
        memcpy(out_pos, d->waypoints[0], 3 * sizeof(float));
        return;
    }
    float target = d->position_along_path * total;
    float accum = 0;
    uint32_t n = d->waypoint_count + (d->loop ? 1 : 0);
    for (uint32_t i = 0; i + 1 < n; ++i) {
        uint32_t a = i % d->waypoint_count;
        uint32_t b = (i + 1) % d->waypoint_count;
        float seg = segment_length(d->waypoints[a], d->waypoints[b]);
        if (target <= accum + seg || i + 2 == n) {
            float u = seg > 0 ? (target - accum) / seg : 0.0f;
            if (u < 0) u = 0;
            if (u > 1) u = 1;
            for (int k = 0; k < 3; ++k)
                out_pos[k] = d->waypoints[a][k] * (1 - u) +
                              d->waypoints[b][k] * u;
            return;
        }
        accum += seg;
    }
    memcpy(out_pos, d->waypoints[d->waypoint_count - 1], 3 * sizeof(float));
}

/* ── Group Composer ──────────────────────────────────────────── */

void jce_vcam_group_apply(const JceVcamGroupComposer *g,
                            float vertical_fov_deg, float aspect,
                            float out_lookat[3], float *out_distance)
{
    if (!g || !out_lookat) return;
    out_lookat[0] = out_lookat[1] = out_lookat[2] = 0.0f;
    if (g->target_count == 0) {
        if (out_distance) *out_distance = g->min_distance;
        return;
    }
    /* Weighted centroid. */
    float total_w = 0;
    for (uint32_t i = 0; i < g->target_count; ++i)
        total_w += g->target_weight[i];
    if (total_w < 1e-6f) total_w = 1.0f;
    for (uint32_t i = 0; i < g->target_count; ++i) {
        float w = g->target_weight[i] / total_w;
        out_lookat[0] += g->target_pos[i][0] * w;
        out_lookat[1] += g->target_pos[i][1] * w;
        out_lookat[2] += g->target_pos[i][2] * w;
    }
    if (!out_distance) return;

    /* Bounding circle radius = max(dist_to_centroid + target_radius). */
    float radius = 0;
    for (uint32_t i = 0; i < g->target_count; ++i) {
        float dx = g->target_pos[i][0] - out_lookat[0];
        float dy = g->target_pos[i][1] - out_lookat[1];
        float dz = g->target_pos[i][2] - out_lookat[2];
        float r  = sqrtf(dx*dx + dy*dy + dz*dz) + g->target_radius[i];
        if (r > radius) radius = r;
    }
    if (!g->adjust_distance) {
        *out_distance = g->min_distance;
        return;
    }
    /* Distance to fit bounding circle in vertical FOV: d = r / tan(fov/2). */
    float half = vertical_fov_deg * 0.5f * 0.01745329f;
    float t = tanf(half);
    /* Use the narrower of vertical/horizontal axes for safety. */
    if (aspect > 0.0f && aspect < 1.0f) t *= aspect;
    float d = (t > 1e-4f) ? radius / t : g->min_distance;
    if (d < g->min_distance) d = g->min_distance;
    if (g->max_distance > 0 && d > g->max_distance) d = g->max_distance;
    *out_distance = d;
}
