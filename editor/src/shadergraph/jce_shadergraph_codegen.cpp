/*
 * jce_shadergraph_codegen.cpp — Graph -> GLSL emission + template splice.
 *
 * Pipeline:
 *   1. topo_sort  : reachable-from-Output dependency order + cycle check.
 *   2. typecheck  : dtype + arity + duplicate-input validation.
 *   3. emit body  : per node, emit one `<type> n<id>_o<sock> = <expr>;`
 *                   line.  Inputs reference upstream variables; unconnected
 *                   inputs fall back to dtype-appropriate constants.
 *   4. emit out   : NT_OUTPUT inputs assigned to mat_* locals required by
 *                   fs_graph_template.sc post-hook code.
 *   5. splice     : replace the region between /JCE_BEGIN_MATERIAL/ and
 *                   /JCE_END_MATERIAL/ in the template with the emitted
 *                   body, then optionally write `fs_<basename>.sc`.
 *
 * Variable naming: `n<id>_o<sock_index>`.  Most nodes have a single
 * output socket and produce a single variable.
 */

#include "shadergraph/jce_shadergraph_codegen.h"

#include "shadergraph/jce_shadergraph_graph.h"
#include "shadergraph/jce_shadergraph_registry.h"
#include "shadergraph/jce_shadergraph_topo.h"
#include "shadergraph/jce_shadergraph_typecheck.h"

#include "io/jce_editor_file_util.h"

#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace jce_sg {

namespace {

constexpr const char *kHookBegin = "/*JCE_BEGIN_MATERIAL*/";
constexpr const char *kHookEnd   = "/*JCE_END_MATERIAL*/";

/* ---------- helpers ---------- */

/* find_node / find_link_into now come from jce_shadergraph_graph.h
 * (const overloads); the private copies were removed to deduplicate. */

/* Remap has five inputs; four was the old ceiling and is why nothing with
 * more than four could be added. */
enum { JCE_SG_MAX_ARGS = 8 };

const char *glsl_type(DataType d)
{
    switch (d) {
        case DT_FLOAT:     return "float";
        case DT_VEC2:      return "vec2";
        case DT_VEC3:      return "vec3";
        case DT_NORMAL:    return "vec3";
        case DT_COLOR:     return "vec4";
        case DT_VEC4:      return "vec4";
        case DT_SAMPLER2D: return "sampler2D";
    }
    return "float";
}

std::string dtype_default(DataType d)
{
    switch (d) {
        case DT_FLOAT:  return "0.0";
        case DT_VEC2:   return "vec2_splat(0.0)";
        case DT_VEC3:
        case DT_NORMAL: return "vec3_splat(0.0)";
        case DT_COLOR:
        case DT_VEC4:   return "vec4(0.0, 0.0, 0.0, 1.0)";
        default:        return "0.0";
    }
}

std::string var_name(int node_id, int sock_index)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "n%d_o%d", node_id, sock_index);
    return buf;
}

/* Return the GLSL expression feeding (to_node, to_sock).
 *
 * An unconnected input takes the SOCKET's own default when it has one, and
 * only falls back to the dtype's zero when it does not.  Zero is the wrong
 * answer often enough that it had already been special-cased once, by dtype,
 * for UVs -- which worked only while every vec2 socket was a UV. */
std::string input_expr(const Graph &g, int to_node, int to_sock,
                       DataType to_dt, const Socket *sock)
{
    const Link *l = find_link_into(g, to_node, to_sock);
    if (l) return var_name(l->from_node, l->from_sock);
    if (sock && sock->default_expr) return sock->default_expr;
    return dtype_default(to_dt);
}

/* Which of the material's maps a Texture node reads.
 *
 * Node::text was serialised and never read, so every Texture node emitted
 * s_albedo and two nodes aimed at different maps sampled the same one.  A
 * SLOT and not a path: a graph program is bound by jce_pbr_material_bind,
 * which binds these five and nothing else -- a per-node file would need a
 * sampler no material fills, which reads whatever the previous draw left on
 * that stage.  An empty or unrecognised value is albedo, which is what every
 * graph authored before this did. */
