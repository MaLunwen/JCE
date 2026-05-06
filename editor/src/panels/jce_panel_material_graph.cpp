/*
 * jce_panel_material_graph.cpp  Material Node Editor scaffold (Phase A).
 *
 * Hand-drawn node graph (no third-party dependency) targeting the
 * existing JcePbrMaterial struct.  Phase A scope:
 *
 *   Node types:
 *     - Output (sinks: BaseColor, Metallic, Roughness, Emissive)
 *     - Color  (rgba constant -> exposes 1 RGBA output socket)
 *     - Float  (scalar 0..1 constant -> exposes 1 Float output socket)
 *
 *   Interaction:
 *     - Drag node body to move.
 *     - Click an output pin then a compatible input pin to create a link.
 *     - Right-click on canvas opens a "create node" menu.
 *     - Right-click a node to delete it.
 *     - "Compile to .mat.json" walks the graph from the Output node
 *       and writes JcePbrMaterial via jce_pbr_material_save_json.
 *
 *   Persistence: graph is JSON-serialized to a sibling file (.matgraph.json)
 *                so the user can reopen it next session.
 *
 * Future phases will add: Texture Sampler nodes, Math nodes, UV nodes,
 * GLSL preview, hot-reload of bound material instances.
 */

#include "io/jce_editor_file_util.h"
#include "core/jce_editor_i18n.h"
#include "ui/jce_editor_panels.h"
#include "ui/jce_theme_palette.h"
#include "core/jce_hotkeys.h"

#include <jce/tools/jce_imgui.hpp>
#include <jce/tools/jce_imgui_internal.h>
extern "C" {
#include <jce/os/core/jce_json.h>
#include <jce/renderer/jce_pbr_material.h>
}

#include <cctype>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <set>
#include <vector>

