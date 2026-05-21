/*
 * material_graph_state.cpp — single-definition site for state.h
 * globals + small panel ops (undo/redo, log, link validation,
 * socket pin geometry).
 */
#include "panels/material_graph/material_graph_state.h"

#include <cstdarg>
#include <cstdio>
#include <cstring>

namespace jce_mgp {

Graph              s_g;
std::vector<Graph> s_undo;
std::vector<Graph> s_redo;
Graph              s_clipboard;
bool               s_has_clipboard = false;
CompileLog         s_log{};
PreviewState       s_prev;

bool   g_qa_open          = false;
char   g_qa_filter[64]    = {0};
ImVec2 g_qa_local;
int    g_qa_focus_request = 0;
int    g_qa_highlight     = 0;

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

void try_make_link(int to_node, int to_sock, DataType to_dt)
{
    /* Pre-validate so push_undo is only paid when a link will
     * actually commit (matches legacy single-file panel timing). */
    if (s_g.pending_from_node < 0) return;
    Node *src = jce_sg::find_node(s_g, s_g.pending_from_node);
    if (!src) { s_g.pending_from_node = -1; return; }
    int sn_count = 0;
    const Socket *sn = jce_sg::sockets_for(src->type, &sn_count);
    if (s_g.pending_from_sock < 0 || s_g.pending_from_sock >= sn_count
        || !sn || sn[s_g.pending_from_sock].dtype != to_dt) {
        s_g.pending_from_node = -1; return;
    }
    push_undo();
    jce_sg::try_make_link(s_g, to_node, to_sock, to_dt);
}

ImVec2 socket_screen_pos(ImVec2 node_origin, ImVec2 node_size,
                         int sock_idx, SocketKind kind)
{
    float row_h = ImGui::GetTextLineHeightWithSpacing();
    float y = node_origin.y + 28.0f + sock_idx * row_h + row_h * 0.5f;
    float x = (kind == SK_INPUT) ? node_origin.x
                                 : node_origin.x + node_size.x;
    return ImVec2(x, y);
}

} /* namespace jce_mgp */