const char *sampler_for_node(const Node &n)
{
    const char *t = n.text;
    if (!t || !t[0]) return "s_albedo";
    if (std::strcmp(t, "metalRough") == 0) return "s_metalRough";
    if (std::strcmp(t, "normalMap")  == 0) return "s_normalMap";
    if (std::strcmp(t, "ao")         == 0) return "s_ao";
    if (std::strcmp(t, "emissive")   == 0) return "s_emissive";
    return "s_albedo";
}

/* Substitute $0/$1/$2 in `tpl` with `args`.  Up to 4 args supported. */
/* $0..$9 are the node's inputs in socket order; $O is the ORDINAL of the
 * output socket being emitted.
 *
 * $O exists for Split, which has one expression and four outputs.  The
 * alternative was four snippets that differ by one character, or a third
 * special case in emit_node beside the two that are already there -- and a
 * special case is where the next node like it goes wrong. */
std::string substitute(const char *tpl, const std::string args[JCE_SG_MAX_ARGS],
                       int n_args, int out_ordinal)
{
    std::string out;
    for (const char *p = tpl; *p; ) {
        if (p[0] == '$' && p[1] >= '0' && p[1] <= '9') {
            int idx = p[1] - '0';
            if (idx < n_args) out += args[idx];
            else              out += "0.0";
            p += 2;
        } else if (p[0] == '$' && p[1] == 'O') {
            out += (char)('0' + (out_ordinal < 0 ? 0 : out_ordinal));
            p += 2;
        } else {
            out += *p++;
        }
    }
    return out;
}

/* Format a float constant safely (preserve enough precision for shader). */
std::string fmt_float(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6f", v);
    return buf;
}

/* Emit `<type> n<id>_o<out> = <expr>;` lines for one node (non-Output). */
void emit_inner_node(const Graph &g, const Node &n, std::string &body)
{
    const NodeMeta *meta = meta_for(n.type);
    if (!meta) return;

    /* Special-cased constant emitters. */
    if (n.type == NT_COLOR) {
        body += "    vec4 ";
        body += var_name(n.id, 0);
        body += " = vec4(";
        body += fmt_float(n.color[0]); body += ", ";
        body += fmt_float(n.color[1]); body += ", ";
        body += fmt_float(n.color[2]); body += ", ";
        body += fmt_float(n.color[3]); body += ");\n";
        return;
    }
    if (n.type == NT_FLOAT) {
        body += "    float ";
        body += var_name(n.id, 0);
        body += " = ";
        body += fmt_float(n.scalar);
        body += ";\n";
        return;
    }
    if (n.type == NT_UV) {
        /* A REAL uv: tile in color[0..1], offset in color[2..3], applied to
         * the interpolated coordinate.  It used to emit `float = scalar`,
         * which no socket in the graph could consume. */
        body += "    vec2 ";
        body += var_name(n.id, 0);
        body += " = mat_uv * vec2(";
        body += fmt_float(n.color[0]); body += ", ";
        body += fmt_float(n.color[1]); body += ") + vec2(";
        body += fmt_float(n.color[2]); body += ", ";
        body += fmt_float(n.color[3]); body += ");\n";
        return;
    }

    if (!meta->glsl_snippet) return; /* no snippet, no emission */

    /* Gather input args (in input-socket order). */
    std::string args[JCE_SG_MAX_ARGS];
    int n_args = 0;
    for (int s = 0; s < meta->socket_count && n_args < JCE_SG_MAX_ARGS; ++s) {
        if (meta->sockets[s].kind != SK_INPUT) continue;
        args[n_args++] = input_expr(g, n.id, s, meta->sockets[s].dtype,
                                    &meta->sockets[s]);
    }

    /* $SAMPLER, before $0..$9: the sampling nodes name their map per node. */
    std::string tpl = meta->glsl_snippet;
    for (size_t at = tpl.find("$SAMPLER"); at != std::string::npos;
         at = tpl.find("$SAMPLER"))
        tpl.replace(at, 8, sampler_for_node(n));

    /* One line per OUTPUT socket, with $O carrying which one. */
    int out_ordinal = 0;
    for (int s = 0; s < meta->socket_count; ++s) {
        if (meta->sockets[s].kind != SK_OUTPUT) continue;
        body += "    ";
        body += glsl_type(meta->sockets[s].dtype);
        body += " ";
        body += var_name(n.id, s);
        body += " = ";
        body += substitute(tpl.c_str(), args, n_args, out_ordinal);
        body += ";\n";
        ++out_ordinal;
    }
}

