/*
 * jce_graph_astar.c — Generic A* on caller-supplied graph topology.
 *
 * Implementation:
 *   - Binary min-heap as the open set, keyed on f = g + h.
 *   - Open-addressed linear-probe hash map mapping node-id → state slot.
 *   - State slot stores g, parent, and an open/closed flag.
 *
 * Memory grows monotonically per context as larger graphs are searched.
 */
#include <jce/middleware/ai/jce_graph_astar.h>
#include <jce/os/core/jce_hash.h>
#include <jce/os/core/jce_profiler.h>
#include "os/core/jce_memory.h"
#include <string.h>
#include <stdlib.h>

#define INVALID_PARENT 0xFFFFFFFFu
#define EMPTY_SLOT     0xFFFFFFFFu
#define HASH_LOAD_NUM  3
#define HASH_LOAD_DEN  4

typedef struct {
    uint32_t id;
    uint32_t parent;   /* slot index of predecessor, or INVALID_PARENT */
    float    g;
    uint32_t state;    /* 0=unseen,1=open,2=closed */
} NodeState;

typedef struct {
    uint32_t slot;
    float    f;
} HeapEntry;

struct JceGraphAstar {
    JceGraphNeighborsFn neighbors_fn;
    JceGraphHeuristicFn heuristic_fn;
    void               *user;
    uint32_t            max_neighbors;

    /* Slot storage — one entry per touched node. */
    NodeState *slots;
    uint32_t   slots_count;
    uint32_t   slots_cap;

    /* Hash map: id → slot index, capacity = power of two. */
    uint32_t  *map_keys;   /* node id, or EMPTY_SLOT */
    uint32_t  *map_vals;   /* slot index */
    uint32_t   map_cap;
    uint32_t   map_count;

    /* Binary heap. */
    HeapEntry *heap;
    uint32_t   heap_size;
    uint32_t   heap_cap;

    /* Scratch for neighbor enumeration. */
    uint32_t  *nbr_ids;
    float     *nbr_costs;

    /* Stats. */
    float    last_cost;
    uint32_t last_nodes_expanded;
};

/* ================================================================== */
/* Helpers                                                             */
/* ================================================================== */

static uint32_t next_pow2(uint32_t v)
{
    uint32_t p = 1;
    while (p < v) p <<= 1;
    return p;
}

static void map_grow(JceGraphAstar *a, uint32_t need)
{
    uint32_t target = next_pow2((need * HASH_LOAD_DEN) / HASH_LOAD_NUM + 1);
    if (target < 64) target = 64;
    if (target <= a->map_cap) return;

    uint32_t *old_keys = a->map_keys;
    uint32_t *old_vals = a->map_vals;
    uint32_t  old_cap  = a->map_cap;

    a->map_keys = (uint32_t *)JCE_MALLOC(sizeof(uint32_t) * target);
    a->map_vals = (uint32_t *)JCE_MALLOC(sizeof(uint32_t) * target);
    for (uint32_t i = 0; i < target; ++i) a->map_keys[i] = EMPTY_SLOT;
    a->map_cap = target;
    a->map_count = 0;

    /* Rehash. */
    if (old_keys) {
        for (uint32_t i = 0; i < old_cap; ++i) {
            if (old_keys[i] == EMPTY_SLOT) continue;
            uint32_t k = old_keys[i];
            uint32_t v = old_vals[i];
            uint32_t mask = a->map_cap - 1;
            uint32_t h = jce_hash_mix32(k) & mask;
            while (a->map_keys[h] != EMPTY_SLOT) h = (h + 1) & mask;
            a->map_keys[h] = k;
            a->map_vals[h] = v;
            a->map_count++;
        }
        JCE_FREE(old_keys);
        JCE_FREE(old_vals);
    }
}