namespace {

enum NodeType {
    NT_OUTPUT     = 0,
    NT_COLOR      = 1,
    NT_FLOAT      = 2,
    NT_TEXTURE    = 3,
    NT_MUL_C      = 4,    /* Color * Color -> Color */
    NT_MUL_F      = 5,    /* Float * Float -> Float */
    NT_ADD_F      = 6,    /* Float + Float -> Float */
    /* Phase D (Sprint 3 #11): node library expansion. */
    NT_NORMAL_MAP = 7,    /* Normal texture sampler -> Color (tangent space) */
    NT_UV         = 8,    /* UV constants (tile/offset) -> Float */
    NT_SUB_F      = 9,    /* Float - Float -> Float */
    NT_LERP_C     = 10,   /* Color lerp (A,B,t) -> Color */
    NT_FRESNEL    = 11,   /* Pseudo-fresnel: 1 - bias -> Float (preview) */
};

enum SocketKind { SK_INPUT = 0, SK_OUTPUT = 1 };
enum DataType   { DT_FLOAT = 0, DT_COLOR = 1 };

struct Socket {
    SocketKind  kind;
    DataType    dtype;
    const char *name;
};

struct Node {
    int     id;
    NodeType type;
    ImVec2  pos;
    /* Constant payloads. */
    float   color[4] = {1, 1, 1, 1};
    float   scalar   = 0.0f;
    char    text[256] = {0};   /* texture path */
};

struct Link {
    int from_node, from_sock;
    int to_node,   to_sock;
};

struct Graph {
    std::vector<Node> nodes;
    std::vector<Link> links;
    int next_id = 1;
    /* Pending link source. */
    int pending_from_node = -1;
    int pending_from_sock = -1;
    /* Canvas state. */
    ImVec2 scroll = ImVec2(0, 0);
    /* File path (display + save target). */
    char path[260] = {0};
    /* Phase C: selection + box-select rubber band. */
    std::set<int> selected;
    bool   box_active = false;
    ImVec2 box_start  = ImVec2(0, 0);
};

Graph s_g;

/* Compile feedback log + last-resolved PBR Output for the preview pane.
 * Captured at every compile_to_material() call so the user gets a
 * consolidated "what did the graph evaluate to" view without having to
 * re-open the resulting .mat.json on disk. */
struct CompileLog {
    char text[2048];
    bool has_error;
};
CompileLog s_log;

struct PreviewState {
    float base_color[4]   = {1, 1, 1, 1};
    float metallic        = 0.0f;
    float roughness       = 0.5f;
    float emissive[3]     = {0, 0, 0};
    char  base_tex[256]   = {0};
    char  mr_tex[256]     = {0};
    char  emis_tex[256]   = {0};
    bool  valid           = false;
};
PreviewState s_prev;

void log_clear(void)
{
    s_log.text[0]   = 0;
    s_log.has_error = false;
}

void log_append(bool err, const char *fmt, ...)
{
    if (err) s_log.has_error = true;
    size_t cur = std::strlen(s_log.text);
    if (cur + 256 >= sizeof(s_log.text)) return;
    va_list ap; va_start(ap, fmt);
    vsnprintf(s_log.text + cur, sizeof(s_log.text) - cur, fmt, ap);
    va_end(ap);
    cur = std::strlen(s_log.text);
    if (cur + 1 < sizeof(s_log.text)) {
        s_log.text[cur]   = '\n';
        s_log.text[cur+1] = 0;
    }
}

/* Undo/redo stacks + clipboard (Phase C). */
std::vector<Graph> s_undo;
std::vector<Graph> s_redo;
Graph              s_clipboard;
bool               s_has_clipboard = false;

void push_undo(void)
{
    if (s_undo.size() > 64) s_undo.erase(s_undo.begin());
    s_undo.push_back(s_g);
    s_redo.clear();
}

void do_undo(void)
{
    if (s_undo.empty()) return;
    s_redo.push_back(s_g);
    s_g = s_undo.back();
    s_undo.pop_back();
}

void do_redo(void)
{
    if (s_redo.empty()) return;
    s_undo.push_back(s_g);
    s_g = s_redo.back();
    s_redo.pop_back();
}

const Socket *node_sockets(NodeType t, int *out_count)
{
    static const Socket s_output[] = {
        { SK_INPUT, DT_COLOR, "BaseColor" },
        { SK_INPUT, DT_FLOAT, "Metallic"  },
        { SK_INPUT, DT_FLOAT, "Roughness" },
        { SK_INPUT, DT_COLOR, "Emissive"  },
    };
    static const Socket s_color  [] = { { SK_OUTPUT, DT_COLOR, "RGBA" } };
    static const Socket s_float  [] = { { SK_OUTPUT, DT_FLOAT, "Out"  } };
    static const Socket s_texture[] = { { SK_OUTPUT, DT_COLOR, "RGBA" } };
    static const Socket s_mul_c  [] = {
        { SK_INPUT,  DT_COLOR, "A" },
        { SK_INPUT,  DT_COLOR, "B" },
        { SK_OUTPUT, DT_COLOR, "Out" },
    };
    static const Socket s_mul_f  [] = {
        { SK_INPUT,  DT_FLOAT, "A" },
        { SK_INPUT,  DT_FLOAT, "B" },
        { SK_OUTPUT, DT_FLOAT, "Out" },
    };
    static const Socket s_add_f  [] = {
        { SK_INPUT,  DT_FLOAT, "A" },
        { SK_INPUT,  DT_FLOAT, "B" },
        { SK_OUTPUT, DT_FLOAT, "Out" },
    };
    static const Socket s_normal [] = { { SK_OUTPUT, DT_COLOR, "Normal" } };
    static const Socket s_uv     [] = { { SK_OUTPUT, DT_FLOAT, "Tile"   } };
    static const Socket s_sub_f  [] = {
        { SK_INPUT,  DT_FLOAT, "A" },
        { SK_INPUT,  DT_FLOAT, "B" },
        { SK_OUTPUT, DT_FLOAT, "Out" },
    };
    static const Socket s_lerp_c [] = {
        { SK_INPUT,  DT_COLOR, "A" },
        { SK_INPUT,  DT_COLOR, "B" },
        { SK_INPUT,  DT_FLOAT, "T" },
        { SK_OUTPUT, DT_COLOR, "Out" },
    };
    static const Socket s_fresnel[] = {
        { SK_INPUT,  DT_FLOAT, "Bias" },
        { SK_OUTPUT, DT_FLOAT, "Out"  },
    };
    switch (t) {
        case NT_OUTPUT:     *out_count = 4; return s_output;
        case NT_COLOR:      *out_count = 1; return s_color;
        case NT_FLOAT:      *out_count = 1; return s_float;
        case NT_TEXTURE:    *out_count = 1; return s_texture;
        case NT_MUL_C:      *out_count = 3; return s_mul_c;
        case NT_MUL_F:      *out_count = 3; return s_mul_f;
        case NT_ADD_F:      *out_count = 3; return s_add_f;
        case NT_NORMAL_MAP: *out_count = 1; return s_normal;
        case NT_UV:         *out_count = 1; return s_uv;
        case NT_SUB_F:      *out_count = 3; return s_sub_f;
        case NT_LERP_C:     *out_count = 4; return s_lerp_c;
        case NT_FRESNEL:    *out_count = 2; return s_fresnel;
    }
    *out_count = 0;
    return nullptr;
}

const char *node_label(NodeType t)
{
    switch (t) {
        case NT_OUTPUT:  return "PBR Output";
        case NT_COLOR:   return "Color";
        case NT_FLOAT:   return "Float";
        case NT_TEXTURE: return "Texture";
        case NT_MUL_C:      return "Multiply (Color)";
        case NT_MUL_F:      return "Multiply (Float)";
        case NT_ADD_F:      return "Add (Float)";
        case NT_NORMAL_MAP: return "Normal Map";
        case NT_UV:         return "UV (Tile/Offset)";
        case NT_SUB_F:      return "Subtract (Float)";
        case NT_LERP_C:     return "Lerp (Color)";
        case NT_FRESNEL:    return "Fresnel (Preview)";
    }
    return "?";
}

void ensure_output_node(void)
{
    for (auto &n : s_g.nodes) if (n.type == NT_OUTPUT) return;
    Node n; n.id = s_g.next_id++; n.type = NT_OUTPUT;
    n.pos = ImVec2(420.0f, 80.0f);
    s_g.nodes.push_back(n);
}

void add_node_at(NodeType t, ImVec2 local)
{
    push_undo();
    Node n; n.id = s_g.next_id++; n.type = t; n.pos = local;
    s_g.nodes.push_back(n);
}

/* User-facing addable types (excludes NT_OUTPUT — there's only one). */
const NodeType kAddable[] = {
    NT_COLOR, NT_FLOAT, NT_TEXTURE, NT_NORMAL_MAP, NT_UV,
    NT_MUL_C, NT_MUL_F, NT_ADD_F, NT_SUB_F, NT_LERP_C, NT_FRESNEL,
};
const int kAddableCount = (int)(sizeof(kAddable) / sizeof(kAddable[0]));

/* Quick-add palette state. */
bool   g_qa_open = false;
char   g_qa_filter[64] = {0};
ImVec2 g_qa_local;
int    g_qa_focus_request = 0;
int    g_qa_highlight = 0;

Node *find_node(int id)
{
    for (auto &n : s_g.nodes) if (n.id == id) return &n;
    return nullptr;
}

ImVec2 socket_screen_pos(ImVec2 node_origin, ImVec2 node_size, int sock_idx,
                         SocketKind kind)
{
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float y = node_origin.y + 28.0f + sock_idx * row_h + row_h * 0.5f;
    float x = (kind == SK_INPUT) ? node_origin.x
                                 : node_origin.x + node_size.x;
    return ImVec2(x, y);
}

void try_make_link(int to_node, int to_sock, DataType to_dt)
{
    if (s_g.pending_from_node < 0) return;
    Node *src = find_node(s_g.pending_from_node);
    if (!src) { s_g.pending_from_node = -1; return; }
    int sn_count = 0;
    const Socket *sn = node_sockets(src->type, &sn_count);
    if (s_g.pending_from_sock < 0 || s_g.pending_from_sock >= sn_count) {
        s_g.pending_from_node = -1;
        return;
    }
    if (sn[s_g.pending_from_sock].dtype != to_dt) {
        s_g.pending_from_node = -1;
        return;
    }
    push_undo();
    /* Replace any existing link into (to_node, to_sock) — single-input rule. */
    for (auto it = s_g.links.begin(); it != s_g.links.end(); ) {
        if (it->to_node == to_node && it->to_sock == to_sock)
            it = s_g.links.erase(it);
        else
            ++it;
    }
    Link l;
    l.from_node = s_g.pending_from_node;
    l.from_sock = s_g.pending_from_sock;
    l.to_node   = to_node;
    l.to_sock   = to_sock;
    s_g.links.push_back(l);
    s_g.pending_from_node = -1;
}

bool find_link_into(int to_node, int to_sock, Link *out)
{
    for (auto &l : s_g.links) {
        if (l.to_node == to_node && l.to_sock == to_sock) {
            *out = l; return true;
        }
    }
    return false;
}

/* Recursive evaluator. Returns a 4-component color (alpha=1 for floats)
 * via *out_color and an optional texture path via *out_tex (empty = none).
 * For float sockets only out_color[0] is meaningful. */
void eval_socket(int node_id, int sock_idx, float out_color[4],
                 char out_tex[256], int depth)
{
    out_color[0] = out_color[1] = out_color[2] = out_color[3] = 1.0f;
    out_tex[0] = 0;
    if (depth > 16) return;
    Link l;
    if (!find_link_into(node_id, sock_idx, &l)) return;
    Node *n = find_node(l.from_node);
    if (!n) return;
    switch (n->type) {
        case NT_COLOR:
            for (int i = 0; i < 4; ++i) out_color[i] = n->color[i];
            return;
        case NT_FLOAT:
            out_color[0] = n->scalar; out_color[1] = out_color[2] = 0;
            out_color[3] = 1.0f;
            return;
        case NT_TEXTURE:
            std::snprintf(out_tex, 256, "%s", n->text);
            return;
        case NT_MUL_C: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            for (int i = 0; i < 4; ++i) out_color[i] = a[i] * b[i];
            if (ta[0]) std::snprintf(out_tex, 256, "%s", ta);
            else if (tb[0]) std::snprintf(out_tex, 256, "%s", tb);
            return;
        }
        case NT_MUL_F: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            out_color[0] = a[0] * b[0];
            return;
        }
        case NT_ADD_F: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            out_color[0] = a[0] + b[0];
            return;
        }
        case NT_NORMAL_MAP:
            std::snprintf(out_tex, 256, "%s", n->text);
            /* Default tangent-space "up" if not sampled. */
            out_color[0] = 0.5f; out_color[1] = 0.5f;
            out_color[2] = 1.0f; out_color[3] = 1.0f;
            return;
        case NT_UV:
            /* Tile factor lives in scalar; preview-only constant. */
            out_color[0] = n->scalar;
            return;
        case NT_SUB_F: {
            float a[4], b[4]; char ta[256], tb[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            out_color[0] = a[0] - b[0];
            return;
        }
        case NT_LERP_C: {
            float a[4], b[4], t[4]; char ta[256], tb[256], tt[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            eval_socket(n->id, 1, b, tb, depth + 1);
            eval_socket(n->id, 2, t, tt, depth + 1);
            float k = t[0]; if (k < 0) k = 0; if (k > 1) k = 1;
            for (int i = 0; i < 4; ++i) out_color[i] = a[i] * (1.0f - k) + b[i] * k;
            return;
        }
        case NT_FRESNEL: {
            float a[4]; char ta[256];
            eval_socket(n->id, 0, a, ta, depth + 1);
            float bias = a[0]; if (bias < 0) bias = 0; if (bias > 1) bias = 1;
            out_color[0] = 1.0f - bias;
            return;
        }
        case NT_OUTPUT:
            return;
    }
}