/* A float as GLSL source, in its shortest form that is still a float literal.
 *
 * Generated shader source is user-visible (the Shader Inspector shows it), and
 * "vec4(1.000000, 1.000000, 1.000000, 1.000000)" reads as machine output while
 * "vec4(1.0, 1.0, 1.0, 1.0)" reads as code. */
std::string glsl_float(float v)
{
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.6g", (double)v);
    std::string s(buf);
    if (s.find('.') == std::string::npos &&
        s.find('e') == std::string::npos &&
        s.find("inf") == std::string::npos &&
        s.find("nan") == std::string::npos)
        s += ".0";
    return s;
}

/* Emit the four mat_* assignments for the NT_OUTPUT node. */
void emit_output(const Graph &g, const Node &out, std::string &body)
{
    /* Formatted from jce_sg::output_socket_default -- the SAME numbers
     * compile_to_material writes into the .mat.json.  They used to be two
     * lists, and the two disagreed on every slot but the first. */
    char defbuf[4][64];
    for (int slot = 0; slot < 4; ++slot) {
        float d[4];
        output_socket_default(slot, d);
        if (slot == 1 || slot == 2)
            std::snprintf(defbuf[slot], sizeof(defbuf[slot]), "%s", glsl_float(d[0]).c_str());
        else
            std::snprintf(defbuf[slot], sizeof(defbuf[slot]),
                          "vec4(%s, %s, %s, %s)",
                          glsl_float(d[0]).c_str(), glsl_float(d[1]).c_str(),
                          glsl_float(d[2]).c_str(), glsl_float(d[3]).c_str());
    }
    const char *defaults[4] = { defbuf[0], defbuf[1], defbuf[2], defbuf[3] };
    const char *targets[4] = {
        "mat_base_color",
        "mat_metallic",
        "mat_roughness",
        "mat_emissive",
    };

    auto pick = [&](int slot) -> std::string {
        const Link *l = find_link_into(g, out.id, slot);
        if (!l) return defaults[slot];
        return var_name(l->from_node, l->from_sock);
    };

    /* mat_normal_ts, from the Normal socket when something drives it.
     *
     * This line used to be an unconditional `vec3(0,0,1)` with no case that
     * ever read a link, which is why the Normal Map node could sample
     * s_normalMap perfectly and change nothing: there was no reader.  The
     * default stays (0,0,1) -- the identity tangent-space normal -- so a
     * graph that does not drive it shades exactly as before. */
    {
        const Link *ln = find_link_into(g, out.id, 4);
        body += "    vec3 mat_normal_ts = ";
        if (ln) {
            body += var_name(ln->from_node, ln->from_sock);
        } else {
            float d[4];
            output_socket_default(4, d);
            char nb[64];
            std::snprintf(nb, sizeof(nb), "vec3(%s, %s, %s)",
                          glsl_float(d[0]).c_str(), glsl_float(d[1]).c_str(),
                          glsl_float(d[2]).c_str());
            body += nb;
        }
        body += ";\n";
    }

    /* base_color : direct vec4. */
    body += "    vec4 ";
    body += targets[0];
    body += " = ";
    body += pick(0);
    body += ";\n";

    /* metallic / roughness : float; if upstream is vec4, take .r. */
    for (int slot = 1; slot <= 2; ++slot) {
        std::string expr = pick(slot);
        body += "    float ";
        body += targets[slot];
        body += " = ";
        body += expr;
        body += ";\n";
    }
    /* Clamp roughness so the lighting term in the template stays sane. */
    body += "    mat_roughness = clamp(mat_roughness, 0.04, 1.0);\n";

    /* emissive : template needs vec3; upstream is vec4 -> take .rgb. */
    std::string em = pick(3);
    body += "    vec3 ";
    body += targets[3];
    body += " = (";
    body += em;
    body += ").rgb;\n";
}