/* Returns slot index, allocating a new NodeState if not already present. */
static uint32_t slot_for(JceGraphAstar *a, uint32_t id)
{
    if ((a->map_count + 1) * HASH_LOAD_DEN >= a->map_cap * HASH_LOAD_NUM)
        map_grow(a, a->map_count + 1);
    uint32_t mask = a->map_cap - 1;
    uint32_t h = jce_hash_mix32(id) & mask;
    while (a->map_keys[h] != EMPTY_SLOT) {
        if (a->map_keys[h] == id) return a->map_vals[h];
        h = (h + 1) & mask;
    }
    /* Allocate new slot. */
    if (a->slots_count == a->slots_cap) {
        uint32_t nc = a->slots_cap ? a->slots_cap * 2 : 64;
        a->slots = (NodeState *)JCE_REALLOC(a->slots, sizeof(NodeState) * nc);
        a->slots_cap = nc;
    }
    uint32_t idx = a->slots_count++;
    a->slots[idx].id = id;
    a->slots[idx].parent = INVALID_PARENT;
    a->slots[idx].g = 0.0f;
    a->slots[idx].state = 0;
    a->map_keys[h] = id;
    a->map_vals[h] = idx;
    a->map_count++;
    return idx;
}

/* ----- Heap ------------------------------------------------------- */
static void heap_push(JceGraphAstar *a, uint32_t slot, float f)
{
    if (a->heap_size == a->heap_cap) {
        uint32_t nc = a->heap_cap ? a->heap_cap * 2 : 64;
        a->heap = (HeapEntry *)JCE_REALLOC(a->heap, sizeof(HeapEntry) * nc);
        a->heap_cap = nc;
    }
    uint32_t i = a->heap_size++;
    a->heap[i].slot = slot;
    a->heap[i].f = f;
    while (i > 0) {
        uint32_t p = (i - 1) / 2;
        if (a->heap[p].f <= a->heap[i].f) break;
        HeapEntry t = a->heap[p]; a->heap[p] = a->heap[i]; a->heap[i] = t;
        i = p;
    }
}

static HeapEntry heap_pop(JceGraphAstar *a)
{
    HeapEntry r = a->heap[0];
    a->heap[0] = a->heap[--a->heap_size];
    uint32_t i = 0;
    for (;;) {
        uint32_t l = 2 * i + 1, rgt = 2 * i + 2, s = i;
        if (l < a->heap_size && a->heap[l].f < a->heap[s].f) s = l;
        if (rgt < a->heap_size && a->heap[rgt].f < a->heap[s].f) s = rgt;
        if (s == i) break;
        HeapEntry t = a->heap[s]; a->heap[s] = a->heap[i]; a->heap[i] = t;
        i = s;
    }
    return r;
}

/* ================================================================== */
/* API                                                                 */
/* ================================================================== */

JceGraphAstar *jce_graph_astar_create(void)
{
    JceGraphAstar *a = (JceGraphAstar *)JCE_MALLOC(sizeof(*a));
    memset(a, 0, sizeof(*a));
    a->max_neighbors = 16;
    a->nbr_ids   = (uint32_t *)JCE_MALLOC(sizeof(uint32_t) * a->max_neighbors);
    a->nbr_costs = (float    *)JCE_MALLOC(sizeof(float)    * a->max_neighbors);
    return a;
}

void jce_graph_astar_destroy(JceGraphAstar *a)
{
    if (!a) return;
    JCE_FREE(a->slots);
    JCE_FREE(a->map_keys);
    JCE_FREE(a->map_vals);
    JCE_FREE(a->heap);
    JCE_FREE(a->nbr_ids);
    JCE_FREE(a->nbr_costs);
    JCE_FREE(a);
}

void jce_graph_astar_set_callbacks(JceGraphAstar *a,
                                   JceGraphNeighborsFn n,
                                   JceGraphHeuristicFn h,
                                   void *user)
{
    a->neighbors_fn = n; a->heuristic_fn = h; a->user = user;
}

