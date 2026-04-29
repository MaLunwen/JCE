/*
 * jce_space_partition.c  Spatial partitioning — uniform-grid backend.
 *
 * Strategy
 *   - 3-D fixed-resolution grid (default 64×64×64 cells inside the
 *     configured world bounds; degenerate axes collapse to 1).
 *   - Each cell holds a small dynamic list of object handles.
 *   - Objects are rasterised across every cell their AABB overlaps,
 *     so a query needs no per-cell linked traversal.
 *   - Queries deduplicate visited objects with a per-call epoch stamp
 *     so an object touching N cells is reported once.
 *   - JCE_SPACE_BVH and JCE_SPACE_OCTREE silently fall back to the
 *     grid for now (logged once, behaviour identical).
 *
 * Layer: Scene (Layer 5).
 */

#include <jce/middleware/scene/jce_space_partition.h>

#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <string.h>

#define LOG_TAG "space_partition"

#define JCE_GRID_DEFAULT_CELLS   32u   /* cells per axis (32^3 = 32k cells) */
#define JCE_GRID_INITIAL_OBJ_CAP 256u
#define JCE_GRID_INITIAL_CELL    4u    /* per-cell list capacity */

/* ================================================================== */
/* Internal types                                                      */
/* ================================================================== */

typedef struct {
    JceAABB  bounds;
    uint32_t user_id;
    uint32_t epoch;     /* last query epoch that touched this slot */
    int32_t  cmin[3];   /* cached cell range, -1 in [0] = unused slot */
    int32_t  cmax[3];
} JceSpaceObj;

typedef struct {
    uint32_t *items;
    uint32_t  count;
    uint32_t  cap;
} JceCell;

struct JceSpaceIndex {
    JceSpaceType  type;
    JceAABB       world;
    jce_vec3      inv_extent;     /* 1.0 / (max - min) per axis */
    int32_t       res[3];         /* cells per axis */
    uint32_t      cell_count;     /* res[0]*res[1]*res[2] */

    JceCell      *cells;

    JceSpaceObj  *objs;
    uint32_t      obj_cap;
    uint32_t      obj_count;      /* live objects */

    uint32_t     *free_list;
    uint32_t      free_count;
    uint32_t      free_cap;

    uint32_t      query_epoch;
};

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static inline float jce__clampf(float v, float a, float b)
{
    return v < a ? a : (v > b ? b : v);
}

static inline int32_t jce__clampi(int32_t v, int32_t a, int32_t b)
{
    return v < a ? a : (v > b ? b : v);
}

static inline uint32_t jce__cell_index(const JceSpaceIndex *g,
                                        int32_t x, int32_t y, int32_t z)
{
    return (uint32_t)(((z * g->res[1]) + y) * g->res[0] + x);
}

/* Project a world-space point onto integer cell coordinates (clamped). */
static void jce__cell_for_point(const JceSpaceIndex *g, jce_vec3 p,
                                 int32_t out[3])
{
    const float fx = (p.x - g->world.min.x) * g->inv_extent.x;
    const float fy = (p.y - g->world.min.y) * g->inv_extent.y;
    const float fz = (p.z - g->world.min.z) * g->inv_extent.z;
    out[0] = jce__clampi((int32_t)floorf(fx * (float)g->res[0]), 0, g->res[0] - 1);
    out[1] = jce__clampi((int32_t)floorf(fy * (float)g->res[1]), 0, g->res[1] - 1);
    out[2] = jce__clampi((int32_t)floorf(fz * (float)g->res[2]), 0, g->res[2] - 1);
}

static void jce__cell_range_for_aabb(const JceSpaceIndex *g, JceAABB b,
                                      int32_t mn[3], int32_t mx[3])
{
    jce__cell_for_point(g, b.min, mn);
    jce__cell_for_point(g, b.max, mx);
}