/* Load entire text file into `out`. */
bool read_file(const char *path, std::string &out)
{
    size_t sz = 0;
    char *buf = (char *)ed_read_file(path, &sz);
    if (!buf) return false;
    out.assign(buf, sz);
    ED_FREE(buf);
    return true;
}

bool write_file(const char *path, const std::string &text)
{
    return ed_write_file(path, text.data(), text.size());
}

/* Splice `body` into `tpl` between hook markers. Returns false if
 * either marker is missing or out-of-order. */
bool splice_hook(const std::string &tpl, const std::string &body,
                 std::string &out)
{
    size_t b = tpl.find(kHookBegin);
    size_t e = tpl.find(kHookEnd);
    if (b == std::string::npos || e == std::string::npos || e <= b) return false;
    size_t b_end = b + std::strlen(kHookBegin);
    out.clear();
    out.reserve(tpl.size() + body.size());
    out.append(tpl, 0, b_end);
    out.push_back('\n');
    out.append(body);
    out.append("    "); /* indent matches surrounding template */
    out.append(tpl, e, tpl.size() - e);
    return true;
}

void push_diag(std::vector<CodegenDiag> &dst, int node_id, std::string msg)
{
    CodegenDiag d;
    d.node_id = node_id;
    d.message = std::move(msg);
    dst.push_back(std::move(d));
}

} /* anonymous namespace */

CodegenResult codegen(const Graph &g,
                      const char *template_path,
                      const char *out_basename,
                      const char *out_dir)
{
    CodegenResult r;

    if (!template_path || !out_basename) {
        push_diag(r.errors, -1, "codegen: template_path/out_basename required.");
        return r;
    }

    /* Type check first (cheap; fails fast on user errors). */
    TypeCheckResult tc = typecheck(g);
    for (const TypeDiag &d : tc.errors)
        push_diag(r.errors, d.node_id, d.message);
    for (const TypeDiag &d : tc.warnings)
        push_diag(r.warnings, d.node_id, d.message);
    if (!tc.ok()) return r;

    /* Topological order. */
    TopoResult topo = topo_sort(g);
    if (!topo.has_output) {
        push_diag(r.errors, -1, "codegen: graph has no PBR Output node.");
        return r;
    }
    if (topo.cycle_node >= 0) {
        char buf[96];
        std::snprintf(buf, sizeof(buf),
            "codegen: cycle detected at node %d.", topo.cycle_node);
        push_diag(r.errors, topo.cycle_node, buf);
        return r;
    }

    /* Emit body. */
    std::string body;
    body.reserve(1024);
    for (int id : topo.order) {
        const Node *n = find_node(g, id);
        if (!n) continue;
        if (n->type == NT_OUTPUT) continue;
        emit_inner_node(g, *n, body);
    }
    const Node *out_node = find_node(g, topo.output_id);
    if (!out_node) {
        push_diag(r.errors, -1, "codegen: Output node vanished after topo.");
        return r;
    }
    emit_output(g, *out_node, body);

    /* Load + splice template. */
    std::string tpl;
    if (!read_file(template_path, tpl)) {
        char buf[256];
        std::snprintf(buf, sizeof(buf),
            "codegen: cannot read template '%s'.", template_path);
        push_diag(r.errors, -1, buf);
        return r;
    }
    if (!splice_hook(tpl, body, r.source)) {
        push_diag(r.errors, -1,
            "codegen: template missing /*JCE_BEGIN_MATERIAL*/ or /*JCE_END_MATERIAL*/.");
        return r;
    }

    /* Optionally write to disk. */
    if (out_dir && out_dir[0] != '\0') {
        char path[1024];
        std::snprintf(path, sizeof(path), "%s/fs_%s.sc", out_dir, out_basename);
        if (!write_file(path, r.source)) {
            char buf[256];
            std::snprintf(buf, sizeof(buf),
                "codegen: failed to write '%s'.", path);
            push_diag(r.errors, -1, buf);
            return r;
        }
        r.out_path = path;
    }

    r.ok = true;
    return r;
}

} /* namespace jce_sg */
