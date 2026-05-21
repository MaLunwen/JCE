/*
 * jce_shadergraph_io.cpp — graph serialisation + .mat.json bootstrap.
 *
 * Phase A extraction: byte-identical JSON to the legacy panel writer.
 */

#include "shadergraph/jce_shadergraph_io.h"
#include "shadergraph/jce_shadergraph_graph.h"
#include "io/jce_editor_file_util.h"
#include "ui/jce_editor_panels.h"

extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/renderer/jce_pbr_material.h>
}

#include <cstdio>
#include <cstring>

namespace jce_sg {

bool save(const Graph &g, const char *path)
{
    if (!path || !path[0]) return false;

    JceJson *root = jce_json_object();
    jce_json_set_number(root, "nextId", (double)g.next_id);

    JceJson *nodes = jce_json_array();
    for (const auto &n : g.nodes) {
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "id",   (double)n.id);
        jce_json_set_number(o, "type", (double)n.type);
        jce_json_set_number(o, "x",    n.pos.x);
        jce_json_set_number(o, "y",    n.pos.y);
        jce_json_set_float_array(o, "color", const_cast<float *>(n.color), 4);
        jce_json_set_number(o, "scalar", n.scalar);
        jce_json_set_string(o, "text",   n.text);
        jce_json_array_push(nodes, o);
    }
    jce_json_set_child(root, "nodes", nodes);

    JceJson *links = jce_json_array();
    for (const auto &l : g.links) {
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "fromNode", (double)l.from_node);
        jce_json_set_number(o, "fromSock", (double)l.from_sock);
        jce_json_set_number(o, "toNode",   (double)l.to_node);
        jce_json_set_number(o, "toSock",   (double)l.to_sock);
        jce_json_array_push(links, o);
    }
    jce_json_set_child(root, "links", links);

    bool ok = ed_write_json_to_file(path, root);
    if (ok) {
        jce_editor_console_log("material graph saved: %s", path);
    } else {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph save failed: %s", path);
    }
    return ok;
}

bool load(Graph &g, const char *path)
{
    if (!path || !path[0]) return false;

    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph load failed: %s", path);
        return false;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return false;

    g.nodes.clear();
    g.links.clear();
    g.selected.clear();
    g.pending_from_node = -1;
    g.pending_from_sock = -1;
    g.box_active = false;
    g.next_id = jce_json_get_int(root, "nextId", 1);

    JceJson *nodes = jce_json_get(root, "nodes");
    if (nodes && jce_json_is_array(nodes)) {
        int n = jce_json_array_size(nodes);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(nodes, i);
            if (!o) continue;
            Node nd;
            nd.id    = jce_json_get_int(o, "id", 0);
            nd.type  = (NodeType)jce_json_get_int(o, "type", 0);
            nd.pos.x = (float)jce_json_get_number(o, "x", 0);
            nd.pos.y = (float)jce_json_get_number(o, "y", 0);
            jce_json_get_floats(o, "color", nd.color, 4, nd.color);
            nd.scalar = (float)jce_json_get_number(o, "scalar", 0);
            const char *txt = jce_json_get_string(o, "text", "");
            std::snprintf(nd.text, sizeof(nd.text), "%s", txt ? txt : "");
            g.nodes.push_back(nd);
        }
    }
    JceJson *links = jce_json_get(root, "links");
    if (links && jce_json_is_array(links)) {
        int n = jce_json_array_size(links);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(links, i);
            if (!o) continue;
            Link l;
            l.from_node = jce_json_get_int(o, "fromNode", 0);
            l.from_sock = jce_json_get_int(o, "fromSock", 0);
            l.to_node   = jce_json_get_int(o, "toNode",   0);
            l.to_sock   = jce_json_get_int(o, "toSock",   0);
            g.links.push_back(l);
        }
    }
    jce_json_free(root);

    ensure_output(g, Vec2{ 420.0f, 80.0f });
    jce_editor_console_log("material graph loaded: %s", path);
    return true;
}

bool import_from_pbr_material(Graph &g, const char *mat_json_path)
{
    if (!mat_json_path || !mat_json_path[0]) return false;

    JcePbrMaterial m = jce_pbr_material_default();
    char tex_paths[5][256] = {};
    if (!jce_pbr_material_load_json(mat_json_path, &m, tex_paths)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph: load .mat.json failed: %s", mat_json_path);
        return false;
    }

    g.nodes.clear();
    g.links.clear();
    g.selected.clear();
    g.pending_from_node = -1;
    g.pending_from_sock = -1;
    g.box_active = false;
    g.next_id = 1;

    Node *out = ensure_output(g, Vec2{ 420.0f, 200.0f });

    auto link_to_out = [&](int src_node_id, int out_slot) {
        Link l;
        l.from_node = src_node_id;
        l.from_sock = 0;
        l.to_node   = out->id;
        l.to_sock   = out_slot;
        g.links.push_back(l);
    };

    /* BaseColor (slot 0) */
    if (tex_paths[0][0]) {
        Node *t = add_node(g, NT_TEXTURE, Vec2{ 80.0f, 60.0f });
        std::snprintf(t->text, sizeof(t->text), "%s", tex_paths[0]);
        link_to_out(t->id, 0);
    } else {
        Node *c = add_node(g, NT_COLOR, Vec2{ 80.0f, 60.0f });
        c->color[0] = m.base_color_factor[0];
        c->color[1] = m.base_color_factor[1];
        c->color[2] = m.base_color_factor[2];
        c->color[3] = m.base_color_factor[3];
        link_to_out(c->id, 0);
    }
    /* Metallic (slot 1) */
    {
        Node *f = add_node(g, NT_FLOAT, Vec2{ 80.0f, 160.0f });
        f->scalar = m.metallic_factor;
        link_to_out(f->id, 1);
    }
    /* Roughness (slot 2) */
    {
        Node *f = add_node(g, NT_FLOAT, Vec2{ 80.0f, 240.0f });
        f->scalar = m.roughness_factor;
        link_to_out(f->id, 2);
    }
    /* Emissive (slot 3) */
    {
        Node *c = add_node(g, NT_COLOR, Vec2{ 80.0f, 320.0f });
        c->color[0] = m.emissive_factor[0];
        c->color[1] = m.emissive_factor[1];
        c->color[2] = m.emissive_factor[2];
        c->color[3] = 1.0f;
        link_to_out(c->id, 3);
    }

    jce_editor_console_log("material graph: imported %s", mat_json_path);
    return true;
}

} /* namespace jce_sg */