void compile_to_material(void)
{
    log_clear();
    s_prev.valid = false;
    Node *out = nullptr;
    for (auto &n : s_g.nodes) if (n.type == NT_OUTPUT) { out = &n; break; }
    if (!out) {
        log_append(true, "missing PBR Output node");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph: missing PBR Output node");
        return;
    }
    if (s_g.path[0] == 0) {
        log_append(true, "no .matgraph.json file path set");
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph: set a file path before compiling");
        return;
    }

    JcePbrMaterial m = jce_pbr_material_default();
    char tex_paths[5][256] = {};

    /* BaseColor (slot 0). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 0, c, tex, 0);
        m.base_color_factor[0] = c[0];
        m.base_color_factor[1] = c[1];
        m.base_color_factor[2] = c[2];
        m.base_color_factor[3] = c[3];
        if (tex[0]) std::snprintf(tex_paths[0], 256, "%s", tex);
        for (int i = 0; i < 4; ++i) s_prev.base_color[i] = c[i];
        std::snprintf(s_prev.base_tex, sizeof(s_prev.base_tex), "%s", tex);
        log_append(false, "BaseColor: rgba=(%.2f,%.2f,%.2f,%.2f) tex=\"%s\"",
                   c[0], c[1], c[2], c[3], tex_paths[0][0] ? tex_paths[0] : "(none)");
    }
    /* Metallic (slot 1). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 1, c, tex, 0);
        m.metallic_factor = c[0];
        if (tex[0]) std::snprintf(tex_paths[1], 256, "%s", tex);
        s_prev.metallic = c[0];
        std::snprintf(s_prev.mr_tex, sizeof(s_prev.mr_tex), "%s", tex);
        log_append(false, "Metallic: %.3f tex=\"%s\"", c[0],
                   tex_paths[1][0] ? tex_paths[1] : "(none)");
    }
    /* Roughness (slot 2). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 2, c, tex, 0);
        m.roughness_factor = c[0];
        if (tex[0] && !tex_paths[1][0])
            std::snprintf(tex_paths[1], 256, "%s", tex);
        s_prev.roughness = c[0];
        log_append(false, "Roughness: %.3f tex=\"%s\"", c[0],
                   tex[0] ? tex : "(none)");
    }
    /* Emissive (slot 3). */
    {
        float c[4]; char tex[256];
        eval_socket(out->id, 3, c, tex, 0);
        m.emissive_factor[0] = c[0];
        m.emissive_factor[1] = c[1];
        m.emissive_factor[2] = c[2];
        if (tex[0]) std::snprintf(tex_paths[4], 256, "%s", tex);
        s_prev.emissive[0] = c[0];
        s_prev.emissive[1] = c[1];
        s_prev.emissive[2] = c[2];
        std::snprintf(s_prev.emis_tex, sizeof(s_prev.emis_tex), "%s", tex);
        log_append(false, "Emissive: rgb=(%.2f,%.2f,%.2f) tex=\"%s\"",
                   c[0], c[1], c[2], tex_paths[4][0] ? tex_paths[4] : "(none)");
    }

    s_prev.valid = true;
    if (jce_pbr_material_save_json(s_g.path, &m, tex_paths)) {
        log_append(false, "saved -> %s", s_g.path);
        jce_editor_console_log("material compiled: %s", s_g.path);
    } else {
        log_append(true, "save failed: %s", s_g.path);
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material compile failed: %s", s_g.path);
    }
}

