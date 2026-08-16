/*
 * jce_space_partition.c  Spatial partitioning — uniform-grid backend.
 *
 * Strategy
 *   - 3-D uniform grid whose per-axis resolution is derived from the
 *     configured world bounds extent / a target cell size (~24 m),
 *     clamped to [1, JCE_GRID_MAX_CELLS]; degenerate axes collapse to 1.
 *     A small world allocates few cells; a multi-km world gets a fine
 *     grid so dense regions don't collapse into a handful of cells.
 *   - Each cell holds a small dynamic list of object handles.
 *   - Objects are rasterised across every cell their AABB overlaps,
 *     so a query needs no per-cell linked traversal.
 *   - A persistent list of OCCUPIED cell indices is maintained so a full
 *     scan (frustum query) visits only non-empty cells instead of res³.
 *   - Queries deduplicate visited objects with a per-call epoch stamp
 *     so an object touching N cells is reported once.
 *   - The broad-phase is PERSISTENT: an object is inserted once and kept
 *     across frames; jce_space_update re-buckets only when its cell range
 *     actually changes (static objects then cost ~0/frame), and
 *     jce_space_remove drops it on despawn.  user_id may be refreshed
 *     in place (jce_space_set_user_id) without touching the cell lists.
 *   - JCE_SPACE_BVH and JCE_SPACE_OCTREE silently fall back to the
 *     grid for now (logged once, behaviour identical).
 *
 * Layer: Scene (Layer 5).
 */

#include <jce/middleware/scene/jce_space_partition.h>

#include <jce/os/core/jce_frustum.h>
#include <jce/os/core/jce_log.h>
#include "os/core/jce_memory.h"

#include <float.h>
#include <math.h>
#include <stddef.h>
#include <stdlib.h>
#include <string.h>

/* Non-faulting hint: bring a line toward L1 without stalling if it is absent.
 * Local to this file until a second caller wants it. */
#if defined(_MSC_VER) && (defined(_M_X64) || defined(_M_IX86))
#  include <xmmintrin.h>
#  define JCE_PREFETCH(p) _mm_prefetch((const char *)(p), _MM_HINT_T0)
#elif defined(__GNUC__) || defined(__clang__)
#  define JCE_PREFETCH(p) __builtin_prefetch((const void *)(p), 0, 3)
#else
#  define JCE_PREFETCH(p) ((void)(p))
#endif

#define LOG_TAG "space_partition"

/* Extent-sized resolution: res_axis = clamp(extent_axis / target_cell, 1, MAX).
 * The target cell size trades grid memory for per-cell occupancy: ~24 m keeps a
 * downtown block's worth of objects per cell while a 5 km world still stays
 * under the per-axis cap (5000/24 ≈ 208 < 256).  MIN_CELLS keeps a tiny world
 * from degenerating to a 1×1×1 grid (which would linear-scan every object). */
#define JCE_GRID_TARGET_CELL     24.0f /* world units per cell (per axis)     */
#define JCE_GRID_MIN_CELLS       4u    /* floor per axis (small worlds)       */
#define JCE_GRID_MAX_CELLS       256u  /* ceiling per axis (5 km @ 24 m/cell) */
#define JCE_GRID_MAX_TOTAL_CELLS (1u << 21) /* 2,097,152 — total-cell safety cap */
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
    int32_t   occ_slot;   /* index into occupied[] when count>0, else -1 */
} JceCell;

struct JceSpaceIndex {
    JceSpaceType  type;
    JceAABB       world;
    jce_vec3      inv_extent;     /* 1.0 / (max - min) per axis */
    int32_t       res[3];         /* cells per axis */
    uint32_t      cell_count;     /* res[0]*res[1]*res[2] */

    JceCell      *cells;

    /* Dense list of currently non-empty cell indices, so a full scan visits
     * only occupied cells instead of all res³.  Maintained incrementally: a
     * cell joins on its first push and leaves (swap-pop) on its last remove. */
    uint32_t     *occupied;
    uint32_t      occupied_count;
    uint32_t      occupied_cap;