void jce_graph_astar_set_max_neighbors(JceGraphAstar *a, uint32_t n)
{
    if (n < 1) n = 1;
    if (n == a->max_neighbors) return;
    a->max_neighbors = n;
    a->nbr_ids   = (uint32_t *)JCE_REALLOC(a->nbr_ids,   sizeof(uint32_t) * n);
    a->nbr_costs = (float    *)JCE_REALLOC(a->nbr_costs, sizeof(float)    * n);
}

static void reset_for_search(JceGraphAstar *a)
{
    a->slots_count = 0;
    a->heap_size = 0;
    /* Wipe map. */
    for (uint32_t i = 0; i < a->map_cap; ++i) a->map_keys[i] = EMPTY_SLOT;
    a->map_count = 0;
    a->last_cost = 0.0f;
    a->last_nodes_expanded = 0;
}

bool jce_graph_astar_search(JceGraphAstar *a,
                            uint32_t start, uint32_t goal,
                            uint32_t *out_path, uint32_t out_cap,
                            uint32_t *out_count)
{
    if (out_count) *out_count = 0;
    if (!a || !a->neighbors_fn || !a->heuristic_fn) return false;
    JCE_PROFILE_ZONE_N("AI::AStar::search");

    reset_for_search(a);
    /* Ensure map is initialized. */
    if (a->map_cap == 0) map_grow(a, 64);

    uint32_t s = slot_for(a, start);
    a->slots[s].g = 0.0f;
    a->slots[s].state = 1;
    a->slots[s].parent = INVALID_PARENT;
    float h0 = a->heuristic_fn(start, goal, a->user);
    heap_push(a, s, h0);

    while (a->heap_size > 0) {
        HeapEntry top = heap_pop(a);
        uint32_t cur = top.slot;
        if (a->slots[cur].state == 2) continue; /* stale entry */
        a->slots[cur].state = 2;
        a->last_nodes_expanded++;

        if (a->slots[cur].id == goal) {
            /* Reconstruct path backwards into a temp range, then reverse. */
            a->last_cost = a->slots[cur].g;
            uint32_t count = 0;
            uint32_t walker = cur;
            while (walker != INVALID_PARENT) {
                count++;
                walker = a->slots[walker].parent;
            }
            if (out_count) *out_count = count;
            if (!out_path || out_cap < count) { JCE_PROFILE_ZONE_END; return false; }
            uint32_t i = count;
            walker = cur;
            while (walker != INVALID_PARENT) {
                out_path[--i] = a->slots[walker].id;
                walker = a->slots[walker].parent;
            }
            JCE_PROFILE_PLOT_I("ai.astar.nodes_expanded", (int64_t)a->last_nodes_expanded);
            JCE_PROFILE_ZONE_END;
            return true;
        }

        uint32_t nn = a->neighbors_fn(a->slots[cur].id,
                                      a->nbr_ids, a->nbr_costs,
                                      a->max_neighbors, a->user);
        if (nn > a->max_neighbors) nn = a->max_neighbors;
        for (uint32_t k = 0; k < nn; ++k) {
            uint32_t ns_id = a->nbr_ids[k];
            float    cost  = a->nbr_costs[k];
            if (cost < 0.0f) continue;
            uint32_t ns = slot_for(a, ns_id);
            if (a->slots[ns].state == 2) continue;
            float tentative = a->slots[cur].g + cost;
            if (a->slots[ns].state == 1 && tentative >= a->slots[ns].g) continue;
            a->slots[ns].parent = cur;
            a->slots[ns].g = tentative;
            a->slots[ns].state = 1;
            float h = a->heuristic_fn(ns_id, goal, a->user);
            heap_push(a, ns, tentative + h);
        }
    }
    JCE_PROFILE_PLOT_I("ai.astar.nodes_expanded", (int64_t)a->last_nodes_expanded);
    JCE_PROFILE_ZONE_END;
    return false;
}

float jce_graph_astar_last_cost(const JceGraphAstar *a) { return a ? a->last_cost : 0.0f; }
uint32_t jce_graph_astar_last_nodes_expanded(const JceGraphAstar *a) { return a ? a->last_nodes_expanded : 0; }
