/*
 * jce_shader_graph.h  Node-based shader authoring DAG.
 *
 * Data-layer foundation for a Unity Shader Graph-style authoring
 * surface: typed nodes connected through typed slot edges.  The
 * graph is *just* a serializable model — codegen / bgfx submission
 * lives in a downstream module (B17.8 jce_shader_graph_eval).
 *
 * Concepts
 *   JceShaderGraph         — top-level container of nodes + edges
 *   JceShaderNode          — one operation (sample, multiply, lerp …)
 *   JceShaderSlot          — typed input/output port on a node
 *   JceShaderEdge          — directed connection slot→slot
 *
 * Slot types match Unity Shader Graph's primitive set: float, vec2,
 * vec3, vec4, texture2D, samplerState, bool, int.  Implicit promotion
 * (float→vec3 broadcast) handled in eval, not in the model.
 *
 * Limits
 *   - 256 nodes per graph
 *   - 8 slots per node (4 in + 4 out is typical)
 *   - 512 edges per graph
 * These are bounded so the model is a single allocation; if you need
 * more, allocate a second graph and link them via subgraph nodes
 * (subgraph encoding TBD post-B11).
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_SHADER_GRAPH_H
#define JCE_SHADER_GRAPH_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_SHADER_GRAPH_MAX_NODES 256
#define JCE_SHADER_GRAPH_MAX_EDGES 512
#define JCE_SHADER_NODE_MAX_SLOTS  8
#define JCE_SHADER_NODE_TYPE_LEN   32
#define JCE_SHADER_SLOT_NAME_LEN   24

typedef enum {
    JCE_SHADER_SLOT_FLOAT   = 0,
    JCE_SHADER_SLOT_VEC2    = 1,
    JCE_SHADER_SLOT_VEC3    = 2,
    JCE_SHADER_SLOT_VEC4    = 3,
    JCE_SHADER_SLOT_TEX2D   = 4,
    JCE_SHADER_SLOT_SAMPLER = 5,
    JCE_SHADER_SLOT_BOOL    = 6,
    JCE_SHADER_SLOT_INT     = 7,
    JCE_SHADER_SLOT__COUNT
} JceShaderSlotType;

typedef enum {
    JCE_SHADER_SLOT_DIR_IN  = 0,
    JCE_SHADER_SLOT_DIR_OUT = 1,
} JceShaderSlotDir;

/* Sentinel for unset / invalid ids. */
#define JCE_SHADER_NODE_INVALID 0xFFFFu
#define JCE_SHADER_EDGE_INVALID 0xFFFFu

typedef struct {
    char              name[JCE_SHADER_SLOT_NAME_LEN];
    JceShaderSlotType type;
    JceShaderSlotDir  dir;
    /* Inline default for unconnected input slots; ignored for output
     * slots.  Components beyond `type`'s arity are unused. */
    float             default_value[4];
    bool              active;
} JceShaderSlot;

typedef struct {
    char           type_name[JCE_SHADER_NODE_TYPE_LEN]; /* "multiply", "sample2d" … */
    JceShaderSlot  slots[JCE_SHADER_NODE_MAX_SLOTS];
    /* Position in the editor canvas (pixels).  Persisted with the
     * graph so layout is stable across loads. */
    float          canvas_x;
    float          canvas_y;
    /* Per-node arbitrary parameter (e.g. constant value, texture
     * asset id).  Interpretation is node-type-specific. */
    char           param[64];
    bool           active;
} JceShaderNode;

typedef struct {
    uint16_t src_node;
    uint8_t  src_slot;
    uint16_t dst_node;
    uint8_t  dst_slot;
    bool     active;
} JceShaderEdge;

typedef struct {
    JceShaderNode nodes[JCE_SHADER_GRAPH_MAX_NODES];
    JceShaderEdge edges[JCE_SHADER_GRAPH_MAX_EDGES];
    uint16_t      node_count;
    uint16_t      edge_count;
    /* Id of the master/output node ("PBR Master" or "Unlit Master"),
     * or JCE_SHADER_NODE_INVALID when not assigned yet. */
    uint16_t      master_node;
} JceShaderGraph;

/* ── Lifecycle ───────────────────────────────────────────────── */

JCE_API void jce_shader_graph_init (JceShaderGraph *g);
JCE_API void jce_shader_graph_clear(JceShaderGraph *g);

/* ── Node manipulation ───────────────────────────────────────── */

/* Add a node of the given type.  `slot_protos` supplies the slot
 * layout (input first, then output, in editor order); `slot_count`
 * caps at JCE_SHADER_NODE_MAX_SLOTS.  Returns the new node id, or
 * JCE_SHADER_NODE_INVALID on overflow. */
JCE_API uint16_t jce_shader_graph_add_node(JceShaderGraph      *g,
                                            const char          *type_name,
                                            const JceShaderSlot *slot_protos,
                                            uint8_t              slot_count,
                                            float canvas_x,
                                            float canvas_y);

/* Remove a node and any edges incident to it.  Marks the node slot
 * inactive (id remains valid for the lifetime of the graph). */
JCE_API bool jce_shader_graph_remove_node(JceShaderGraph *g, uint16_t node);

JCE_API JceShaderNode *jce_shader_graph_get_node(JceShaderGraph *g, uint16_t id);

/* Tag node as the master output (only one at a time). */
JCE_API void jce_shader_graph_set_master(JceShaderGraph *g, uint16_t node);

/* ── Edge manipulation ───────────────────────────────────────── */

/* Connect src.slot → dst.slot.  Fails if the slot directions are
 * wrong (src must be OUT, dst must be IN), types are incompatible,
 * or the destination input is already connected.  Returns the new
 * edge id, or JCE_SHADER_EDGE_INVALID. */
JCE_API uint16_t jce_shader_graph_connect(JceShaderGraph *g,
                                           uint16_t src_node, uint8_t src_slot,
                                           uint16_t dst_node, uint8_t dst_slot);

JCE_API bool     jce_shader_graph_disconnect(JceShaderGraph *g, uint16_t edge);

/* Find the edge currently feeding (dst_node, dst_slot), or
 * JCE_SHADER_EDGE_INVALID. */
JCE_API uint16_t jce_shader_graph_input_edge(const JceShaderGraph *g,
                                              uint16_t dst_node,
                                              uint8_t  dst_slot);

/* ── Queries ─────────────────────────────────────────────────── */

JCE_API uint16_t jce_shader_graph_active_node_count(const JceShaderGraph *g);
JCE_API uint16_t jce_shader_graph_active_edge_count(const JceShaderGraph *g);

/* Slot type compatibility — true iff `src` can drive `dst` without
 * a conversion node.  Handles scalar→vector broadcast (float→vec3). */
JCE_API bool jce_shader_slot_types_compatible(JceShaderSlotType src,
                                               JceShaderSlotType dst);

/* Number of components in a slot type (1 for float, 3 for vec3, …;
 * 0 for non-numeric types). */
JCE_API uint8_t jce_shader_slot_arity(JceShaderSlotType t);

JCE_EXTERN_C_END

#endif /* JCE_SHADER_GRAPH_H */
