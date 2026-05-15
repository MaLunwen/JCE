/*
 * jce_vscript.h  Visual scripting node graph.
 *
 * Bolt / Unity Visual Scripting equivalent at the data layer.  A
 * graph is a DAG of typed nodes with two edge channels:
 *   - Data edges: carry typed values (float / bool / vec3 / string)
 *   - Exec edges: drive the dispatch order (statement sequencing)
 *
 * Nodes have a fixed schema (input pins + output pins of each kind).
 * The library of node kinds is registered separately
 * (jce_vscript_nodes.{c,h}); this header owns the model + DAG
 * topology + JSON serialization shape.
 *
 * Layer: middleware/ai (Layer 4) — public.
 */

#ifndef JCE_VSCRIPT_H
#define JCE_VSCRIPT_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_VSCRIPT_MAX_NODES   512
#define JCE_VSCRIPT_MAX_EDGES   1024
#define JCE_VSCRIPT_PINS_PER_NODE 8
#define JCE_VSCRIPT_NODE_TYPE_LEN 32
#define JCE_VSCRIPT_PIN_NAME_LEN  16

typedef enum {
    JCE_VS_TYPE_EXEC   = 0,   /* control flow, no data */
    JCE_VS_TYPE_FLOAT  = 1,
    JCE_VS_TYPE_INT    = 2,
    JCE_VS_TYPE_BOOL   = 3,
    JCE_VS_TYPE_VEC3   = 4,
    JCE_VS_TYPE_STRING = 5,
    JCE_VS_TYPE_ENTITY = 6,   /* opaque uint64 entity handle */
} JceVsType;

typedef enum {
    JCE_VS_PIN_IN  = 0,
    JCE_VS_PIN_OUT = 1,
} JceVsPinDir;

typedef struct {
    char        name[JCE_VSCRIPT_PIN_NAME_LEN];
    JceVsType   type;
    JceVsPinDir dir;
    /* Literal value for unconnected input pins.  Interpretation
     * depends on type — vec3 uses lit_f[0..2]; entity uses lit_u64. */
    float       lit_f[3];
    int32_t     lit_i;
    int32_t     lit_b;
    uint64_t    lit_u64;
    char        lit_str[64];
    bool        active;
} JceVsPin;

typedef struct {
    char     type_name[JCE_VSCRIPT_NODE_TYPE_LEN];
    JceVsPin pins[JCE_VSCRIPT_PINS_PER_NODE];
    /* Canvas position (editor-side persistence). */
    float    canvas_x;
    float    canvas_y;
    bool     active;
} JceVsNode;

typedef struct {
    uint16_t src_node;
    uint8_t  src_pin;
    uint16_t dst_node;
    uint8_t  dst_pin;
    bool     active;
} JceVsEdge;

typedef struct {
    JceVsNode nodes[JCE_VSCRIPT_MAX_NODES];
    JceVsEdge edges[JCE_VSCRIPT_MAX_EDGES];
    uint16_t  node_count;
    uint16_t  edge_count;
    /* Entry node id (typically a "Start" / "OnEvent" node).  Used by
     * the VM as the dispatch root. */
    uint16_t  entry_node;
} JceVsGraph;

/* ── Lifecycle ───────────────────────────────────────────────── */

JCE_API void jce_vs_init (JceVsGraph *g);
JCE_API void jce_vs_clear(JceVsGraph *g);

/* Add a node with `type_name`, populating `pin_protos[]`.  Returns
 * the new node id or 0xFFFF. */
JCE_API uint16_t jce_vs_add_node(JceVsGraph     *g,
                                   const char     *type_name,
                                   const JceVsPin *pin_protos,
                                   uint8_t         pin_count,
                                   float canvas_x,
                                   float canvas_y);

JCE_API bool jce_vs_remove_node(JceVsGraph *g, uint16_t node);

JCE_API JceVsNode *jce_vs_get_node(JceVsGraph *g, uint16_t id);

/* Connect.  Returns edge id or 0xFFFF; fails on type mismatch /
 * dir mismatch / dst already wired. */
JCE_API uint16_t jce_vs_connect(JceVsGraph *g,
                                  uint16_t src, uint8_t src_pin,
                                  uint16_t dst, uint8_t dst_pin);

JCE_API bool jce_vs_disconnect(JceVsGraph *g, uint16_t edge);

JCE_API uint16_t jce_vs_input_edge(const JceVsGraph *g,
                                     uint16_t dst, uint8_t dst_pin);

JCE_API void jce_vs_set_entry(JceVsGraph *g, uint16_t node);

/* Active counts. */
JCE_API uint16_t jce_vs_active_node_count(const JceVsGraph *g);
JCE_API uint16_t jce_vs_active_edge_count(const JceVsGraph *g);

JCE_EXTERN_C_END

#endif /* JCE_VSCRIPT_H */