    JceSpaceObj  *objs;
    uint32_t      obj_cap;
    uint32_t      obj_count;      /* live objects */

    uint32_t     *free_list;
    uint32_t      free_count;
    uint32_t      free_cap;

    uint32_t      query_epoch;

    /* Work counters from the most recent frustum query.  Banked here rather
     * than logged in place: a query runs on whatever thread called it. */
    uint32_t      q_cells_walked;
    uint32_t      q_cells_acc;
    uint32_t      q_objs_visited;
    uint32_t      q_objs_tested;
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

/* Mark a cell occupied (append to the occupied list) on its first item. */
static void jce__cell_mark_occupied(JceSpaceIndex *g, uint32_t cell_idx)
{
    JceCell *c = &g->cells[cell_idx];
    if (c->occ_slot >= 0) return;          /* already listed */
    if (g->occupied_count == g->occupied_cap) {
        const uint32_t new_cap = g->occupied_cap ? g->occupied_cap * 2u : 64u;
        g->occupied = (uint32_t *)JCE_REALLOC(g->occupied,
                                              new_cap * sizeof(uint32_t));
        g->occupied_cap = new_cap;
    }
    c->occ_slot = (int32_t)g->occupied_count;
    g->occupied[g->occupied_count++] = cell_idx;
}

/* Drop a cell from the occupied list (swap-pop) when its last item leaves. */
static void jce__cell_mark_empty(JceSpaceIndex *g, uint32_t cell_idx)
{
    JceCell *c = &g->cells[cell_idx];
    if (c->occ_slot < 0) return;
    const uint32_t slot = (uint32_t)c->occ_slot;
    const uint32_t last = --g->occupied_count;
    if (slot != last) {
        const uint32_t moved = g->occupied[last];
        g->occupied[slot] = moved;
        g->cells[moved].occ_slot = (int32_t)slot;
    }
    c->occ_slot = -1;
}

/* Append a handle to a cell's item list. */
static void jce__cell_push(JceSpaceIndex *g, uint32_t cell_idx, uint32_t handle)
{
    JceCell *c = &g->cells[cell_idx];
    if (c->count == 0) jce__cell_mark_occupied(g, cell_idx);
    if (c->count == c->cap) {
        const uint32_t new_cap = c->cap ? c->cap * 2u : JCE_GRID_INITIAL_CELL;
        c->items = (uint32_t *)JCE_REALLOC(c->items, new_cap * sizeof(uint32_t));
        c->cap   = new_cap;
    }
    c->items[c->count++] = handle;
}

/* Swap-pop a handle from a cell. */
static void jce__cell_remove(JceSpaceIndex *g, uint32_t cell_idx, uint32_t handle)
{
    JceCell *c = &g->cells[cell_idx];
    for (uint32_t i = 0; i < c->count; i++) {
        if (c->items[i] == handle) {
            c->items[i] = c->items[--c->count];
            if (c->count == 0) jce__cell_mark_empty(g, cell_idx);
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
        jce__cell_push(g, jce__cell_index(g, x, y, z), handle);
    }
}

static void jce__remove_from_cells(JceSpaceIndex *g, uint32_t handle,
                                    const int32_t mn[3], const int32_t mx[3])
{
    for (int32_t z = mn[2]; z <= mx[2]; z++)
    for (int32_t y = mn[1]; y <= mx[1]; y++)
    for (int32_t x = mn[0]; x <= mx[0]; x++) {
        jce__cell_remove(g, jce__cell_index(g, x, y, z), handle);
    }
}

static bool jce__aabb_overlap(JceAABB a, JceAABB b)
{
    return !(a.max.x < b.min.x || a.min.x > b.max.x ||
             a.max.y < b.min.y || a.min.y > b.max.y ||
             a.max.z < b.min.z || a.min.z > b.max.z);
}

/* Forwarder onto the shared jce_frustum.h positive-vertex test (one canonical
 * impl); adapts the JceAABB struct to the {min,max} vec3 pair. */
static bool jce__aabb_inside_frustum(JceAABB b, const jce_vec4 planes[6])
{
    return jce_aabb_in_frustum(planes, b.min, b.max);
}

static inline bool jce__aabb_in_frustum_fast(JceAABB b, const jce_vec4 planes[6],
                                             const float absn[6][3])
{
    return jce_aabb_in_frustum_fast(planes, absn, b.min, b.max);
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
/* Resolution sizing                                                   */
/* ================================================================== */

/* Derive the per-axis cell count from the world extent / target cell size,
 * clamped to [MIN, MAX].  Degenerate (<= 0) axes collapse to 1.  A final pass
 * shrinks the largest axes proportionally if the total cell count would blow
 * past JCE_GRID_MAX_TOTAL_CELLS (a thin, very long world could otherwise hit
 * 256×256×N).  Pure function of `world`; called by create + reset. */
static void jce__derive_resolution(const JceAABB *world, int32_t res[3])
{
    const float ext[3] = {
        world->max.x - world->min.x,
        world->max.y - world->min.y,
        world->max.z - world->min.z,
    };
    for (int a = 0; a < 3; a++) {
        if (ext[a] <= 0.0f) { res[a] = 1; continue; }
        int32_t r = (int32_t)(ext[a] / JCE_GRID_TARGET_CELL);
        if (r < (int32_t)JCE_GRID_MIN_CELLS) r = (int32_t)JCE_GRID_MIN_CELLS;
        if (r > (int32_t)JCE_GRID_MAX_CELLS) r = (int32_t)JCE_GRID_MAX_CELLS;
        res[a] = r;
    }
    /* Total-cell safety cap: halve the largest axis until under the ceiling. */
    while ((uint64_t)res[0] * (uint64_t)res[1] * (uint64_t)res[2]
           > (uint64_t)JCE_GRID_MAX_TOTAL_CELLS) {
        int amax = (res[0] >= res[1] && res[0] >= res[2]) ? 0
                 : (res[1] >= res[2] ? 1 : 2);
        if (res[amax] <= 1) break;          /* cannot shrink further */
        res[amax] = (res[amax] + 1) / 2;    /* round up so it never hits 0 */
    }
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

    jce__derive_resolution(&g->world, g->res);
    g->cell_count = (uint32_t)g->res[0] * (uint32_t)g->res[1] * (uint32_t)g->res[2];
    g->cells = (JceCell *)JCE_CALLOC(g->cell_count, sizeof(JceCell));
    /* CALLOC zeroes occ_slot; an empty cell must read -1 ("not listed"). */
    for (uint32_t i = 0; i < g->cell_count; i++) g->cells[i].occ_slot = -1;
    g->occupied      = NULL;
    g->occupied_cap  = 0;
    g->occupied_count = 0;

    const uint32_t hint = config->max_objects ? config->max_objects
                                              : JCE_GRID_INITIAL_OBJ_CAP;
    g->obj_cap   = hint;
    g->objs      = (JceSpaceObj *)JCE_CALLOC(g->obj_cap, sizeof(JceSpaceObj));
    g->free_cap  = 32;
    g->free_list = (uint32_t *)JCE_CALLOC(g->free_cap, sizeof(uint32_t));

    /* Mark every slot unused. */
    for (uint32_t i = 0; i < g->obj_cap; i++) g->objs[i].cmin[0] = -1;

    LOG_INFO(LOG_TAG, "uniform grid: %dx%dx%d (%u) cells, world [%.1f..%.1f]^3, hint=%u objs",
             g->res[0], g->res[1], g->res[2], g->cell_count,
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
    JCE_FREE(g->occupied);
    JCE_FREE(g->objs);
    JCE_FREE(g->free_list);
    JCE_FREE(g);
}

void jce_space_reset(JceSpaceIndex *g, const JceAABB *new_bounds)
{
    if (!g) return;

    /* Drop every object: only the OCCUPIED cells hold items, so zero just those
     * (was: scan all res³ cells every reset — the per-frame rebuild cost the
     * persistent path now avoids; the occupied list keeps even the legacy reset
     * proportional to live occupancy).  Then clear the occupied list, wipe the
     * free-list, and mark every object slot unused. */
    if (g->cells) {
        for (uint32_t i = 0; i < g->occupied_count; i++) {
            JceCell *c = &g->cells[g->occupied[i]];
            c->count    = 0;
            c->occ_slot = -1;
        }
    }
    g->occupied_count = 0;
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

        /* Re-derive the resolution from the new extent; reallocate the cell
         * grid only when the cell count actually changes (a small bounds wobble
         * frame-to-frame keeps the same grid → no realloc churn). */
        int32_t nres[3];
        jce__derive_resolution(&g->world, nres);
        if (nres[0] != g->res[0] || nres[1] != g->res[1] || nres[2] != g->res[2]) {
            const uint32_t ncount = (uint32_t)nres[0] * (uint32_t)nres[1]
                                  * (uint32_t)nres[2];
            JceCell *ncells = (JceCell *)JCE_CALLOC(ncount, sizeof(JceCell));
            if (ncells) {
                /* Free the old per-cell item lists (objects were already
                 * dropped above, so nothing is lost). */
                if (g->cells) {
                    for (uint32_t i = 0; i < g->cell_count; i++)
                        JCE_FREE(g->cells[i].items);
                    JCE_FREE(g->cells);
                }
                for (uint32_t i = 0; i < ncount; i++) ncells[i].occ_slot = -1;
                g->cells      = ncells;
                g->cell_count = ncount;
                g->res[0] = nres[0]; g->res[1] = nres[1]; g->res[2] = nres[2];
            }
            /* On OOM keep the old grid (still correct, just coarser). */
        }
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

void jce_space_set_user_id(JceSpaceIndex *g, uint32_t handle, uint32_t user_id)
{
    if (!g || handle == 0 || handle > g->obj_cap) return;
    JceSpaceObj *o = &g->objs[handle - 1u];
    if (o->cmin[0] < 0) return;            /* freed slot */
    o->user_id = user_id;
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
    const int32_t res0 = g->res[0], res1 = g->res[1];
    const int32_t plane = res0 * res1;

    const uint32_t epoch = ++g->query_epoch;
    uint32_t found = 0;

    /* Hoist |n| once for the whole query: the per-box test then has no sign
     * tests or selects.  Same predicate, by identity -- see jce_frustum.h. */
    float absn[6][3];
    jce_frustum_abs_normals(planes, absn);

    /* A uniform grid only pays when cells outnumber the objects they hold.
     * Large flat geometry breaks that: street_demo registers 832 objects across
     * 69696 occupied cells -- 84 cells per object, because a road or a terrain
     * chunk spans hundreds of them -- so the cell walk ran 69696 frustum tests
     * to filter 832 objects, and that WAS the frame's scene cost (sr_vis 0.97 ms
     * of a 2.04 ms frame that issued 218 draws).
     *
     * The opposite regime is just as real: the 200k bench holds 200833 objects
     * in 14393 occupied cells on a 145x75x145 grid, and the coarse reject keeps
     * only 905 of them (6.3%) -- the grid is worth an order of magnitude there.
     * So this is a per-query decision, not a configuration one.
     *
     * Walking the objects directly is not an approximation: each gets the same
     * per-object test it would get through a cell, and the epoch dedup a
     * multi-cell object needs is moot because each object is visited once.  A
     * cell walk can be TIGHTER -- rejecting a long thin box by its cells
     * resolves a frustum corner finer than any single whole-box test -- so this
     * path may emit a few extra objects.  That is the conservative direction,
     * and the one the broad-phase contract allows. */
    if (g->obj_count > 0 && g->occupied_count >= g->obj_count) {
        for (uint32_t slot = 0; slot < g->obj_count; slot++) {
            JceSpaceObj *o = &g->objs[slot];
            if (o->cmin[0] < 0) continue;              /* freed slot */
            o->epoch = epoch;
            if (!jce__aabb_in_frustum_fast(o->bounds, planes, absn)) continue;
            if (out_ids && found < max) out_ids[found] = o->user_id;
            found++;
            if (out_ids && found >= max) return found;
        }
        return found;
    }

    /* Iterate only the OCCUPIED cells (a dense list maintained on push/remove)
     * instead of all res³ — on a sparse streamed world the empty-space scan is
     * what made a fine grid expensive, and it produced ZERO extra candidates.
     * Same per-cell coarse frustum reject + per-object dedup as before, so the
     * result set is byte-identical to the full scan. */
    /* PARALLELISING this walk was tried too, and it is the most interesting
     * failure of the four. The whole-process sampler (JCE_SAMPLER_ALL=1) shows
     * every worker thread parked at 100% in a wait while the main thread spends
     * ~21% here, so fanning 14,393 independent cells across an idle pool looks
     * free. It is not:
     *
     *     serial    query median 0.827 ms   (0.816-0.854)
     *     parallel  query median 0.909 ms   (0.887-1.008)
     *
     * and the sampler says exactly why. With the parallel path on, the workers
     * DO run jce__space_q_range and enkiTS SplitAndAdd/TryRunTask appear -- and
     * MAIN's 21% in the query becomes 20% in NtWaitForSingleObject. It traded
     * work for waiting. The dispatch and wake of parked workers costs about
     * what the walk costs, because the walk is only ~0.8 ms and the main thread
     * must block on the result before it can do anything else.
     *
     * So the lever is not fan-out. It is PIPELINING -- overlapping the query
     * with main-thread work that does not depend on it -- which is a change to
     * the frame's dependency chain, not to this function. Do not re-try the
     * fan-out without first breaking that dependency.
     *
     * (The implementation itself was sound and is worth rebuilding if that
     * dependency ever breaks: read-only parallel phase over objs[], no epoch
     * writes so no false sharing, slots appended through one atomic cursor in
     * 256-wide batches, then a single serial pass to stamp epochs, dedup and
     * convert slots to user_ids in place. Visible counts matched exactly.)
     */

    /* THREE memory-layout changes have been tried on the loop below and all
     * three measured as nothing. Recorded together because the pattern is the
     * finding, not any one of them:
     *
     *   inline the AABB in the cell item   13.90 vs 13.67 ns/unit   slower
     *   split hot(32B)/cold(24B) records    query 0.872 -> 0.963 median,
     *                                       ranges 0.853-1.161 vs 0.742-0.970
     *
     * The third of them, accepting a fully-inside cell's contents untested, has
     * since SHIPPED -- see the classify below. It measured -3.4% and inside the
     * noise the first time, and this note said why and named the condition to
     * retry under: "a camera INSIDE a dense field would flip that, and nothing
     * here can produce one". JCE_BENCH_CAM can now produce one, and under it:
     *     per-object tests   199,675 -> 66,183   -67%
     *     query median       2.980 -> 1.909 ms   (2.884-2.995 vs 1.882-2.051)
     * Same visible count, 194,667, in all six runs. The rejection was right for
     * the load it was measured on and wrong as a general conclusion, which is
     * the argument for writing the retry condition down rather than just the
     * verdict.
     *
     * All were interleaved A/B, three rounds, identical visible counts. The
     * third is the most instructive: 32-byte records never straddle a cache
     * line where 56-byte ones straddle ~44% of the time, and the working set
     * drops 11.2 -> 6.4 MB, yet the median moved the WRONG way.
     *
     * The reading that fits all three: the prefetch a few lines down already
     * covers the latency this loop would otherwise stall on, so bytes-touched
     * is no longer the binding constraint and shrinking them buys nothing. What
     * would move it is fewer object tests (67688 for 25433 visible) or fewer
     * cells walked -- an algorithmic change, not a layout one. Bring a
     * measurement, not a cache-line argument.
     */

    /* Counters, not timers, and NOT logged from here.
     *
     * Counters because bracketing a loop body this hot with rdtsc costs more
     * than the body -- the per-entity probes in the submit loop inflate it 2.6x
     * (sr_loop 2.12 -> 7.55 ms at the 200k bench), which makes their
     * percentages upper bounds rather than measurements. Counting is free and
     * exact; divide by the phase timer the caller already keeps.
     *
     * Not logged from here because a query runs in whatever context called it,
     * including job threads, and logging from one crashed this engine before.
     * The numbers are banked on the index and printed by the caller. */
    uint32_t n_cell_acc = 0, n_obj_visit = 0, n_obj_test = 0;

    /* A cell lists SLOT INDICES, and JceSpaceObj is 56 bytes, so an accepted
     * cell turns into a scatter over an 11 MB array at the 200k bench (85155
     * slot touches per frame).  That is memory latency, not arithmetic, so walk
     * ahead in the (contiguous) item list and prefetch the object the loop will
     * want a few iterations from now.  JCE_CULL_PREFETCH tunes the distance;
     * 0 disables, which is how the effect was measured. */
    static int s_pf = -2;
    if (s_pf == -2) {
        const char *v = getenv("JCE_CULL_PREFETCH");
        s_pf = (v && v[0]) ? atoi(v) : 8;
        if (s_pf < 0) s_pf = 0;
        if (s_pf > 64) s_pf = 64;
    }
    const int pf_dist = s_pf;

    /* Tried and rejected: carrying each object's AABB inline in the cell item
     * so the coarse reject touches no object record at all, with epoch and
     * user_id moved to dense side arrays.  It is the textbook answer and it
     * does remove every scattered read -- and it measured SLOWER.
     *
     * Interleaved A/B, three alternating rounds, normalised by work done
     * because occupied_count swings 14056..95782 between runs and an unnormalised
     * comparison is meaningless here:
     *     prefetch (this code)  13.67 ns per (cell + item)
     *     inline-bounds item    13.90 ns per (cell + item)
     * The prefetch already hides the latency the restructure removes, while the
     * item grows 4 -> 28 bytes, so the win comes back as bandwidth. It also cost
     * ~2 MB and a bounds-refresh pass on every in-place update.  Not worth it
     * unless the walk itself gets cheaper first. */

    for (uint32_t oc = 0; oc < g->occupied_count; oc++) {
        const uint32_t ci = g->occupied[oc];
        /* Decompose the linear cell index back to (x,y,z) for its world AABB. */
        const int32_t z = (int32_t)(ci / (uint32_t)plane);
        const int32_t rem = (int32_t)(ci - (uint32_t)z * (uint32_t)plane);
        const int32_t y = rem / res0;
        const int32_t x = rem - y * res0;

        /* Cell-coarse reject: skip the whole cell if its AABB sits outside
         * any frustum plane. */
        JceAABB cell_aabb;
        cell_aabb.min.x = g->world.min.x + (float)x * cell_dx;
        cell_aabb.min.y = g->world.min.y + (float)y * cell_dy;
        cell_aabb.min.z = g->world.min.z + (float)z * cell_dz;
        cell_aabb.max.x = cell_aabb.min.x + cell_dx;
        cell_aabb.max.y = cell_aabb.min.y + cell_dy;
        cell_aabb.max.z = cell_aabb.min.z + cell_dz;
        /* Tried and rejected: classify the cell three ways and let a cell that
         * is FULLY inside the frustum answer for its contents, skipping the
         * per-object test entirely. The reasoning is sound -- an object is only
         * registered in cells it overlaps, so a non-empty overlap inside the
         * frustum proves the object intersects it -- and the extra cost is one
         * subtract and compare per plane on the CELL test only.
         *
         * It does not pay here. 478 of 855 accepted cells classify as fully
         * inside, but they hold ~3% of the objects: a dense field mostly
         * OUTSIDE the frustum meets it at the boundary, and boundary cells
         * straddle by definition. Interleaved A/B, three rounds, identical
         * visible count (25433) both ways:
         *     per-object tests  67688 -> 65366   (-3.4%)
         *     query median      0.977 -> 0.957 ms, ranges 0.955-1.096 vs
         *                       0.948-1.110 -- overlapping, so not a result.
         * A camera INSIDE a dense field would flip that, and nothing here can
         * produce one; revisit with such a load, not on this evidence. */
        /* Three-way classify. A cell wholly inside the frustum answers for its
         * contents: an object is only registered in cells it overlaps, so a
         * non-empty overlap inside the frustum proves the object intersects it.
         * Exact, not conservative.
         *
         * This was measured and reverted once (-3.4% tests, ranges overlapping)
         * and the note said why: 478 of 855 accepted cells classified inside
         * but held ~3% of the objects, because that camera had the field mostly
         * OUTSIDE the frustum and met it at the boundary, where cells straddle
         * by definition. The same note named the condition to retry under -- a
         * camera INSIDE the field -- which JCE_BENCH_CAM can now produce. */
        static int s_inside = -2;
        if (s_inside == -2) {
            const char *v = getenv("JCE_CULL_CELL_INSIDE");
            s_inside = (v && v[0]) ? (v[0] != '0') : 1;
        }
        int cls;
        if (s_inside) {
            cls = jce_aabb_frustum_classify(planes, absn,
                                            cell_aabb.min, cell_aabb.max);
            if (cls == 0) continue;
        } else {
            if (!jce__aabb_in_frustum_fast(cell_aabb, planes, absn)) continue;
            cls = 1;
        }
        n_cell_acc++;

        const JceCell *c = &g->cells[ci];
        n_obj_visit += c->count;
        for (uint32_t i = 0; i < c->count; i++) {
            const uint32_t slot = c->items[i];
            if (pf_dist && i + (uint32_t)pf_dist < c->count)
                JCE_PREFETCH(&g->objs[c->items[i + (uint32_t)pf_dist]]);
            JceSpaceObj *o = &g->objs[slot];
            if (o->epoch == epoch) continue;
            o->epoch = epoch;
            if (cls != 2) {
                n_obj_test++;
                if (!jce__aabb_in_frustum_fast(o->bounds, planes, absn)) continue;
            }
            if (out_ids && found < max) out_ids[found] = o->user_id;
            found++;
            if (out_ids && found >= max) goto qf_done;
        }
    }
qf_done:
    g->q_cells_walked = g->occupied_count;
    g->q_cells_acc    = n_cell_acc;
    g->q_objs_visited = n_obj_visit;
    g->q_objs_tested  = n_obj_test;
    return found;
}

void jce_space_last_query_stats(const JceSpaceIndex *idx,
                                 uint32_t *out_cells_walked,
                                 uint32_t *out_cells_accepted,
                                 uint32_t *out_objs_visited,
                                 uint32_t *out_objs_tested)
{
    if (out_cells_walked)   *out_cells_walked   = idx ? idx->q_cells_walked : 0u;
    if (out_cells_accepted) *out_cells_accepted = idx ? idx->q_cells_acc    : 0u;
    if (out_objs_visited)   *out_objs_visited   = idx ? idx->q_objs_visited : 0u;
    if (out_objs_tested)    *out_objs_tested    = idx ? idx->q_objs_tested  : 0u;
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

uint32_t jce_space_cell_count(const JceSpaceIndex *g)
{
    return g ? g->cell_count : 0;
}

uint32_t jce_space_occupied_cell_count(const JceSpaceIndex *g)
{
    return g ? g->occupied_count : 0;
}

void jce_space_resolution(const JceSpaceIndex *g, uint32_t out_res[3])
{
    if (!out_res) return;
    if (!g) { out_res[0] = out_res[1] = out_res[2] = 0; return; }
    out_res[0] = (uint32_t)g->res[0];
    out_res[1] = (uint32_t)g->res[1];
    out_res[2] = (uint32_t)g->res[2];
}