void save_graph(const char *path)
{
    JceJson *root = jce_json_object();
    jce_json_set_number(root, "nextId", (double)s_g.next_id);
    JceJson *nodes = jce_json_array();
    for (auto &n : s_g.nodes) {
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "id",   (double)n.id);
        jce_json_set_number(o, "type", (double)n.type);
        jce_json_set_number(o, "x",    n.pos.x);
        jce_json_set_number(o, "y",    n.pos.y);
        jce_json_set_float_array(o, "color", n.color, 4);
        jce_json_set_number(o, "scalar", n.scalar);
        jce_json_set_string(o, "text",   n.text);
        jce_json_array_push(nodes, o);
    }
    jce_json_set_child(root, "nodes", nodes);
    JceJson *links = jce_json_array();
    for (auto &l : s_g.links) {
        JceJson *o = jce_json_object();
        jce_json_set_number(o, "fromNode", (double)l.from_node);
        jce_json_set_number(o, "fromSock", (double)l.from_sock);
        jce_json_set_number(o, "toNode",   (double)l.to_node);
        jce_json_set_number(o, "toSock",   (double)l.to_sock);
        jce_json_array_push(links, o);
    }
    jce_json_set_child(root, "links", links);
    if (ed_write_json_to_file(path, root))
        jce_editor_console_log("material graph saved: %s", path);
    else
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph save failed: %s", path);
}

void load_graph(const char *path)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph load failed: %s", path);
        return;
    }
    JceJson *root = jce_json_parse(buf, sz);
    ED_FREE(buf);
    if (!root) return;
    s_g.nodes.clear();
    s_g.links.clear();
    s_g.next_id = jce_json_get_int(root, "nextId", 1);
    JceJson *nodes = jce_json_get(root, "nodes");
    if (nodes && jce_json_is_array(nodes)) {
        int n = jce_json_array_size(nodes);
        for (int i = 0; i < n; ++i) {
            JceJson *o = jce_json_array_at(nodes, i);
            if (!o) continue;
            Node nd;
            nd.id   = jce_json_get_int(o, "id", 0);
            nd.type = (NodeType)jce_json_get_int(o, "type", 0);
            nd.pos.x = (float)jce_json_get_number(o, "x", 0);
            nd.pos.y = (float)jce_json_get_number(o, "y", 0);
            jce_json_get_floats(o, "color", nd.color, 4, nd.color);
            nd.scalar = (float)jce_json_get_number(o, "scalar", 0);
            const char *txt = jce_json_get_string(o, "text", "");
            std::snprintf(nd.text, sizeof(nd.text), "%s", txt ? txt : "");
            s_g.nodes.push_back(nd);
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
            s_g.links.push_back(l);
        }
    }
    jce_json_free(root);
    ensure_output_node();
    jce_editor_console_log("material graph loaded: %s", path);
}

/* Inverse of compile_to_material: import a .mat.json into the graph,
 * auto-creating Texture/Color/Float nodes wired into the PBR Output. */
void import_from_material(const char *path)
{
    JcePbrMaterial m  = jce_pbr_material_default();
    char tex_paths[5][256] = {};
    if (!jce_pbr_material_load_json(path, &m, tex_paths)) {
        jce_editor_console_log_level(JCE_CONSOLE_ERROR,
            "material graph: load .mat.json failed: %s", path);
        return;
    }

    push_undo();
    s_g.nodes.clear();
    s_g.links.clear();
    s_g.next_id = 1;

    auto add = [&](NodeType t, ImVec2 pos) -> Node * {
        Node n; n.id = s_g.next_id++; n.type = t; n.pos = pos;
        s_g.nodes.push_back(n);
        return &s_g.nodes.back();
    };
    auto link_to_out = [&](int src_node_id, int out_slot) {
        Node *out = nullptr;
        for (auto &n : s_g.nodes) if (n.type == NT_OUTPUT) { out = &n; break; }
        if (!out) return;
        Link l { src_node_id, 0, out->id, out_slot };
        s_g.links.push_back(l);
    };

    Node *out = add(NT_OUTPUT, ImVec2(420, 200));

    /* BaseColor (slot 0) */
    if (tex_paths[0][0]) {
        Node *t = add(NT_TEXTURE, ImVec2(80, 60));
        std::snprintf(t->text, sizeof(t->text), "%s", tex_paths[0]);
        link_to_out(t->id, 0);
    } else {
        Node *c = add(NT_COLOR, ImVec2(80, 60));
        c->color[0] = m.base_color_factor[0];
        c->color[1] = m.base_color_factor[1];
        c->color[2] = m.base_color_factor[2];
        c->color[3] = m.base_color_factor[3];
        link_to_out(c->id, 0);
    }

    /* Metallic (slot 1) */
    {
        Node *f = add(NT_FLOAT, ImVec2(80, 160));
        f->scalar = m.metallic_factor;
        link_to_out(f->id, 1);
    }

    /* Roughness (slot 2) */
    {
        Node *f = add(NT_FLOAT, ImVec2(80, 240));
        f->scalar = m.roughness_factor;
        link_to_out(f->id, 2);
    }

    /* Emissive (slot 3) */
    {
        Node *c = add(NT_COLOR, ImVec2(80, 320));
        c->color[0] = m.emissive_factor[0];
        c->color[1] = m.emissive_factor[1];
        c->color[2] = m.emissive_factor[2];
        c->color[3] = 1.0f;
        link_to_out(c->id, 3);
    }

    (void)out;
    jce_editor_console_log("material graph: imported %s", path);
}

/* ---------------- Phase C selection / clipboard ---------------- */

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

