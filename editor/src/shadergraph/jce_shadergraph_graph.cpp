/*
 * jce_shadergraph_graph.cpp — pure-model graph operations.
 *
 * Extracted from jce_panel_material_graph.cpp (Phase A).  No ImGui /
 * bgfx / engine state — safe to unit-test headlessly.
 */

#include "shadergraph/jce_shadergraph_graph.h"
#include "shadergraph/jce_shadergraph_registry.h"

namespace jce_sg {

Node *find_node(Graph &g, int id)
{
    for (auto &n : g.nodes)
        if (n.id == id) return &n;
    return nullptr;
}

bool find_link_into(const Graph &g, int to_node, int to_sock, Link *out)
{
    for (const auto &l : g.links) {
        if (l.to_node == to_node && l.to_sock == to_sock) {
            if (out) *out = l;
            return true;
        }
    }
    return false;
}

Node *add_node(Graph &g, NodeType t, Vec2 pos)
{
    Node n;
    n.id   = g.next_id++;
    n.type = t;
    n.pos  = pos;
    g.nodes.push_back(n);
    return &g.nodes.back();
}

Node *ensure_output(Graph &g, Vec2 default_pos)
{
    for (auto &n : g.nodes)
        if (n.type == NT_OUTPUT) return &n;
    return add_node(g, NT_OUTPUT, default_pos);
}

bool try_make_link(Graph &g, int to_node, int to_sock, DataType to_dt)
{
    if (g.pending_from_node < 0) return false;

    Node *src = find_node(g, g.pending_from_node);
    if (!src) { g.pending_from_node = -1; return false; }

    int sn_count = 0;
    const Socket *sn = sockets_for(src->type, &sn_count);
    if (g.pending_from_sock < 0 || g.pending_from_sock >= sn_count) {
        g.pending_from_node = -1;
        return false;
    }
    if (!sn || sn[g.pending_from_sock].dtype != to_dt) {
        g.pending_from_node = -1;
        return false;
    }

    /* Single-input rule: drop any prior link feeding the destination. */
    for (auto it = g.links.begin(); it != g.links.end(); ) {
        if (it->to_node == to_node && it->to_sock == to_sock)
            it = g.links.erase(it);
        else
            ++it;
    }
    Link l;
    l.from_node = g.pending_from_node;
    l.from_sock = g.pending_from_sock;
    l.to_node   = to_node;
    l.to_sock   = to_sock;
    g.links.push_back(l);
    g.pending_from_node = -1;
    return true;
}

bool remove_node(Graph &g, int id)
{
    Node *n = find_node(g, id);
    if (!n || n->type == NT_OUTPUT) return false;

    for (auto it = g.links.begin(); it != g.links.end(); ) {
        if (it->from_node == id || it->to_node == id)
            it = g.links.erase(it);
        else
            ++it;
    }
    for (auto it = g.nodes.begin(); it != g.nodes.end(); ++it) {
        if (it->id == id) { g.nodes.erase(it); return true; }
    }
    return false;
}

void purge_links_touching(Graph &g, const std::set<int> &doomed)
{
    for (auto it = g.links.begin(); it != g.links.end(); ) {
        if (doomed.count(it->from_node) || doomed.count(it->to_node))
            it = g.links.erase(it);
        else
            ++it;
    }
}

} /* namespace jce_sg */
