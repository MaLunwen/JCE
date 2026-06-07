/*
 * jce_shadergraph_graph.h — pure-model graph operations.
 *
 * Side-effect-free helpers operating on JceShaderGraph.  Undo/redo
 * and clipboard are deliberately NOT here — they're editor policy
 * and live in the panel.
 */

#pragma once

#include "shadergraph/jce_shadergraph_types.h"

namespace jce_sg {

/* Returns NULL if id not found. */
Node *find_node(Graph &g, int id);

/* Looks up an existing link feeding (to_node, to_sock).
 * Writes into *out and returns true on hit. */
bool find_link_into(const Graph &g, int to_node, int to_sock, Link *out);

/* Const overloads — identical lookups against an immutable graph, used by the
 * read-only codegen / typecheck passes (which previously kept private copies). */
const Node *find_node(const Graph &g, int id);
const Link *find_link_into(const Graph &g, int to_node, int to_sock);

/* Pure model: add a node of type `t` at local position `pos`.
 * Caller is responsible for any undo snapshot. */
Node *add_node(Graph &g, NodeType t, Vec2 pos);

/* Ensure the singleton PBR-Output node exists; create at `default_pos`
 * if missing.  Returns the existing or newly-created node. */
Node *ensure_output(Graph &g, Vec2 default_pos);

/* Attempt to connect g.pending_from_{node,sock} → (to_node, to_sock).
 * Validates type compatibility (must match `to_dt`), replaces any
 * existing link into the destination (single-input rule), and clears
 * pending state on completion / failure.
 * Returns true if a new link was committed. */
bool try_make_link(Graph &g, int to_node, int to_sock, DataType to_dt);

/* Remove a single node (and all links touching it) by id.
 * The NT_OUTPUT node cannot be removed (returns false). */
bool remove_node(Graph &g, int id);

/* Remove all links involving any node id in `doomed`. */
void purge_links_touching(Graph &g, const std::set<int> &doomed);

} /* namespace jce_sg */