void draw_node(Node &n, ImDrawList *dl, ImVec2 origin)
{
    int sock_count = 0;
    const Socket *socks = node_sockets(n.type, &sock_count);
    float content_h = sock_count * ImGui::GetTextLineHeightWithSpacing();
    if (n.type == NT_COLOR)      content_h += 24.0f;
    if (n.type == NT_FLOAT)      content_h += 24.0f;
    if (n.type == NT_TEXTURE)    content_h += 24.0f;
    if (n.type == NT_NORMAL_MAP) content_h += 24.0f;
    if (n.type == NT_UV)         content_h += 24.0f;
    float node_w = (n.type == NT_TEXTURE || n.type == NT_NORMAL_MAP) ? 220.0f : 170.0f;
    ImVec2 size = ImVec2(node_w, 28.0f + content_h + 6.0f);
    ImVec2 tl   = ImVec2(origin.x + n.pos.x, origin.y + n.pos.y);
    ImVec2 br   = ImVec2(tl.x + size.x, tl.y + size.y);

    /* Body. */
    dl->AddRectFilled(tl, br, jce_theme::col_from(ImGuiCol_FrameBg, 0.95f), 6.0f);
    bool sel = s_g.selected.count(n.id) > 0;
    if (sel)
        dl->AddRect(tl, br, jce_theme::selection_outline(), 6.0f, 0, 2.5f);
    else
        dl->AddRect(tl, br, jce_theme::col_from(ImGuiCol_Border), 6.0f, 0, 1.5f);
    dl->AddRectFilled(tl, ImVec2(br.x, tl.y + 22.0f),
                      jce_theme::col_from(ImGuiCol_TitleBgActive), 6.0f,
                      ImDrawFlags_RoundCornersTop);
    dl->AddText(ImVec2(tl.x + 8.0f, tl.y + 4.0f),
                jce_theme::text_primary(), node_label(n.type));

    /* Drag handle = title bar. */
    ImGui::SetCursorScreenPos(tl);
    ImGui::PushID(n.id);
    ImGui::InvisibleButton("title", ImVec2(size.x, 22.0f));
    if (ImGui::IsItemActivated()) {
        bool shift = ImGui::GetIO().KeyShift;
        if (shift) {
            if (s_g.selected.count(n.id)) s_g.selected.erase(n.id);
            else                          s_g.selected.insert(n.id);
        } else if (!s_g.selected.count(n.id)) {
            s_g.selected.clear();
            s_g.selected.insert(n.id);
        }
    }
    if (ImGui::IsItemActive() && ImGui::IsMouseDragging(0)) {
        ImVec2 d = ImGui::GetIO().MouseDelta;
        if (s_g.selected.count(n.id) && s_g.selected.size() > 1) {
            for (auto &m : s_g.nodes) {
                if (s_g.selected.count(m.id)) {
                    m.pos.x += d.x; m.pos.y += d.y;
                }
            }
        } else {
            n.pos.x += d.x; n.pos.y += d.y;
        }
    }
    if (ImGui::IsItemHovered() && ImGui::IsMouseClicked(1) && n.type != NT_OUTPUT)
        ImGui::OpenPopup("node_ctx");

    if (ImGui::BeginPopup("node_ctx")) {
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.delete"))) {
            push_undo();
            int del_id = n.id;
            for (auto it = s_g.links.begin(); it != s_g.links.end(); ) {
                if (it->from_node == del_id || it->to_node == del_id)
                    it = s_g.links.erase(it);
                else
                    ++it;
            }
            for (auto it = s_g.nodes.begin(); it != s_g.nodes.end(); ++it) {
                if (it->id == del_id) { s_g.nodes.erase(it); break; }
            }
            ImGui::EndPopup();
            ImGui::PopID();
            return;
        }
        ImGui::EndPopup();
    }

    /* Sockets + pin hit-testing. */
    for (int i = 0; i < sock_count; ++i) {
        const Socket &s = socks[i];
        ImVec2 pin = socket_screen_pos(tl, size, i, s.kind);
        ImU32 col = (s.dtype == DT_COLOR) ? IM_COL32(220, 200, 80, 255)
                                          : IM_COL32(120, 220, 200, 255);
        dl->AddCircleFilled(pin, 5.0f, col);
        ImVec2 lbl_pos = (s.kind == SK_INPUT)
            ? ImVec2(pin.x + 8.0f, pin.y - ImGui::GetTextLineHeight() * 0.5f)
            : ImVec2(pin.x - 8.0f - ImGui::CalcTextSize(s.name).x,
                     pin.y - ImGui::GetTextLineHeight() * 0.5f);
        dl->AddText(lbl_pos, jce_theme::text_primary(), s.name);

        ImGui::SetCursorScreenPos(ImVec2(pin.x - 6.0f, pin.y - 6.0f));
        ImGui::PushID(i);
        ImGui::InvisibleButton("pin", ImVec2(12.0f, 12.0f));
        if (ImGui::IsItemClicked()) {
            if (s.kind == SK_OUTPUT) {
                s_g.pending_from_node = n.id;
                s_g.pending_from_sock = i;
            } else {
                try_make_link(n.id, i, s.dtype);
            }
        }
        ImGui::PopID();
    }

    /* Constant payload editor. push_undo on first activation so the
     * pre-edit value can be restored via Ctrl+Z. */
    if (n.type == NT_COLOR) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::ColorEdit4("##c", n.color,
                          ImGuiColorEditFlags_NoInputs |
                          ImGuiColorEditFlags_AlphaBar);
        if (ImGui::IsItemActivated()) push_undo();
    } else if (n.type == NT_FLOAT) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::SliderFloat("##s", &n.scalar, 0.0f, 1.0f, "%.3f");
        if (ImGui::IsItemActivated()) push_undo();
    } else if (n.type == NT_TEXTURE || n.type == NT_NORMAL_MAP) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::InputText("##tex", n.text, sizeof(n.text));
        if (ImGui::IsItemActivated()) push_undo();
    } else if (n.type == NT_UV) {
        ImGui::SetCursorScreenPos(ImVec2(tl.x + 6.0f,
                                         tl.y + 28.0f + sock_count *
                                         ImGui::GetTextLineHeightWithSpacing()));
        ImGui::SetNextItemWidth(size.x - 12.0f);
        ImGui::SliderFloat("##tile", &n.scalar, 0.1f, 16.0f, jce_editor_i18n("materialGraph.node.tileFmt"));
        if (ImGui::IsItemActivated()) push_undo();
    }

    ImGui::PopID();
}

