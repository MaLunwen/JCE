/*
 * jce_graph_astar.h  Generic graph A* pathfinder.
 *
 * Topology-agnostic: caller supplies node-neighbor and heuristic
 * callbacks, so this works for road networks, waypoint graphs,
 * dialogue/quest graphs, mesh-adjacency, etc.  Nodes are identified
 * by uint32_t IDs; the search returns a path as an ID array.
 *
 * Allocation strategy: the context owns persistent open/closed buffers
 * sized to the largest node count seen.  Successive searches reuse
 * memory.  Node IDs need not be contiguous — internal hash map handles
 * sparse graphs.
 *
 * Thread-safety: a JceGraphAstar context is single-threaded — never
 * call jce_graph_astar_search() concurrently on the same context.
 * Create one context per worker thread for parallel pathfinding.
 *
 * Example:
 *   JceGraphAstar *ctx = jce_graph_astar_create(8192);
 *   uint32_t path[256];
 *   uint32_t n = jce_graph_astar_search(ctx, start_id, goal_id,
 *                                       my_neighbors_cb, my_heuristic_cb,
 *                                       user_data, path, 256);
 *   for (uint32_t i = 0; i < n; ++i) follow_node(path[i]);
 *   jce_graph_astar_destroy(ctx);
 *
 * Layer: AI (Layer 3).  No game-specific concepts.
 */
#ifndef JCE_GRAPH_ASTAR_H
#define JCE_GRAPH_ASTAR_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceGraphAstar JceGraphAstar;

/* ================================================================== */
/* Callback types                                                      */
/* ================================================================== */

/*
 * Enumerate neighbors of `node`.  Caller fills `out_ids` and `out_costs`
 * up to `out_capacity` and returns the count actually written.  Edge
 * costs must be non-negative and finite.
 */
typedef uint32_t (*JceGraphNeighborsFn)(uint32_t node,
                                        uint32_t *out_ids,
                                        float    *out_costs,
                                        uint32_t  out_capacity,
                                        void     *user);

/* Heuristic estimate from `node` to `goal`.  Must be admissible
 * (never over-estimate) for A* optimality. */
typedef float (*JceGraphHeuristicFn)(uint32_t node, uint32_t goal, void *user);

/* ================================================================== */
/* Lifecycle                                                           */
/* ================================================================== */

JCE_API JceGraphAstar *jce_graph_astar_create(void);
JCE_API void           jce_graph_astar_destroy(JceGraphAstar *a);

/* Configure callbacks (and userdata).  Required before search. */
JCE_API void jce_graph_astar_set_callbacks(JceGraphAstar *a,
                                           JceGraphNeighborsFn neighbors_fn,
                                           JceGraphHeuristicFn heuristic_fn,
                                           void *user);

/* Maximum neighbors per node — used to size internal scratch arrays.
 * Default is 16; raise this if your graph has high branching factor. */
JCE_API void jce_graph_astar_set_max_neighbors(JceGraphAstar *a, uint32_t n);

/* ================================================================== */
/* Search                                                              */
/* ================================================================== */
/*
 * Find shortest path from `start` to `goal`.  On success the IDs are
 * written into `out_path` (start .. goal inclusive) up to capacity.
 *
 *   *out_count   — number of IDs written (>= 1 when found).
 *   Returns true if a path was found, false otherwise (no path or
 *   capacity exceeded — check *out_count vs capacity to disambiguate).
 *
 * Safe to call repeatedly with different start/goal — internal state
 * is reset each call.
 */
JCE_API bool jce_graph_astar_search(JceGraphAstar *a,
                                    uint32_t start, uint32_t goal,
                                    uint32_t *out_path,
                                    uint32_t  out_capacity,
                                    uint32_t *out_count);

/* Total cost of the last successful search (g-score at goal). */
JCE_API float jce_graph_astar_last_cost(const JceGraphAstar *a);

/* Number of nodes expanded during the last search (for profiling). */
JCE_API uint32_t jce_graph_astar_last_nodes_expanded(const JceGraphAstar *a);

JCE_EXTERN_C_END
#endif /* JCE_GRAPH_ASTAR_H */