/* Append a handle to a cell's item list. */
static void jce__cell_push(JceCell *c, uint32_t handle)
{
    if (c->count == c->cap) {
        const uint32_t new_cap = c->cap ? c->cap * 2u : JCE_GRID_INITIAL_CELL;
        c->items = (uint32_t *)JCE_REALLOC(c->items, new_cap * sizeof(uint32_t));
        c->cap   = new_cap;
    }
    c->items[c->count++] = handle;
}

/* Swap-pop a handle from a cell. */
static void jce__cell_remove(JceCell *c, uint32_t handle)
{
    for (uint32_t i = 0; i < c->count; i++) {
        if (c->items[i] == handle) {
            c->items[i] = c->items[--c->count];
            return;
        }
    }
}

static void jce__add_to_cells(JceSpaceIndex *g, uint32_t handle,
                                const int32_t mn[3], const int32_t mx[3])
{
    for (int32_t z = mn[2]; z <= mx[2]; z++)
    for (int32_t y = mn[1]; y <= mx[1]; y++)
    for (int32_t x = mn[0]; x <= mx[0]; x++) {
        jce__cell_push(&g->cells[jce__cell_index(g, x, y, z)], handle);
    }
}

static void jce__remove_from_cells(JceSpaceIndex *g, uint32_t handle,
                                    const int32_t mn[3], const int32_t mx[3])
{
    for (int32_t z = mn[2]; z <= mx[2]; z++)
    for (int32_t y = mn[1]; y <= mx[1]; y++)
    for (int32_t x = mn[0]; x <= mx[0]; x++) {
        jce__cell_remove(&g->cells[jce__cell_index(g, x, y, z)], handle);
    }
}

static bool jce__aabb_overlap(JceAABB a, JceAABB b)
{
    return !(a.max.x < b.min.x || a.min.x > b.max.x ||
             a.max.y < b.min.y || a.min.y > b.max.y ||
             a.max.z < b.min.z || a.min.z > b.max.z);
}

/* Plane: dot(plane.xyz, p) + plane.w >= 0 means inside the half-space.
 * Test by evaluating the AABB's "p-vertex" (most positive towards normal). */
static bool jce__aabb_inside_frustum(JceAABB b, const jce_vec4 planes[6])
{
    for (int i = 0; i < 6; i++) {
        const jce_vec4 p = planes[i];
        const float px = (p.x >= 0.0f) ? b.max.x : b.min.x;
        const float py = (p.y >= 0.0f) ? b.max.y : b.min.y;
        const float pz = (p.z >= 0.0f) ? b.max.z : b.min.z;
        if (p.x * px + p.y * py + p.z * pz + p.w < 0.0f) return false;
    }
    return true;
}

static bool jce__sphere_aabb(jce_vec3 c, float r, JceAABB b)
{
    float d = 0.0f;
    if (c.x < b.min.x) { float t = b.min.x - c.x; d += t * t; }
    else if (c.x > b.max.x) { float t = c.x - b.max.x; d += t * t; }
    if (c.y < b.min.y) { float t = b.min.y - c.y; d += t * t; }
    else if (c.y > b.max.y) { float t = c.y - b.max.y; d += t * t; }
    if (c.z < b.min.z) { float t = b.min.z - c.z; d += t * t; }
    else if (c.z > b.max.z) { float t = c.z - b.max.z; d += t * t; }
    return d <= r * r;
}

/* Slab ray-AABB; returns hit distance in [0, max_dist] or -1 on miss.
 * Hits behind origin (entering tmin < 0) clamp to 0. */