void draw_canvas(void)
{
    ImVec2 canvas_p0 = ImGui::GetCursorScreenPos();
    ImVec2 canvas_sz = ImGui::GetContentRegionAvail();
    if (canvas_sz.x < 200.0f) canvas_sz.x = 200.0f;
    if (canvas_sz.y < 200.0f) canvas_sz.y = 200.0f;
    ImVec2 canvas_p1 = ImVec2(canvas_p0.x + canvas_sz.x, canvas_p0.y + canvas_sz.y);

    ImDrawList *dl = ImGui::GetWindowDrawList();
    dl->AddRectFilled(canvas_p0, canvas_p1, jce_theme::canvas_bg());

    /* Grid. */
    const float grid = 32.0f;
    for (float x = fmodf(s_g.scroll.x, grid); x < canvas_sz.x; x += grid)
        dl->AddLine(ImVec2(canvas_p0.x + x, canvas_p0.y),
                    ImVec2(canvas_p0.x + x, canvas_p1.y),
                    jce_theme::grid_minor());
    for (float y = fmodf(s_g.scroll.y, grid); y < canvas_sz.y; y += grid)
        dl->AddLine(ImVec2(canvas_p0.x, canvas_p0.y + y),
                    ImVec2(canvas_p1.x, canvas_p0.y + y),
                    jce_theme::grid_minor());

    ImGui::InvisibleButton("canvas", canvas_sz,
                           ImGuiButtonFlags_MouseButtonLeft |
                           ImGuiButtonFlags_MouseButtonRight);
    bool canvas_hovered = ImGui::IsItemHovered();
    if (canvas_hovered && ImGui::IsMouseDragging(2)) {
        s_g.scroll.x += ImGui::GetIO().MouseDelta.x;
        s_g.scroll.y += ImGui::GetIO().MouseDelta.y;
    }

    /* Begin box-select on left-mouse-down on empty canvas. */
    if (canvas_hovered && ImGui::IsMouseClicked(0) && !s_g.box_active) {
        bool shift = ImGui::GetIO().KeyShift;
        if (!shift) s_g.selected.clear();
        s_g.box_active = true;
        s_g.box_start  = ImGui::GetIO().MousePos;
    }

    if (canvas_hovered && ImGui::IsMouseClicked(1))
        ImGui::OpenPopup("canvas_ctx");

    /* Keyboard shortcuts (panel-focused). */
    if (ImGui::IsWindowFocused(ImGuiFocusedFlags_RootAndChildWindows)) {
        ImGuiIO &io = ImGui::GetIO();
        if (jce_hotkey_pressed(JCE_HK_EDIT_UNDO))      do_undo();
        if (jce_hotkey_pressed(JCE_HK_EDIT_REDO))      do_redo();
        if (jce_hotkey_pressed(JCE_HK_EDIT_COPY))      copy_selection();
        if (jce_hotkey_pressed(JCE_HK_EDIT_PASTE)) {
            ImVec2 mp    = io.MousePos;
            ImVec2 local = ImVec2(mp.x - canvas_p0.x - s_g.scroll.x,
                                  mp.y - canvas_p0.y - s_g.scroll.y);
            paste_clipboard(local);
        }
        if (jce_hotkey_pressed(JCE_HK_EDIT_DELETE)) delete_selected();
        /* Quick-add palette: Space over canvas. */
        if (canvas_hovered && ImGui::IsKeyPressed(ImGuiKey_Space) &&
            !io.KeyCtrl && !io.KeyAlt && !io.KeyShift) {
            ImVec2 mp = io.MousePos;
            g_qa_local = ImVec2(mp.x - canvas_p0.x - s_g.scroll.x,
                                mp.y - canvas_p0.y - s_g.scroll.y);
            g_qa_filter[0] = 0;
            g_qa_highlight = 0;
            g_qa_focus_request = 1;
            ImGui::OpenPopup("matgraph_quick_add");
        }
    }

    ImVec2 origin = ImVec2(canvas_p0.x + s_g.scroll.x, canvas_p0.y + s_g.scroll.y);

    /* Links (under nodes). */
    for (auto &l : s_g.links) {
        Node *a = find_node(l.from_node);
        Node *b = find_node(l.to_node);
        if (!a || !b) continue;
        int ac = 0, bc = 0;
        const Socket *as = node_sockets(a->type, &ac);
        const Socket *bs = node_sockets(b->type, &bc);
        if (l.from_sock >= ac || l.to_sock >= bc) continue;
        ImVec2 atl = ImVec2(origin.x + a->pos.x, origin.y + a->pos.y);
        ImVec2 btl = ImVec2(origin.x + b->pos.x, origin.y + b->pos.y);
        ImVec2 asize(170.0f, 0.0f), bsize(170.0f, 0.0f);
        ImVec2 p0 = socket_screen_pos(atl, asize, l.from_sock, SK_OUTPUT);
        ImVec2 p1 = socket_screen_pos(btl, bsize, l.to_sock,   SK_INPUT);
        ImVec2 c1 = ImVec2(p0.x + 50.0f, p0.y);
        ImVec2 c2 = ImVec2(p1.x - 50.0f, p1.y);
        ImU32 col = (as[l.from_sock].dtype == DT_COLOR)
            ? IM_COL32(220, 200, 80, 255) : IM_COL32(120, 220, 200, 255);
        dl->AddBezierCubic(p0, c1, c2, p1, col, 2.5f);
        (void)bs;
    }

    /* Nodes. */
    for (auto &n : s_g.nodes) draw_node(n, dl, origin);

    /* Box select rubber band + commit. */
    if (s_g.box_active) {
        ImVec2 a = s_g.box_start;
        ImVec2 b = ImGui::GetIO().MousePos;
        ImVec2 mn(a.x < b.x ? a.x : b.x, a.y < b.y ? a.y : b.y);
        ImVec2 mx(a.x > b.x ? a.x : b.x, a.y > b.y ? a.y : b.y);
        if ((mx.x - mn.x) > 2.0f || (mx.y - mn.y) > 2.0f) {
            dl->AddRectFilled(mn, mx, IM_COL32(255, 200, 60, 35));
            dl->AddRect      (mn, mx, jce_theme::selection_outline());
        }
        if (ImGui::IsMouseReleased(0)) {
            for (auto &n : s_g.nodes) {
                int sc = 0; (void)node_sockets(n.type, &sc);
                float w = (n.type == NT_TEXTURE || n.type == NT_NORMAL_MAP) ? 220.0f : 170.0f;
                ImVec2 ntl(origin.x + n.pos.x, origin.y + n.pos.y);
                ImVec2 nbr(ntl.x + w, ntl.y + 22.0f + 16.0f);
                if (ntl.x < mx.x && nbr.x > mn.x &&
                    ntl.y < mx.y && nbr.y > mn.y)
                    s_g.selected.insert(n.id);
            }
            s_g.box_active = false;
        }
    }

    /* Pending link rubber band. */
    if (s_g.pending_from_node >= 0) {
        Node *src = find_node(s_g.pending_from_node);
        if (src) {
            ImVec2 atl = ImVec2(origin.x + src->pos.x, origin.y + src->pos.y);
            ImVec2 p0  = socket_screen_pos(atl, ImVec2(170, 0),
                                           s_g.pending_from_sock, SK_OUTPUT);
            ImVec2 mp  = ImGui::GetIO().MousePos;
            dl->AddBezierCubic(p0,
                               ImVec2(p0.x + 50.0f, p0.y),
                               ImVec2(mp.x - 50.0f, mp.y), mp,
                               jce_theme::col_from(ImGuiCol_Text, 0.8f), 2.0f);
            if (ImGui::IsMouseClicked(1)) s_g.pending_from_node = -1;
        }
    }

    if (ImGui::BeginPopup("canvas_ctx")) {
        s_g.box_active = false;   /* don't commit a stray box on right-click */
        ImVec2 mp = ImGui::GetMousePosOnOpeningCurrentPopup();
        ImVec2 local = ImVec2(mp.x - origin.x, mp.y - origin.y);
        bool can_paste = s_has_clipboard;
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addColor"))) {
            push_undo();
            Node n; n.id = s_g.next_id++; n.type = NT_COLOR; n.pos = local;
            s_g.nodes.push_back(n);
        }
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addFloat"))) {
            push_undo();
            Node n; n.id = s_g.next_id++; n.type = NT_FLOAT; n.pos = local;
            s_g.nodes.push_back(n);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addTextureSampler"))) {
            push_undo();
            Node n; n.id = s_g.next_id++; n.type = NT_TEXTURE; n.pos = local;
            s_g.nodes.push_back(n);
        }
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addMulColor"))) {
            push_undo();
            Node n; n.id = s_g.next_id++; n.type = NT_MUL_C; n.pos = local;
            s_g.nodes.push_back(n);
        }
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addMulFloat"))) {
            push_undo();
            Node n; n.id = s_g.next_id++; n.type = NT_MUL_F; n.pos = local;
            s_g.nodes.push_back(n);
        }
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.addAddFloat"))) {
            push_undo();
            Node n; n.id = s_g.next_id++; n.type = NT_ADD_F; n.pos = local;
            s_g.nodes.push_back(n);
        }
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.copy"),  "Ctrl+C", false,
                            !s_g.selected.empty())) copy_selection();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.paste"), "Ctrl+V", false, can_paste))
            paste_clipboard(local);
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.del"), "Del", false,
                            !s_g.selected.empty())) delete_selected();
        ImGui::Separator();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.undo"), "Ctrl+Z", false, !s_undo.empty())) do_undo();
        if (ImGui::MenuItem(jce_editor_i18n("materialGraph.menu.redo"), "Ctrl+Y", false, !s_redo.empty())) do_redo();
        ImGui::EndPopup();
    }

    /* Quick-add palette popup. */
    if (ImGui::BeginPopup("matgraph_quick_add")) {
        ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.popup.quickAdd"));
        ImGui::Separator();
        if (g_qa_focus_request) {
            ImGui::SetKeyboardFocusHere();
            g_qa_focus_request = 0;
        }
        ImGui::SetNextItemWidth(220.0f);
        bool committed_via_text =
            ImGui::InputText("##qa_filter", g_qa_filter, sizeof(g_qa_filter),
                             ImGuiInputTextFlags_EnterReturnsTrue);

        /* Build filtered list. */
        int  visible[32];
        int  visible_n = 0;
        char fbuf[64];
        std::snprintf(fbuf, sizeof(fbuf), "%s", g_qa_filter);
        for (int i = 0; fbuf[i]; ++i)
            fbuf[i] = (char)std::tolower((unsigned char)fbuf[i]);
        for (int i = 0; i < kAddableCount; ++i) {
            const char *lbl = node_label(kAddable[i]);
            char lbuf[64];
            std::snprintf(lbuf, sizeof(lbuf), "%s", lbl);
            for (int k = 0; lbuf[k]; ++k)
                lbuf[k] = (char)std::tolower((unsigned char)lbuf[k]);
            if (!fbuf[0] || std::strstr(lbuf, fbuf))
                visible[visible_n++] = i;
        }
        if (visible_n == 0) g_qa_highlight = 0;
        else if (g_qa_highlight >= visible_n) g_qa_highlight = visible_n - 1;
        else if (g_qa_highlight < 0) g_qa_highlight = 0;

        /* Arrow nav. */
        if (ImGui::IsKeyPressed(ImGuiKey_DownArrow) && visible_n > 0)
            g_qa_highlight = (g_qa_highlight + 1) % visible_n;
        if (ImGui::IsKeyPressed(ImGuiKey_UpArrow) && visible_n > 0)
            g_qa_highlight = (g_qa_highlight - 1 + visible_n) % visible_n;

        ImGui::BeginChild("##qa_list", ImVec2(220.0f, 160.0f), true);
        for (int j = 0; j < visible_n; ++j) {
            int        i   = visible[j];
            NodeType   t   = kAddable[i];
            const char *lbl = node_label(t);
            bool selected = (j == g_qa_highlight);
            if (ImGui::Selectable(lbl, selected)) {
                add_node_at(t, g_qa_local);
                ImGui::CloseCurrentPopup();
            }
            if (selected && ImGui::IsKeyPressed(ImGuiKey_None, false)) {
                /* keep selected visible */
            }
        }
        ImGui::EndChild();

        if (committed_via_text && visible_n > 0) {
            add_node_at(kAddable[visible[g_qa_highlight]], g_qa_local);
            ImGui::CloseCurrentPopup();
        }
        if (ImGui::IsKeyPressed(ImGuiKey_Escape))
            ImGui::CloseCurrentPopup();
        ImGui::EndPopup();
    }
}

