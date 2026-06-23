/*
 * jce_material_graph_edit.cpp — selection / clipboard operations
 * (Delete, Copy, Paste).  Phase B extraction, no logic change.
 */
#include "panels/material_graph/jce_material_graph_state.h"

#include <set>
#include <utility>
#include <vector>

namespace jce_mgp {

void delete_selected(void)
{
    if (s_g.selected.empty()) return;
    push_undo();
    /* Drop links whose endpoints are selected. Skip OUTPUT (cannot delete). */
    std::set<int> doomed;
    for (int id : s_g.selected) {
        Node *n = find_node(id);
        if (n && n->type != NT_OUTPUT) doomed.insert(id);
    }
    for (auto it = s_g.links.begin(); it != s_g.links.end(); ) {
        if (doomed.count(it->from_node) || doomed.count(it->to_node))
            it = s_g.links.erase(it);
        else
            ++it;
    }
    for (auto it = s_g.nodes.begin(); it != s_g.nodes.end(); ) {
        if (doomed.count(it->id)) it = s_g.nodes.erase(it);
        else                      ++it;
    }
    s_g.selected.clear();
}

void copy_selection(void)
{
    s_clipboard.nodes.clear();
    s_clipboard.links.clear();
    /* Copy non-OUTPUT selected nodes, normalising origin to (0,0). */
    float min_x = 1e9f, min_y = 1e9f;
    for (int id : s_g.selected) {
        Node *n = find_node(id);
        if (!n || n->type == NT_OUTPUT) continue;
        if (n->pos.x < min_x) min_x = n->pos.x;
        if (n->pos.y < min_y) min_y = n->pos.y;
    }
    if (min_x > 1e8f) { s_has_clipboard = false; return; }
    std::set<int> in_clip;
    for (int id : s_g.selected) {
        Node *n = find_node(id);
        if (!n || n->type == NT_OUTPUT) continue;
        Node copy = *n;
        copy.pos.x -= min_x;
        copy.pos.y -= min_y;
        s_clipboard.nodes.push_back(copy);
        in_clip.insert(id);
    }
    for (auto &l : s_g.links) {
        if (in_clip.count(l.from_node) && in_clip.count(l.to_node))
            s_clipboard.links.push_back(l);
    }
    s_has_clipboard = !s_clipboard.nodes.empty();
}

void paste_clipboard(ImVec2 paste_origin_local)
{
    if (!s_has_clipboard || s_clipboard.nodes.empty()) return;
    push_undo();
    std::vector<std::pair<int,int>> remap;   /* old -> new */
    s_g.selected.clear();
    for (auto &n : s_clipboard.nodes) {
        Node copy = n;
        int old_id = copy.id;
        copy.id    = s_g.next_id++;
        copy.pos.x = paste_origin_local.x + n.pos.x;
        copy.pos.y = paste_origin_local.y + n.pos.y;
        s_g.nodes.push_back(copy);
        remap.push_back({old_id, copy.id});
        s_g.selected.insert(copy.id);
    }
    auto remap_id = [&](int id) {
        for (auto &p : remap) if (p.first == id) return p.second;
        return -1;
    };
    for (auto &l : s_clipboard.links) {
        Link nl;
        nl.from_node = remap_id(l.from_node);
        nl.to_node   = remap_id(l.to_node);
        nl.from_sock = l.from_sock;
        nl.to_sock   = l.to_sock;
        if (nl.from_node >= 0 && nl.to_node >= 0) s_g.links.push_back(nl);
    }
}

} /* namespace jce_mgp */