static float jce__ray_aabb(jce_vec3 o, jce_vec3 d, float max_dist, JceAABB b)
{
    float tmin = 0.0f, tmax = max_dist;
    for (int axis = 0; axis < 3; axis++) {
        const float oo = (&o.x)[axis];
        const float dd = (&d.x)[axis];
        const float bmin = (&b.min.x)[axis];
        const float bmax = (&b.max.x)[axis];
        if (fabsf(dd) < 1e-8f) {
            if (oo < bmin || oo > bmax) return -1.0f;
        } else {
            const float inv = 1.0f / dd;
            float t1 = (bmin - oo) * inv;
            float t2 = (bmax - oo) * inv;
            if (t1 > t2) { float tmp = t1; t1 = t2; t2 = tmp; }
            if (t1 > tmin) tmin = t1;
            if (t2 < tmax) tmax = t2;
            if (tmin > tmax) return -1.0f;
        }
    }
    return tmin;
}

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JceSpaceIndex *jce_space_create(const JceSpaceConfig *config)
{
    if (!config) return NULL;

    JceSpaceIndex *g = JCE_NEW(JceSpaceIndex);
    if (!g) return NULL;

    g->type  = config->type;
    g->world = config->world_bounds;

    if (config->type != JCE_SPACE_GRID) {
        LOG_INFO(LOG_TAG, "%s requested; using uniform grid backend",
                 config->type == JCE_SPACE_BVH ? "BVH" : "octree");
    }

    /* Default world if caller passed all-zero. */
    const jce_vec3 ext = {
        g->world.max.x - g->world.min.x,
        g->world.max.y - g->world.min.y,
        g->world.max.z - g->world.min.z,
    };
    if (ext.x <= 0.0f || ext.y <= 0.0f || ext.z <= 0.0f) {
        g->world.min.x = -1024.0f; g->world.max.x = 1024.0f;
        g->world.min.y = -1024.0f; g->world.max.y = 1024.0f;
        g->world.min.z = -1024.0f; g->world.max.z = 1024.0f;
    }

    g->inv_extent.x = 1.0f / (g->world.max.x - g->world.min.x);
    g->inv_extent.y = 1.0f / (g->world.max.y - g->world.min.y);
    g->inv_extent.z = 1.0f / (g->world.max.z - g->world.min.z);

    g->res[0] = g->res[1] = g->res[2] = (int32_t)JCE_GRID_DEFAULT_CELLS;
    g->cell_count = (uint32_t)g->res[0] * (uint32_t)g->res[1] * (uint32_t)g->res[2];
    g->cells = (JceCell *)JCE_CALLOC(g->cell_count, sizeof(JceCell));

    const uint32_t hint = config->max_objects ? config->max_objects
                                              : JCE_GRID_INITIAL_OBJ_CAP;
    g->obj_cap   = hint;
    g->objs      = (JceSpaceObj *)JCE_CALLOC(g->obj_cap, sizeof(JceSpaceObj));
    g->free_cap  = 32;
    g->free_list = (uint32_t *)JCE_CALLOC(g->free_cap, sizeof(uint32_t));

    /* Mark every slot unused. */
    for (uint32_t i = 0; i < g->obj_cap; i++) g->objs[i].cmin[0] = -1;

    LOG_INFO(LOG_TAG, "uniform grid: %dx%dx%d cells, world [%.1f..%.1f]^3, hint=%u objs",
             g->res[0], g->res[1], g->res[2],
             g->world.min.x, g->world.max.x, hint);
    return g;
}

void jce_space_destroy(JceSpaceIndex *g)
{
    if (!g) return;
    if (g->cells) {
        for (uint32_t i = 0; i < g->cell_count; i++) JCE_FREE(g->cells[i].items);
        JCE_FREE(g->cells);
    }
    JCE_FREE(g->objs);
    JCE_FREE(g->free_list);
    JCE_FREE(g);
}

void jce_space_reset(JceSpaceIndex *g, const JceAABB *new_bounds)
{
    if (!g) return;

    /* Drop every object: zero each cell's count (keep its capacity), wipe
     * the free-list, and mark every slot unused. */
    if (g->cells) {
        for (uint32_t i = 0; i < g->cell_count; i++) g->cells[i].count = 0;
    }
    for (uint32_t i = 0; i < g->obj_cap; i++) g->objs[i].cmin[0] = -1;
    g->obj_count   = 0;
    g->free_count  = 0;
    g->query_epoch = 0;

    if (new_bounds) {
        JceAABB nb = *new_bounds;
        const jce_vec3 ext = {
            nb.max.x - nb.min.x,
            nb.max.y - nb.min.y,
            nb.max.z - nb.min.z,
        };
        if (ext.x <= 0.0f || ext.y <= 0.0f || ext.z <= 0.0f) {
            nb.min.x = -1024.0f; nb.max.x = 1024.0f;
            nb.min.y = -1024.0f; nb.max.y = 1024.0f;
            nb.min.z = -1024.0f; nb.max.z = 1024.0f;
        }
        g->world         = nb;
        g->inv_extent.x  = 1.0f / (nb.max.x - nb.min.x);
        g->inv_extent.y  = 1.0f / (nb.max.y - nb.min.y);
        g->inv_extent.z  = 1.0f / (nb.max.z - nb.min.z);
    }
}

