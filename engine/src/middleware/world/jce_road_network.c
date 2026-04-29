/*
 * jce_road_network.c -- road graph implementation.
 *
 * Storage: parallel growable arrays for points / segments / nodes.
 * On finalize() we build per-node adjacency lists for fast traversal.
 *
 * Geometry queries use a brute-force linear scan over segments — fine
 * for hundreds-to-low-thousands of segments.  For larger networks add
 * a spatial grid (TODO).
 */

#include <jce/middleware/world/jce_road_network.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "road_net"

struct JceRoadNetwork {
    JceRoadNode    *nodes;       uint32_t node_count;     uint32_t node_cap;
    JceRoadSegment *segments;    uint32_t segment_count;  uint32_t segment_cap;
    jce_vec3       *points;      uint32_t point_count;    uint32_t point_cap;
    uint32_t       *adjacency;   uint32_t adjacency_count;
    bool            finalized;
};

static bool grow_nodes(JceRoadNetwork *n, uint32_t need)
{
    if (n->node_cap >= need) return true;
    uint32_t cap = n->node_cap ? n->node_cap : 16;
    while (cap < need) cap *= 2;
    JceRoadNode *p = (JceRoadNode *)JCE_REALLOC(n->nodes, cap * sizeof(JceRoadNode));
    if (!p) return false;
    n->nodes = p; n->node_cap = cap; return true;
}
static bool grow_segments(JceRoadNetwork *n, uint32_t need)
{
    if (n->segment_cap >= need) return true;
    uint32_t cap = n->segment_cap ? n->segment_cap : 32;
    while (cap < need) cap *= 2;
    JceRoadSegment *p = (JceRoadSegment *)JCE_REALLOC(n->segments, cap * sizeof(JceRoadSegment));
    if (!p) return false;
    n->segments = p; n->segment_cap = cap; return true;
}
static bool grow_points(JceRoadNetwork *n, uint32_t need)
{
    if (n->point_cap >= need) return true;
    uint32_t cap = n->point_cap ? n->point_cap : 128;
    while (cap < need) cap *= 2;
    jce_vec3 *p = (jce_vec3 *)JCE_REALLOC(n->points, cap * sizeof(jce_vec3));
    if (!p) return false;
    n->points = p; n->point_cap = cap; return true;
}

JceRoadNetwork *jce_road_network_create(const JceRoadNetworkDesc *desc)
{
    JceRoadNetwork *n = (JceRoadNetwork *)JCE_CALLOC(1, sizeof(*n));
    if (!n) return NULL;
    if (desc) {
        if (desc->initial_node_capacity)    grow_nodes(n,    desc->initial_node_capacity);
        if (desc->initial_segment_capacity) grow_segments(n, desc->initial_segment_capacity);
        if (desc->initial_point_capacity)   grow_points(n,   desc->initial_point_capacity);
    }
    return n;
}

void jce_road_network_destroy(JceRoadNetwork *n)
{
    if (!n) return;
    JCE_FREE(n->nodes);
    JCE_FREE(n->segments);
    JCE_FREE(n->points);
    JCE_FREE(n->adjacency);
    JCE_FREE(n);
}

void jce_road_network_clear(JceRoadNetwork *n)
{
    if (!n) return;
    n->node_count = 0;
    n->segment_count = 0;
    n->point_count = 0;
    JCE_FREE(n->adjacency);
    n->adjacency = NULL;
    n->adjacency_count = 0;
    n->finalized = false;
}

uint32_t jce_road_network_add_node(JceRoadNetwork *n, jce_vec3 pos)
{
    if (!n) return JCE_ROAD_INVALID_ID;
    if (!grow_nodes(n, n->node_count + 1)) return JCE_ROAD_INVALID_ID;
    JceRoadNode *node = &n->nodes[n->node_count];
    node->position      = pos;
    node->first_segment = JCE_ROAD_INVALID_ID;
    node->segment_count = 0;
    n->finalized = false;
    return n->node_count++;
}

static float v3_dist(jce_vec3 a, jce_vec3 b)
{
    float dx = a.x-b.x, dy = a.y-b.y, dz = a.z-b.z;
    return sqrtf(dx*dx + dy*dy + dz*dz);
}

