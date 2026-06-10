/*
 * material_graph_state.h — shared state + helpers for the split
 * Material Node Editor panel TUs.
 *
 * Phase B extraction (P2-(2)): this header is the single source of
 * truth for cross-TU symbols (graph singleton, undo/redo stacks,
 * clipboard, compile log, preview state, quick-add palette
 * globals).  Member function-style helpers (find_node, add_node_at,
 * etc.) are inline wrappers over editor/src/shadergraph/ so all
 * panel TUs share one set of names with no extra dispatch cost.
 *
 * Scope: editor-only.  No engine code includes this.
 */
#pragma once

#include "shadergraph/jce_shadergraph_types.h"
#include "shadergraph/jce_shadergraph_registry.h"
#include "shadergraph/jce_shadergraph_graph.h"
#include "shadergraph/jce_shadergraph_io.h"

#include <jce/renderer/jce_gfx_types.h>     /* JceShaderHandle (POD: uint16_t) */
#include <jce/tools/jce_imgui.hpp>

#include <vector>

namespace jce_mgp {

/* Convenience aliases so callers can keep using unqualified names
 * (mirrors the legacy single-file panel). */
using jce_sg::NodeType;
using jce_sg::SocketKind;
using jce_sg::DataType;
using jce_sg::Socket;
using jce_sg::Node;
using jce_sg::Link;
using jce_sg::Graph;

using jce_sg::NT_OUTPUT;
using jce_sg::NT_COLOR;
using jce_sg::NT_FLOAT;
using jce_sg::NT_TEXTURE;
using jce_sg::NT_MUL_C;
using jce_sg::NT_MUL_F;
using jce_sg::NT_ADD_F;
using jce_sg::NT_NORMAL_MAP;
using jce_sg::NT_UV;
using jce_sg::NT_SUB_F;
using jce_sg::NT_LERP_C;
using jce_sg::NT_FRESNEL;

using jce_sg::SK_INPUT;
using jce_sg::SK_OUTPUT;
using jce_sg::DT_FLOAT;
using jce_sg::DT_COLOR;

/* ---- panel-wide value types ---- */

struct CompileLog {
    char text[2048];
    bool has_error;
};

/* Resolved PBR Output for the preview pane.  Plain POD: no bgfx
 * handles, safe to live in a shared header. */
struct PreviewState {
    float           base_color[4] = {1, 1, 1, 1};
    float           metallic      = 0.0f;
    float           roughness     = 0.5f;
    float           emissive[3]   = {0, 0, 0};
    char            base_tex[256] = {0};
    char            mr_tex[256]   = {0};
    char            emis_tex[256] = {0};
    bool            valid         = false;

    /* Phase D: program built by compile_and_bind().  When valid, the
     * preview pane (and any future material binding) renders with this
     * graph-generated shader instead of the default PBR program.
     * JceShaderHandle is a {uint16_t idx} POD — no bgfx headers needed
     * here.  JCE_INVALID_SHADER means "no custom program yet". */
    JceShaderHandle custom_program = { UINT16_MAX };
};

/* ---- shared mutable state (defined once in state.cpp) ---- */

extern Graph              s_g;
extern std::vector<Graph> s_undo;
extern std::vector<Graph> s_redo;
extern Graph              s_clipboard;
extern bool               s_has_clipboard;
extern CompileLog         s_log;
extern PreviewState       s_prev;

/* Quick-add palette state. */
extern bool   g_qa_open;
extern char   g_qa_filter[64];
extern ImVec2 g_qa_local;
extern int    g_qa_focus_request;
extern int    g_qa_highlight;

/* ---- panel-wide operations (defined in state.cpp) ---- */

void push_undo(void);
void do_undo(void);
void do_redo(void);

void log_clear(void);
void log_append(bool err, const char *fmt, ...);

/* try_make_link wraps the model op + push_undo with the same timing
 * the legacy single-file panel had (push only when a link would
 * actually commit). */
void try_make_link(int to_node, int to_sock, DataType to_dt);

/* Canvas helper shared by draw_node + draw_canvas. */
ImVec2 socket_screen_pos(ImVec2 node_origin, ImVec2 node_size,
                         int sock_idx, SocketKind kind);

/* ---- thin wrappers over jce_sg:: (inline, no dispatch cost) ---- */

inline Node *find_node(int id)
{
    return jce_sg::find_node(s_g, id);
}

inline bool find_link_into(int to_node, int to_sock, Link *out)
{
    return jce_sg::find_link_into(s_g, to_node, to_sock, out);
}

inline const Socket *node_sockets(NodeType t, int *out_count)
{
    return jce_sg::sockets_for(t, out_count);
}

inline const char *node_label(NodeType t)
{
    return jce_sg::label_for(t);
}

inline void ensure_output_node(void)
{
    jce_sg::ensure_output(s_g, jce_sg::Vec2{420.0f, 80.0f});
}

inline void add_node_at(NodeType t, ImVec2 local)
{
    push_undo();
    jce_sg::add_node(s_g, t, jce_sg::Vec2{local.x, local.y});
}

inline void save_graph(const char *path) { jce_sg::save(s_g, path); }
inline void load_graph(const char *path) { jce_sg::load(s_g, path); }

inline void import_from_material(const char *path)
{
    push_undo();
    jce_sg::import_from_pbr_material(s_g, path);
}

/* ---- forward decls of drawing / editing entry points ---- */

void eval_socket(int node_id, int sock_idx, float out_color[4],
                 char out_tex[256], int depth);
void compile_to_material(void);
void generate_shader(void);

/* Phase D: full compile-and-bind pipeline.
 *   1. codegen()        — graph -> .sc text + (optional) .sc on disk
 *   2. compile_sc() x2  — vs_pbr.sc + generated fs to bgfx .bin blobs
 *   3. create_program_from_blobs — link into a bgfx program
 *   4. swap s_prev.custom_program (old one destroyed via bgfx defer)
 * All diagnostics land in s_log; on failure s_prev.custom_program
 * keeps its previous value (last-known-good fallback).
 *
 * Async: codegen + path resolution run on the calling (UI) thread, then
 * the two shaderc.exe invocations run on a background worker so the UI
 * never blocks (shaderc can take seconds, with a 30 s hard cap).  The
 * GPU program create + .bin/.mat.json persist happen on the main thread
 * in shader_compile_poll().  Call shader_compile_poll() every frame the
 * panel is alive to pick up a finished compile. */
void compile_and_bind(void);
void shader_compile_poll(void);
bool shader_compile_running(void);

void delete_selected(void);
void copy_selection(void);
void paste_clipboard(ImVec2 paste_origin_local);

void draw_node(Node &n, ImDrawList *dl, ImVec2 origin);
void draw_canvas(void);

void draw_preview_sphere(ImVec2 size);
void draw_preview_pane(void);

} /* namespace jce_mgp */