/* ================================================================== */
/* Object management                                                   */
/* ================================================================== */

static uint32_t jce__alloc_slot(JceSpaceIndex *g)
{
    if (g->free_count) return g->free_list[--g->free_count];
    if (g->obj_count == g->obj_cap) {
        const uint32_t new_cap = g->obj_cap ? g->obj_cap * 2u : 64u;
        g->objs = (JceSpaceObj *)JCE_REALLOC(g->objs, new_cap * sizeof(JceSpaceObj));
        for (uint32_t i = g->obj_cap; i < new_cap; i++) {
            memset(&g->objs[i], 0, sizeof(JceSpaceObj));
            g->objs[i].cmin[0] = -1;
        }
        g->obj_cap = new_cap;
    }
    return g->obj_count++;
}

uint32_t jce_space_insert(JceSpaceIndex *g, JceAABB bounds, uint32_t user_id)
{
    if (!g) return 0;
    const uint32_t slot = jce__alloc_slot(g);

    int32_t mn[3], mx[3];
    jce__cell_range_for_aabb(g, bounds, mn, mx);

    JceSpaceObj *o = &g->objs[slot];
    o->bounds  = bounds;
    o->user_id = user_id;
    o->epoch   = 0;
    memcpy(o->cmin, mn, sizeof(mn));
    memcpy(o->cmax, mx, sizeof(mx));

    jce__add_to_cells(g, slot, mn, mx);
    return slot + 1u;   /* 0 is reserved as "invalid" */
}

void jce_space_update(JceSpaceIndex *g, uint32_t handle, JceAABB new_bounds)
{
    if (!g || handle == 0 || handle > g->obj_cap) return;
    JceSpaceObj *o = &g->objs[handle - 1u];
    if (o->cmin[0] < 0) return;

    int32_t mn[3], mx[3];
    jce__cell_range_for_aabb(g, new_bounds, mn, mx);

    if (o->cmin[0] != mn[0] || o->cmin[1] != mn[1] || o->cmin[2] != mn[2] ||
        o->cmax[0] != mx[0] || o->cmax[1] != mx[1] || o->cmax[2] != mx[2]) {
        jce__remove_from_cells(g, handle - 1u, o->cmin, o->cmax);
        memcpy(o->cmin, mn, sizeof(mn));
        memcpy(o->cmax, mx, sizeof(mx));
        jce__add_to_cells(g, handle - 1u, mn, mx);
    }
    o->bounds = new_bounds;
}

void jce_space_remove(JceSpaceIndex *g, uint32_t handle)
{
    if (!g || handle == 0 || handle > g->obj_cap) return;
    const uint32_t slot = handle - 1u;
    JceSpaceObj *o = &g->objs[slot];
    if (o->cmin[0] < 0) return;

    jce__remove_from_cells(g, slot, o->cmin, o->cmax);
    o->cmin[0] = -1;

    if (g->free_count == g->free_cap) {
        const uint32_t new_cap = g->free_cap * 2u;
        g->free_list = (uint32_t *)JCE_REALLOC(g->free_list,
                                               new_cap * sizeof(uint32_t));
        g->free_cap  = new_cap;
    }
    g->free_list[g->free_count++] = slot;
}

/* ================================================================== */
/* Queries                                                             */
/* ================================================================== */

/* Internal driver: walk every cell in [mn..mx], dedupe by epoch,
 * and call `accept` on each candidate's AABB. */