/* Preview pane: stylised lit sphere driven by the last-compiled PBR Output
 * + a compile-feedback log. Lives in a left-hand column inside the
 * material graph window so the user sees node graph + result side by
 * side, mimicking Unity's Shader Graph "Master Preview". */
void draw_preview_sphere(ImVec2 size)
{
    ImDrawList *dl = ImGui::GetWindowDrawList();
    ImVec2 p0 = ImGui::GetCursorScreenPos();
    ImVec2 p1 = ImVec2(p0.x + size.x, p0.y + size.y);
    dl->AddRectFilled(p0, p1, jce_theme::canvas_bg(), 6.0f);

    ImVec2 c = ImVec2(p0.x + size.x * 0.5f, p0.y + size.y * 0.5f);
    float  r = (size.x < size.y ? size.x : size.y) * 0.42f;

    /* Resolve color: use compiled output when available, otherwise white. */
    float bc[4] = { s_prev.base_color[0], s_prev.base_color[1],
                    s_prev.base_color[2], s_prev.base_color[3] };
    float em[3] = { s_prev.emissive[0], s_prev.emissive[1], s_prev.emissive[2] };
    float met = s_prev.metallic;
    float rough = s_prev.roughness;
    if (!s_prev.valid) { bc[0]=bc[1]=bc[2]=0.7f; bc[3]=1.0f; met=0; rough=0.5f; }

    /* Layered shaded disk: dark rim -> base -> highlight. Highlight size
     * shrinks with roughness; metallic biases tint toward base color. */
    auto col = [](float r, float g, float b, float a) {
        auto C = [](float x){ x = x < 0 ? 0 : (x > 1 ? 1 : x);
                              return (int)(x * 255.0f + 0.5f); };
        return IM_COL32(C(r), C(g), C(b), C(a));
    };
    /* Background ambient ring */
    dl->AddCircleFilled(c, r, col(bc[0]*0.15f, bc[1]*0.15f, bc[2]*0.15f, 1.0f), 64);
    /* Diffuse body */
    dl->AddCircleFilled(c, r * 0.94f,
        col(bc[0]*0.55f + em[0]*0.5f,
            bc[1]*0.55f + em[1]*0.5f,
            bc[2]*0.55f + em[2]*0.5f, 1.0f), 64);
    /* Highlight (top-left) — radius shrinks with roughness, intensity rises with metallic */
    float hi_r = r * (0.35f - rough * 0.25f);
    if (hi_r < 4.0f) hi_r = 4.0f;
    ImVec2 hi = ImVec2(c.x - r * 0.35f, c.y - r * 0.35f);
    float spec = 0.7f + 0.3f * met;
    dl->AddCircleFilled(hi, hi_r,
        col(bc[0]*0.4f + spec, bc[1]*0.4f + spec, bc[2]*0.4f + spec, 1.0f), 32);

    /* Caption strip. */
    char info[128];
    std::snprintf(info, sizeof(info), "M=%.2f  R=%.2f", met, rough);
    ImVec2 ts = ImGui::CalcTextSize(info);
    dl->AddText(ImVec2(p1.x - ts.x - 6.0f, p1.y - ts.y - 4.0f),
                jce_theme::text_secondary(), info);

    ImGui::Dummy(size);
}

