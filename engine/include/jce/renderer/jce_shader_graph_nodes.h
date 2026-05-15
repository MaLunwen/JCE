/*
 * jce_shader_graph_nodes.h  Built-in node prototypes.
 *
 * A small library of factory helpers that wire JceShaderSlot arrays
 * for the 12 common shader-graph node kinds.  Use these from the
 * editor's "Create Node" menu so every freshly-added node arrives
 * with the right slot layout — callers don't repeat slot lists.
 *
 * Naming follows Unity Shader Graph's nomenclature where reasonable
 * ("Sample Texture 2D", "Multiply", "Lerp").  Each helper returns
 * the new node id (or JCE_SHADER_NODE_INVALID).
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_SHADER_GRAPH_NODES_H
#define JCE_SHADER_GRAPH_NODES_H

#include <jce/renderer/jce_shader_graph.h>

JCE_EXTERN_C_BEGIN

/* Master nodes — terminal sinks for the graph. */
JCE_API uint16_t jce_shader_node_pbr_master  (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_unlit_master(JceShaderGraph *g, float x, float y);

/* Input nodes. */
JCE_API uint16_t jce_shader_node_uv        (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_normal_ws (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_time      (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_const_float(JceShaderGraph *g, float x, float y,
                                              float value);
JCE_API uint16_t jce_shader_node_const_vec4(JceShaderGraph *g, float x, float y,
                                              float a, float b, float c, float d);

/* Texture / sampling nodes. */
JCE_API uint16_t jce_shader_node_sample2d  (JceShaderGraph *g, float x, float y);

/* Math nodes. */
JCE_API uint16_t jce_shader_node_multiply  (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_add       (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_lerp      (JceShaderGraph *g, float x, float y);
JCE_API uint16_t jce_shader_node_saturate  (JceShaderGraph *g, float x, float y);

/* ── JSON serializer ─────────────────────────────────────────── */

/* Persist `g` to `path`.  Returns true on success.  Layout is a
 * compact array of {nodes: [...], edges: [...], master: id}. */
JCE_API bool jce_shader_graph_save_json(const JceShaderGraph *g,
                                         const char *path);

/* Load `path` into `g` (clears `g` first).  Returns true on success.
 * Format-version mismatch silently truncates fields it doesn't know,
 * matching the project's other serializers. */
JCE_API bool jce_shader_graph_load_json(JceShaderGraph *g,
                                         const char *path);

JCE_EXTERN_C_END

#endif /* JCE_SHADER_GRAPH_NODES_H */