typedef bool (*jce__accept_fn)(const JceSpaceObj *o, void *ctx);

static uint32_t jce__cell_query(JceSpaceIndex *g,
                                 const int32_t mn[3], const int32_t mx[3],
                                 jce__accept_fn accept, void *ctx,
                                 uint32_t *out, uint32_t max)
{
    const uint32_t epoch = ++g->query_epoch;
    uint32_t found = 0;

    for (int32_t z = mn[2]; z <= mx[2]; z++)
    for (int32_t y = mn[1]; y <= mx[1]; y++)
    for (int32_t x = mn[0]; x <= mx[0]; x++) {
        const JceCell *c = &g->cells[jce__cell_index(g, x, y, z)];
        for (uint32_t i = 0; i < c->count; i++) {
            const uint32_t slot = c->items[i];
            JceSpaceObj *o = &g->objs[slot];
            if (o->epoch == epoch) continue;
            o->epoch = epoch;
            if (!accept(o, ctx)) continue;
            if (out && found < max) out[found] = o->user_id;
            found++;
            if (out && found >= max) return found;
        }
    }
    return found;
}

/* --- Frustum query (cell-coarse rejection inlined below) --- */

uint32_t jce_space_query_frustum(const JceSpaceIndex *idx,
                                  const jce_vec4 planes[6],
                                  uint32_t *out_ids, uint32_t max)
{
    if (!idx) return 0;
    JceSpaceIndex *g = (JceSpaceIndex *)idx;

    const float cell_dx = (g->world.max.x - g->world.min.x) / (float)g->res[0];
    const float cell_dy = (g->world.max.y - g->world.min.y) / (float)g->res[1];
    const float cell_dz = (g->world.max.z - g->world.min.z) / (float)g->res[2];

    const uint32_t epoch = ++g->query_epoch;
    uint32_t found = 0;

    for (int32_t z = 0; z < g->res[2]; z++)
    for (int32_t y = 0; y < g->res[1]; y++)
    for (int32_t x = 0; x < g->res[0]; x++) {
        /* Cell-coarse reject: skip the whole cell if its AABB sits
         * outside any frustum plane. */
        JceAABB cell_aabb;
        cell_aabb.min.x = g->world.min.x + (float)x * cell_dx;
        cell_aabb.min.y = g->world.min.y + (float)y * cell_dy;
        cell_aabb.min.z = g->world.min.z + (float)z * cell_dz;
        cell_aabb.max.x = cell_aabb.min.x + cell_dx;
        cell_aabb.max.y = cell_aabb.min.y + cell_dy;
        cell_aabb.max.z = cell_aabb.min.z + cell_dz;
        if (!jce__aabb_inside_frustum(cell_aabb, planes)) continue;

        const JceCell *c = &g->cells[jce__cell_index(g, x, y, z)];
        for (uint32_t i = 0; i < c->count; i++) {
            const uint32_t slot = c->items[i];
            JceSpaceObj *o = &g->objs[slot];
            if (o->epoch == epoch) continue;
            o->epoch = epoch;
            if (!jce__aabb_inside_frustum(o->bounds, planes)) continue;
            if (out_ids && found < max) out_ids[found] = o->user_id;
            found++;
            if (out_ids && found >= max) return found;
        }
    }
    return found;
}

/* --- AABB query --- */
typedef struct { JceAABB region; } jce__AabbCtx;

static bool jce__aabb_accept(const JceSpaceObj *o, void *vctx)
{
    const jce__AabbCtx *c = (const jce__AabbCtx *)vctx;
    return jce__aabb_overlap(o->bounds, c->region);
}

uint32_t jce_space_query_aabb(const JceSpaceIndex *idx, JceAABB region,
                               uint32_t *out_ids, uint32_t max)
{
    if (!idx) return 0;
    JceSpaceIndex *g = (JceSpaceIndex *)idx;
    int32_t mn[3], mx[3];
    jce__cell_range_for_aabb(g, region, mn, mx);
    jce__AabbCtx ctx = { region };
    return jce__cell_query(g, mn, mx, jce__aabb_accept, &ctx, out_ids, max);
}

