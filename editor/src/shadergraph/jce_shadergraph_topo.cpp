/*
 * jce_shadergraph_topo.cpp — reverse DFS topological sort.
 *
 * Walks the link graph backwards from NT_OUTPUT to discover the set of
 * nodes whose outputs contribute to the final shader.  Nodes are emitted
 * in postorder, so for any link a -> b the producer `a` precedes the
 * consumer `b` in `result.order`.
 *
 * Cycle detection uses the classic three-colour scheme:
 *   white = not visited, grey = on current DFS stack, black = finished.
 * Re-entering a grey node yields the offending node id and aborts.
 */

#include "shadergraph/jce_shadergraph_topo.h"

#include <unordered_map>
#include <vector>

namespace jce_sg {

namespace {

enum Colour : char { WHITE = 0, GREY = 1, BLACK = 2 };

/* Collect upstream node ids feeding any input socket of `consumer`. */
void gather_upstream(const Graph &g, int consumer, std::vector<int> &out)
{
    out.clear();
    for (const Link &l : g.links) {
        if (l.to_node == consumer) out.push_back(l.from_node);
    }
}

bool dfs(const Graph &g, int node_id,
         std::unordered_map<int, Colour> &colour,
         std::vector<int> &order, int &cycle_node)
{
    Colour c = colour[node_id];
    if (c == BLACK) return true;
    if (c == GREY) {
        cycle_node = node_id;
        return false;
    }

    colour[node_id] = GREY;

    std::vector<int> upstream;
    gather_upstream(g, node_id, upstream);
    for (int up : upstream) {
        if (!dfs(g, up, colour, order, cycle_node)) return false;
    }

    colour[node_id] = BLACK;
    order.push_back(node_id);
    return true;
}

int find_output_id(const Graph &g)
{
    for (const Node &n : g.nodes) {
        if (n.type == NT_OUTPUT) return n.id;
    }
    return -1;
}

} /* anonymous namespace */

TopoResult topo_sort(const Graph &g)
{
    TopoResult r;
    r.has_output = false;
    r.output_id  = -1;
    r.cycle_node = -1;

    int out_id = find_output_id(g);
    if (out_id < 0) return r;
    r.has_output = true;
    r.output_id  = out_id;

    std::unordered_map<int, Colour> colour;
    colour.reserve(g.nodes.size());
    for (const Node &n : g.nodes) colour[n.id] = WHITE;

    if (!dfs(g, out_id, colour, r.order, r.cycle_node)) {
        r.order.clear();
    }
    return r;
}

} /* namespace jce_sg */