void draw_preview_pane(void)
{
    ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.title"));
    ImGui::Separator();

    /* Sphere */
    ImVec2 avail = ImGui::GetContentRegionAvail();
    float  side = avail.x < 200.0f ? avail.x : 200.0f;
    if (side < 80.0f) side = 80.0f;
    draw_preview_sphere(ImVec2(side, side));

    ImGui::Spacing();

    /* Resolved values readout */
    ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.values"));
    if (!s_prev.valid) {
        ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.preview.notYetCompiled"));
    } else {
        ImGui::ColorButton("##bc", ImVec4(s_prev.base_color[0],
                                          s_prev.base_color[1],
                                          s_prev.base_color[2],
                                          s_prev.base_color[3]),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
        ImGui::SameLine(); ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.baseColor"));
        ImGui::Text(jce_editor_i18n("materialGraph.preview.metallicFmt"), s_prev.metallic);
        ImGui::Text(jce_editor_i18n("materialGraph.preview.roughnessFmt"), s_prev.roughness);
        ImGui::ColorButton("##em", ImVec4(s_prev.emissive[0],
                                          s_prev.emissive[1],
                                          s_prev.emissive[2], 1.0f),
                           ImGuiColorEditFlags_NoTooltip, ImVec2(20, 20));
        ImGui::SameLine(); ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.emissive"));
        if (s_prev.base_tex[0]) ImGui::TextDisabled(jce_editor_i18n("materialGraph.preview.bcTexFmt"), s_prev.base_tex);
        if (s_prev.mr_tex[0])   ImGui::TextDisabled(jce_editor_i18n("materialGraph.preview.mrTexFmt"), s_prev.mr_tex);
        if (s_prev.emis_tex[0]) ImGui::TextDisabled(jce_editor_i18n("materialGraph.preview.emTexFmt"), s_prev.emis_tex);
    }

    ImGui::Spacing();
    ImGui::Separator();

    /* Compile feedback log */
    ImGui::TextUnformatted(jce_editor_i18n("materialGraph.preview.compileLog"));
    /* Compile feedback log — tinted variants of the canvas bg so light
       and dark themes both render legibly. */
    ImU32 bg;
    if (s_log.has_error) {
        bg = jce_theme::is_light() ? IM_COL32(250, 220, 220, 255)
                                   : IM_COL32( 60,  20,  20, 255);
    } else {
        bg = jce_theme::is_light() ? IM_COL32(225, 240, 225, 255)
                                   : IM_COL32( 20,  30,  24, 255);
    }
    ImGui::PushStyleColor(ImGuiCol_ChildBg, bg);
    ImGui::BeginChild("##cmplog", ImVec2(0, 0), true,
                      ImGuiWindowFlags_HorizontalScrollbar);
    if (s_log.text[0])
        ImGui::TextUnformatted(s_log.text);
    else
        ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.preview.logEmpty"));
    ImGui::EndChild();
    ImGui::PopStyleColor();
}

void draw_content(void)
{
    ensure_output_node();

    ImGui::InputText(jce_editor_i18n("materialGraph.field.file"), s_g.path, sizeof(s_g.path));
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.saveGraph")) && s_g.path[0]) save_graph(s_g.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.loadGraph")) && s_g.path[0]) load_graph(s_g.path);
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.compile"))) compile_to_material();
    ImGui::SameLine();
    if (ImGui::Button(jce_editor_i18n("materialGraph.button.import"))) {
        if (s_g.path[0]) {
            char p[260];
            std::snprintf(p, sizeof(p), "%s", s_g.path);
            char *dot = std::strrchr(p, '.');
            if (dot) *dot = '\0';
            char *dot2 = std::strrchr(p, '.');
            if (dot2 && std::strcmp(dot2, ".matgraph") == 0) *dot2 = '\0';
            std::strncat(p, ".mat.json", sizeof(p) - std::strlen(p) - 1);
            import_from_material(p);
        }
    }
    ImGui::SameLine();
    ImGui::TextDisabled("%s", jce_editor_i18n("materialGraph.hint.import"));
    ImGui::Separator();

    /* Two-column layout: preview/log on the left, node canvas on the right. */
    ImGui::BeginChild("##matgraph_preview", ImVec2(220.0f, 0), true);
    draw_preview_pane();
    ImGui::EndChild();
    ImGui::SameLine();
    ImGui::BeginChild("##matgraph_canvas", ImVec2(0, 0), false);
    draw_canvas();
    ImGui::EndChild();
}

} /* namespace */

extern "C" void jce_editor_panel_material_graph(void)
{
    bool *vis = jce_editor_panel_visible_ptr(JCE_PANEL_MATERIAL_GRAPH);
    if (!vis || !*vis) return;
    char _wt[96];
    snprintf(_wt, sizeof(_wt), "%s###jce_material_graph", jce_editor_i18n("materialGraph.title"));
    if (ImGui::Begin(_wt, vis)) {
        draw_content();
    }
    ImGui::End();
}