/* --- Sphere query --- */
typedef struct { jce_vec3 c; float r; } jce__SphereCtx;

static bool jce__sphere_accept(const JceSpaceObj *o, void *vctx)
{
    const jce__SphereCtx *c = (const jce__SphereCtx *)vctx;
    return jce__sphere_aabb(c->c, c->r, o->bounds);
}

uint32_t jce_space_query_sphere(const JceSpaceIndex *idx,
                                 jce_vec3 center, float radius,
                                 uint32_t *out_ids, uint32_t max)
{
    if (!idx) return 0;
    JceSpaceIndex *g = (JceSpaceIndex *)idx;
    JceAABB region;
    region.min.x = center.x - radius; region.max.x = center.x + radius;
    region.min.y = center.y - radius; region.max.y = center.y + radius;
    region.min.z = center.z - radius; region.max.z = center.z + radius;
    int32_t mn[3], mx[3];
    jce__cell_range_for_aabb(g, region, mn, mx);
    jce__SphereCtx ctx = { center, radius };
    return jce__cell_query(g, mn, mx, jce__sphere_accept, &ctx, out_ids, max);
}

/* --- Raycast (closest hit) --- */
bool jce_space_raycast(const JceSpaceIndex *idx, jce_vec3 origin,
                        jce_vec3 direction, float max_dist,
                        JceSpaceRayHit *out_hit)
{
    if (!idx || max_dist <= 0.0f) return false;
    JceSpaceIndex *g = (JceSpaceIndex *)idx;

    /* Brute scan over all cells the ray's bounding aabb touches.
     * A 3D-DDA is left for a follow-up; current loads are tiny. */
    JceAABB ray_aabb;
    {
        const jce_vec3 a = origin;
        const jce_vec3 b = {
            origin.x + direction.x * max_dist,
            origin.y + direction.y * max_dist,
            origin.z + direction.z * max_dist,
        };
        ray_aabb.min.x = a.x < b.x ? a.x : b.x;
        ray_aabb.min.y = a.y < b.y ? a.y : b.y;
        ray_aabb.min.z = a.z < b.z ? a.z : b.z;
        ray_aabb.max.x = a.x > b.x ? a.x : b.x;
        ray_aabb.max.y = a.y > b.y ? a.y : b.y;
        ray_aabb.max.z = a.z > b.z ? a.z : b.z;
    }
    int32_t mn[3], mx[3];
    jce__cell_range_for_aabb(g, ray_aabb, mn, mx);

    const uint32_t epoch = ++g->query_epoch;
    float best_t = max_dist;
    uint32_t best_id = UINT32_MAX;

    for (int32_t z = mn[2]; z <= mx[2]; z++)
    for (int32_t y = mn[1]; y <= mx[1]; y++)
    for (int32_t x = mn[0]; x <= mx[0]; x++) {
        const JceCell *c = &g->cells[jce__cell_index(g, x, y, z)];
        for (uint32_t i = 0; i < c->count; i++) {
            const uint32_t slot = c->items[i];
            JceSpaceObj *o = &g->objs[slot];
            if (o->epoch == epoch) continue;
            o->epoch = epoch;
            const float t = jce__ray_aabb(origin, direction, best_t, o->bounds);
            if (t >= 0.0f && t < best_t) {
                best_t  = t;
                best_id = o->user_id;
            }
        }
    }

    if (best_id == UINT32_MAX) return false;
    if (out_hit) {
        out_hit->user_id  = best_id;
        out_hit->distance = best_t;
        out_hit->point.x  = origin.x + direction.x * best_t;
        out_hit->point.y  = origin.y + direction.y * best_t;
        out_hit->point.z  = origin.z + direction.z * best_t;
    }
    return true;
}

/* ================================================================== */
/* Stats                                                               */
/* ================================================================== */

uint32_t jce_space_object_count(const JceSpaceIndex *g)
{
    if (!g) return 0;
    return g->obj_count - g->free_count;
}