uint32_t jce_road_network_add_segment(JceRoadNetwork *n,
                                      uint32_t node_a, uint32_t node_b,
                                      const jce_vec3 *points, uint32_t point_count,
                                      uint8_t lanes_ab, uint8_t lanes_ba,
                                      JceRoadClass cls, float speed_limit)
{
    if (!n || !points || point_count < 2) return JCE_ROAD_INVALID_ID;
    if (node_a >= n->node_count || node_b >= n->node_count) return JCE_ROAD_INVALID_ID;
    if (!grow_segments(n, n->segment_count + 1)) return JCE_ROAD_INVALID_ID;
    if (!grow_points(n, n->point_count + point_count)) return JCE_ROAD_INVALID_ID;

    uint32_t seg_id = n->segment_count++;
    JceRoadSegment *seg = &n->segments[seg_id];
    seg->node_a       = node_a;
    seg->node_b       = node_b;
    seg->lanes_ab     = lanes_ab;
    seg->lanes_ba     = lanes_ba;
    seg->road_class   = cls;
    seg->speed_limit  = speed_limit;
    seg->point_offset = n->point_count;
    seg->point_count  = point_count;
    memcpy(&n->points[n->point_count], points, point_count * sizeof(jce_vec3));
    n->point_count += point_count;

    float len = 0.0f;
    for (uint32_t i = 1; i < point_count; i++)
        len += v3_dist(points[i-1], points[i]);
    seg->length = len;

    n->finalized = false;
    return seg_id;
}

void jce_road_network_finalize(JceRoadNetwork *n)
{
    if (!n) return;
    /* Build per-node adjacency. Two passes: count, then fill. */
    JCE_FREE(n->adjacency); n->adjacency = NULL; n->adjacency_count = 0;

    for (uint32_t i = 0; i < n->node_count; i++) {
        n->nodes[i].first_segment = JCE_ROAD_INVALID_ID;
        n->nodes[i].segment_count = 0;
    }
    /* Count segments per node. */
    for (uint32_t i = 0; i < n->segment_count; i++) {
        n->nodes[n->segments[i].node_a].segment_count++;
        n->nodes[n->segments[i].node_b].segment_count++;
    }
    /* Allocate flat adjacency. */
    uint32_t total = 0;
    for (uint32_t i = 0; i < n->node_count; i++) total += n->nodes[i].segment_count;
    if (total > 0) {
        n->adjacency = (uint32_t *)JCE_MALLOC(total * sizeof(uint32_t));
        if (!n->adjacency) { LOG_ERROR(LOG_TAG, "adjacency alloc failed"); return; }
        n->adjacency_count = total;
    }
    /* Assign offsets, reset counts to act as cursors. */
    uint32_t cursor = 0;
    for (uint32_t i = 0; i < n->node_count; i++) {
        n->nodes[i].first_segment = cursor;
        cursor += n->nodes[i].segment_count;
        n->nodes[i].segment_count = 0;
    }
    /* Fill. */
    for (uint32_t i = 0; i < n->segment_count; i++) {
        JceRoadNode *na = &n->nodes[n->segments[i].node_a];
        JceRoadNode *nb = &n->nodes[n->segments[i].node_b];
        n->adjacency[na->first_segment + na->segment_count++] = i;
        n->adjacency[nb->first_segment + nb->segment_count++] = i;
    }
    n->finalized = true;
    LOG_INFO(LOG_TAG, "finalized: %u nodes, %u segments, %u points",
             n->node_count, n->segment_count, n->point_count);
}

uint32_t jce_road_network_node_count(const JceRoadNetwork *n)    { return n ? n->node_count    : 0; }
uint32_t jce_road_network_segment_count(const JceRoadNetwork *n) { return n ? n->segment_count : 0; }

const JceRoadSegment *jce_road_network_get_segment(const JceRoadNetwork *n, uint32_t id)
{
    if (!n || id >= n->segment_count) return NULL;
    return &n->segments[id];
}
const JceRoadNode *jce_road_network_get_node(const JceRoadNetwork *n, uint32_t id)
{
    if (!n || id >= n->node_count) return NULL;
    return &n->nodes[id];
}
const jce_vec3 *jce_road_network_get_points(const JceRoadNetwork *n, uint32_t seg_id, uint32_t *out_count)
{
    if (!n || seg_id >= n->segment_count) { if (out_count) *out_count = 0; return NULL; }
    const JceRoadSegment *s = &n->segments[seg_id];
    if (out_count) *out_count = s->point_count;
    return &n->points[s->point_offset];
}

