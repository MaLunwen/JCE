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

/* Return the GLSL expression feeding (to_node, to_sock). */
std::string input_expr(const Graph &g, int to_node, int to_sock, DataType to_dt)
{
    const Link *l = find_link_into(g, to_node, to_sock);
    if (!l) return dtype_default(to_dt);
    return var_name(l->from_node, l->from_sock);
}

/* Substitute $0/$1/$2 in `tpl` with `args`.  Up to 4 args supported. */
std::string substitute(const char *tpl, const std::string args[4], int n_args)
{
    std::string out;
    for (const char *p = tpl; *p; ) {
        if (p[0] == '$' && p[1] >= '0' && p[1] <= '9') {
            int idx = p[1] - '0';
            if (idx < n_args) out += args[idx];
            else              out += "0.0";
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
    if (n.type == NT_FLOAT || n.type == NT_UV) {
        /* NT_UV output is declared DT_FLOAT in the registry (tiling
         * factor) so it shares NT_FLOAT's constant emission. */
        body += "    float ";
        body += var_name(n.id, 0);
        body += " = ";
        body += fmt_float(n.scalar);
        body += ";\n";
        return;
    }

    if (!meta->glsl_snippet) return; /* no snippet, no emission */

    /* Gather input args (in input-socket order). */
    std::string args[4];
    int n_args = 0;
    for (int s = 0; s < meta->socket_count && n_args < 4; ++s) {
        if (meta->sockets[s].kind != SK_INPUT) continue;
        args[n_args++] = input_expr(g, n.id, s, meta->sockets[s].dtype);
    }

    /* Find the (first) output socket; emit for each output socket. */
    for (int s = 0; s < meta->socket_count; ++s) {
        if (meta->sockets[s].kind != SK_OUTPUT) continue;
        body += "    ";
        body += glsl_type(meta->sockets[s].dtype);
        body += " ";
        body += var_name(n.id, s);
        body += " = ";
        body += substitute(meta->glsl_snippet, args, n_args);
        body += ";\n";
    }
}

/* Emit the four mat_* assignments for the NT_OUTPUT node. */
void emit_output(const Graph &g, const Node &out, std::string &body)
{
    /* Defaults match the un-substituted template body. */
    const char *defaults[4] = {
        "vec4(1.0, 1.0, 1.0, 1.0)",  /* BaseColor */
        "0.0",                       /* Metallic  */
        "0.5",                       /* Roughness */
        "vec4(0.0, 0.0, 0.0, 1.0)",  /* Emissive  */
    };
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

    /* Always declare mat_normal_ts (template main() consumes it). */
    body += "    vec3 mat_normal_ts = vec3(0.0, 0.0, 1.0);\n";

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
