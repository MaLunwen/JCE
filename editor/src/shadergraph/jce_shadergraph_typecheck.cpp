/*
 * jce_shadergraph_typecheck.cpp — link validation pass.
 *
 * Checks performed:
 *   1. Both endpoints of every link resolve to a real node + socket index.
 *   2. Producer socket is SK_OUTPUT and consumer socket is SK_INPUT.
 *   3. Producer dtype matches consumer dtype (no implicit conversion in
 *      the MVP — codegen would otherwise emit syntactically invalid GLSL).
 *   4. At most one link feeds each (consumer_node, consumer_socket) pair.
 *   5. Exactly one NT_OUTPUT node exists.
 *
 * Warnings (non-fatal):
 *   - NT_OUTPUT input sockets that are not connected (codegen uses defaults).
 */

#include "shadergraph/jce_shadergraph_typecheck.h"

#include "shadergraph/jce_shadergraph_graph.h"
#include "shadergraph/jce_shadergraph_registry.h"

#include <cstdio>
#include <unordered_set>

namespace jce_sg {

namespace {

/* find_node now comes from jce_shadergraph_graph.h (const overload). */

/* Two dtypes that compile to the same GLSL type connect.
 *
 * DT_COLOR and DT_VEC4 are both `vec4`, and DT_NORMAL is DT_VEC3 under
 * another name; the enum distinguishes them so a socket can SAY what it
 * carries, which is a fact about vocabulary, not about types.  Refusing the
 * link taught the author that "Color" and "Vector4" are different things to
 * the compiler, which they are not, and made a Combine node unable to feed a
 * BaseColor.
 *
 * Everything else still requires exact equality: this is not implicit
 * conversion, and a float into a vec3 is still an error rather than a silent
 * splat -- the author should see which wire is wrong. */
bool dtype_connectable(DataType from, DataType to)
{
    if (from == to) return true;
    const bool a4 = (from == DT_COLOR || from == DT_VEC4);
    const bool b4 = (to   == DT_COLOR || to   == DT_VEC4);
    if (a4 && b4) return true;
    const bool a3 = (from == DT_VEC3 || from == DT_NORMAL);
    const bool b3 = (to   == DT_VEC3 || to   == DT_NORMAL);
    return a3 && b3;
}

TypeDiag make_msg(int link, int node, const char *msg)
{
    TypeDiag d;
    d.link_index = link;
    d.node_id    = node;
    d.message    = msg;
    return d;
}

#define TC_DIAG(LIST, LINK, NODE, ...) do {                          \
    char _tcbuf[256];                                                \
    std::snprintf(_tcbuf, sizeof(_tcbuf), __VA_ARGS__);              \
    (LIST).push_back(make_msg((LINK), (NODE), _tcbuf));              \
} while (0)

} /* anonymous namespace */

TypeCheckResult typecheck(const Graph &g)
{
    TypeCheckResult r;

    /* Output node count. */
    int output_count = 0;
    int output_id    = -1;
    for (const Node &n : g.nodes) {
        if (n.type == NT_OUTPUT) {
            ++output_count;
            output_id = n.id;
        }
    }
    if (output_count == 0) {
        r.errors.push_back(make_msg(-1, -1, "Graph has no PBR Output node."));
        return r; /* no Output: skip per-link checks (likely all dangling) */
    }
    if (output_count > 1) {
        TC_DIAG(r.errors, -1, -1,
            "Graph has %d PBR Output nodes; exactly one is required.",
            output_count);
    }

    /* Per-consumer-socket: track which links land there. */
    auto slot_key = [](int node, int sock) -> long long {
        return ((long long)node << 32) | (unsigned)sock;
    };
    std::unordered_set<long long> claimed;

    for (size_t i = 0; i < g.links.size(); ++i) {
        const Link &l = g.links[i];

        const Node *src = find_node(g, l.from_node);
        const Node *dst = find_node(g, l.to_node);
        if (!src || !dst) {
            TC_DIAG(r.errors, (int)i, -1,
                "Link references missing node (link #%d).", (int)i);
            continue;
        }

        int n_src_sock = 0, n_dst_sock = 0;
        const Socket *src_socks = sockets_for(src->type, &n_src_sock);
        const Socket *dst_socks = sockets_for(dst->type, &n_dst_sock);
        if (!src_socks || l.from_sock < 0 || l.from_sock >= n_src_sock) {
            TC_DIAG(r.errors, (int)i, src->id,
                "Link source socket %d out of range on node %d.",
                l.from_sock, src->id);
            continue;
        }
        if (!dst_socks || l.to_sock < 0 || l.to_sock >= n_dst_sock) {
            TC_DIAG(r.errors, (int)i, dst->id,
                "Link target socket %d out of range on node %d.",
                l.to_sock, dst->id);
            continue;
        }

        const Socket &ss = src_socks[l.from_sock];
        const Socket &ds = dst_socks[l.to_sock];
        if (ss.kind != SK_OUTPUT) {
            TC_DIAG(r.errors, (int)i, src->id,
                "Link source must be an output socket (node %d).",
                src->id);
            continue;
        }
        if (ds.kind != SK_INPUT) {
            TC_DIAG(r.errors, (int)i, dst->id,
                "Link target must be an input socket (node %d).",
                dst->id);
            continue;
        }
        if (!dtype_connectable(ss.dtype, ds.dtype)) {
            TC_DIAG(r.errors, (int)i, dst->id,
                "Type mismatch: producer dtype %d -> consumer dtype %d.",
                (int)ss.dtype, (int)ds.dtype);
            continue;
        }

        long long k = slot_key(l.to_node, l.to_sock);
        if (!claimed.insert(k).second) {
            TC_DIAG(r.errors, (int)i, dst->id,
                "Duplicate link into node %d socket %d (single-input rule).",
                dst->id, l.to_sock);
            continue;
        }
    }

    /* Warn on unconnected Output inputs. */
    if (output_id >= 0) {
        const Node *out = find_node(g, output_id);
        if (out) {
            int n_out_socks = 0;
            const Socket *out_socks = sockets_for(out->type, &n_out_socks);
            for (int s = 0; s < n_out_socks; ++s) {
                if (out_socks[s].kind != SK_INPUT) continue;
                long long k = slot_key(out->id, s);
                if (claimed.find(k) == claimed.end()) {
                    TC_DIAG(r.warnings, -1, out->id,
                        "Output socket %d (\"%s\") is unconnected; default used.",
                        s, out_socks[s].name);
                }
            }
        }
    }

    return r;
}

} /* namespace jce_sg */
