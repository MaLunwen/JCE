/*
 * jce_shadergraph_io.h — graph serialisation.
 *
 * JSON schema is the same one the legacy panel emitted:
 *   { "nextId": int, "nodes": [...], "links": [...] }
 * Backward compatible: missing fields fall back to defaults, new
 * runtime-only fields (scroll/selection/...) are not persisted.
 */

#pragma once

#include "shadergraph/jce_shadergraph_types.h"

namespace jce_sg {

/* Returns true on success.  Logs to jce_editor_console_log* on failure. */
bool save(const Graph &g, const char *path);

/* Returns true on success.  Mutates `g`: replaces nodes/links/next_id,
 * resets all runtime/UI state.  Calls ensure_output() before returning
 * so the user always sees a valid PBR Output. */
bool load(Graph &g, const char *path);

/* Bootstrap a fresh graph from an existing PBR material on disk.
 * Creates Texture / Color / Float nodes wired into a single PBR Output.
 * Used by the panel's "Import .mat.json" button. */
bool import_from_pbr_material(Graph &g, const char *mat_json_path);

} /* namespace jce_sg */