bool jce_road_network_sample_segment(const JceRoadNetwork *n, uint32_t seg_id,
                                      float t, jce_vec3 *out_pos, jce_vec3 *out_fwd)
{
    if (!n || seg_id >= n->segment_count) return false;
    const JceRoadSegment *s = &n->segments[seg_id];
    if (s->point_count < 2 || s->length <= 0.0f) return false;

    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    float target = t * s->length;
    const jce_vec3 *pts = &n->points[s->point_offset];

    float accum = 0.0f;
    for (uint32_t i = 1; i < s->point_count; i++) {
        float seg_len = v3_dist(pts[i-1], pts[i]);
        if (accum + seg_len >= target || i == s->point_count - 1) {
            float local_t = seg_len > 0.0f ? (target - accum) / seg_len : 0.0f;
            if (local_t > 1.0f) local_t = 1.0f;
            if (out_pos) {
                out_pos->x = pts[i-1].x + (pts[i].x - pts[i-1].x) * local_t;
                out_pos->y = pts[i-1].y + (pts[i].y - pts[i-1].y) * local_t;
                out_pos->z = pts[i-1].z + (pts[i].z - pts[i-1].z) * local_t;
            }
            if (out_fwd) {
                jce_vec3 d = jce_v3(pts[i].x - pts[i-1].x,
                                    pts[i].y - pts[i-1].y,
                                    pts[i].z - pts[i-1].z);
                float l = sqrtf(d.x*d.x + d.y*d.y + d.z*d.z);
                if (l > 1e-5f) { d.x/=l; d.y/=l; d.z/=l; }
                else           { d = jce_v3(1.0f, 0.0f, 0.0f); }
                *out_fwd = d;
            }
            return true;
        }
        accum += seg_len;
    }
    return false;
}

static float point_seg_dist2(jce_vec3 p, jce_vec3 a, jce_vec3 b, float *out_t)
{
    jce_vec3 ab = jce_v3(b.x-a.x, b.y-a.y, b.z-a.z);
    jce_vec3 ap = jce_v3(p.x-a.x, p.y-a.y, p.z-a.z);
    float ab2 = ab.x*ab.x + ab.y*ab.y + ab.z*ab.z;
    float t = ab2 > 1e-8f ? (ap.x*ab.x + ap.y*ab.y + ap.z*ab.z) / ab2 : 0.0f;
    if (t < 0.0f) t = 0.0f; if (t > 1.0f) t = 1.0f;
    if (out_t) *out_t = t;
    float dx = a.x + ab.x*t - p.x;
    float dy = a.y + ab.y*t - p.y;
    float dz = a.z + ab.z*t - p.z;
    return dx*dx + dy*dy + dz*dz;
}

uint32_t jce_road_network_closest_segment(const JceRoadNetwork *n, jce_vec3 pos,
                                           float *out_t, float *out_dist)
{
    if (!n || n->segment_count == 0) return JCE_ROAD_INVALID_ID;
    uint32_t best = JCE_ROAD_INVALID_ID;
    float best_d2 = INFINITY;
    float best_t  = 0.0f;
    for (uint32_t i = 0; i < n->segment_count; i++) {
        const JceRoadSegment *s = &n->segments[i];
        const jce_vec3 *pts = &n->points[s->point_offset];
        float accum_len = 0.0f;
        for (uint32_t j = 1; j < s->point_count; j++) {
            float local_t;
            float d2 = point_seg_dist2(pos, pts[j-1], pts[j], &local_t);
            if (d2 < best_d2) {
                best_d2 = d2;
                best = i;
                float seg_len = v3_dist(pts[j-1], pts[j]);
                best_t = s->length > 0.0f ? (accum_len + seg_len * local_t) / s->length : 0.0f;
            }
            accum_len += v3_dist(pts[j-1], pts[j]);
        }
    }
    if (out_t)    *out_t    = best_t;
    if (out_dist) *out_dist = sqrtf(best_d2);
    return best;
}

static uint32_t rng_next(uint64_t *s)
{
    uint64_t x = *s ? *s : 0x9E3779B97F4A7C15ULL;
    x ^= x << 13; x ^= x >> 7; x ^= x << 17;
    *s = x;
    return (uint32_t)(x & 0xFFFFFFFFu);
}

uint32_t jce_road_network_pick_random_near(const JceRoadNetwork *n, jce_vec3 pos,
                                            float radius, uint64_t *rng_state)
{
    if (!n || n->segment_count == 0 || !rng_state) return JCE_ROAD_INVALID_ID;
    float r2 = radius * radius;
    /* Reservoir sample over segments whose midpoint lies within radius. */
    uint32_t chosen = JCE_ROAD_INVALID_ID;
    uint32_t k = 0;
    for (uint32_t i = 0; i < n->segment_count; i++) {
        const JceRoadSegment *s = &n->segments[i];
        const jce_vec3 *pts = &n->points[s->point_offset];
        jce_vec3 mid = pts[s->point_count / 2];
        float dx = mid.x - pos.x, dy = mid.y - pos.y, dz = mid.z - pos.z;
        if (dx*dx + dy*dy + dz*dz > r2) continue;
        k++;
        if ((rng_next(rng_state) % k) == 0) chosen = i;
    }
    return chosen;
}
