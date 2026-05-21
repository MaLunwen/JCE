/*
 * jce_shadergraph_registry.h — declarative node metadata lookup.
 *
 * Replaces the per-call switch in legacy node_sockets() / node_label().
 * Phase C will fill `glsl_snippet` with HLSL/GLSL expression templates
 * driving .sc code generation; Phase A keeps it NULL.
 */

#pragma once

#include "shadergraph/jce_shadergraph_types.h"

namespace jce_sg {

/* Per-node-type metadata.  All pointers are static-lifetime — safe
 * to cache the returned `JceShaderNodeMeta*` indefinitely. */
struct NodeMeta {
    NodeType      type;
    const char   *label;        /* human-readable name (already-localised UI labels live elsewhere) */
    const Socket *sockets;      /* socket array */
    int           socket_count;
    const char   *glsl_snippet; /* Phase C codegen template, NULL in Phase A */
};

/* Returns NULL when type is unknown. */
const NodeMeta *meta_for(NodeType t);

/* Convenience wrappers. */
const Socket *sockets_for(NodeType t, int *out_count);
const char   *label_for(NodeType t);

/* User-addable type list (excludes NT_OUTPUT — graphs have exactly one). */
const NodeType *addable_types(int *out_count);

} /* namespace jce_sg */
