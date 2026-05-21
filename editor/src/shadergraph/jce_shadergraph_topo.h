/*
 * jce_shadergraph_topo.h — topological sort for shader graphs.
 *
 * Reverse-DFS from the singleton NT_OUTPUT node; only nodes contributing
 * to the final output appear in the result (dead branches are pruned).
 * Cycle detection short-circuits on the first back-edge encountered.
 *
 * Pure model utility; no ImGui / bgfx dependency.
 */

#pragma once

#include "shadergraph/jce_shadergraph_types.h"

#include <vector>

namespace jce_sg {

struct TopoResult {
    std::vector<int> order;       /* node ids in dependency order (leaves first) */
    bool             has_output;  /* true if a NT_OUTPUT node was found */
    int              output_id;   /* id of the NT_OUTPUT node (-1 if absent) */
    int              cycle_node;  /* -1 if acyclic; otherwise a node on the cycle */
};

TopoResult topo_sort(const Graph &g);

} /* namespace jce_sg */
