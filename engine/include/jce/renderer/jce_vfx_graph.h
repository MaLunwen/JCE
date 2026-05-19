/*
 * jce_vfx_graph.h  Visual-Effects Graph DAG (data layer).
 *
 * Unity VFX Graph equivalent — a node-based authoring surface
 * targeting GPU particle systems.  Distinct from Shader Graph: VFX
 * graphs describe per-particle behaviour (spawn rate, lifetime,
 * velocity, color, attribute over time) while Shader Graph
 * describes material surface response.
 *
 * Layer 3 (renderer) — public.  No bgfx coupling; the runtime
 * exec layer (compute pass + spawn buffer) is wired in a host
 * build by binding the DAG to jce_compute and jce_gpu_particles.
 */

#ifndef JCE_VFX_GRAPH_H
#define JCE_VFX_GRAPH_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_VFX_GRAPH_MAX_NODES   256
#define JCE_VFX_GRAPH_MAX_EDGES   512
#define JCE_VFX_NODE_MAX_SLOTS    8
#define JCE_VFX_NODE_TYPE_LEN     32
#define JCE_VFX_SLOT_NAME_LEN     24

typedef enum {
    JCE_VFX_SLOT_FLOAT     = 0,
    JCE_VFX_SLOT_VEC3      = 1,
    JCE_VFX_SLOT_VEC4      = 2,
    JCE_VFX_SLOT_COLOR     = 3,
    JCE_VFX_SLOT_TEXTURE   = 4,
    JCE_VFX_SLOT_EVENT     = 5,   /* particle event channel (Spawn / Update / OutputContext) */
    JCE_VFX_SLOT__COUNT
} JceVfxSlotType;

typedef enum {
    JCE_VFX_SLOT_DIR_IN  = 0,
    JCE_VFX_SLOT_DIR_OUT = 1,
} JceVfxSlotDir;

typedef enum {
    JCE_VFX_CTX_SPAWN  = 0,
    JCE_VFX_CTX_INIT   = 1,
    JCE_VFX_CTX_UPDATE = 2,
    JCE_VFX_CTX_OUTPUT = 3,
} JceVfxNodeContext;

typedef struct {
    char           name[JCE_VFX_SLOT_NAME_LEN];
    JceVfxSlotType type;
    JceVfxSlotDir  dir;
    float          default_value[4];
    bool           active;
} JceVfxSlot;

typedef struct {
    char              type_name[JCE_VFX_NODE_TYPE_LEN];
    JceVfxNodeContext context;
    JceVfxSlot        slots[JCE_VFX_NODE_MAX_SLOTS];
    float             canvas_x;
    float             canvas_y;
    char              param[64];
    bool              active;
} JceVfxNode;

typedef struct {
    uint16_t src_node;
    uint8_t  src_slot;
    uint16_t dst_node;
    uint8_t  dst_slot;
    bool     active;
} JceVfxEdge;

typedef struct {
    JceVfxNode nodes[JCE_VFX_GRAPH_MAX_NODES];
    JceVfxEdge edges[JCE_VFX_GRAPH_MAX_EDGES];
    uint16_t   node_count;
    uint16_t   edge_count;
} JceVfxGraph;

/* ── Lifecycle ───────────────────────────────────────────────── */

JCE_API void jce_vfx_graph_init (JceVfxGraph *g);
JCE_API void jce_vfx_graph_clear(JceVfxGraph *g);

JCE_API uint16_t jce_vfx_graph_add_node(JceVfxGraph     *g,
                                         const char       *type_name,
                                         JceVfxNodeContext ctx,
                                         const JceVfxSlot *slot_protos,
                                         uint8_t          slot_count,
                                         float canvas_x, float canvas_y);

JCE_API bool jce_vfx_graph_remove_node(JceVfxGraph *g, uint16_t node);

JCE_API uint16_t jce_vfx_graph_connect(JceVfxGraph *g,
                                        uint16_t src, uint8_t sp,
                                        uint16_t dst, uint8_t dp);

JCE_API bool jce_vfx_graph_disconnect(JceVfxGraph *g, uint16_t edge);

JCE_API uint16_t jce_vfx_graph_active_node_count(const JceVfxGraph *g);

/* ── Built-in node factories ─────────────────────────────────── */

/* Spawn context. */
JCE_API uint16_t jce_vfxn_spawn_rate          (JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_spawn_burst         (JceVfxGraph *g, float x, float y);
/* Init context. */
JCE_API uint16_t jce_vfxn_init_velocity_random(JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_init_lifetime_random(JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_init_position_shape (JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_init_color          (JceVfxGraph *g, float x, float y);
/* Update context. */
JCE_API uint16_t jce_vfxn_update_gravity      (JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_update_drag         (JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_update_color_curve  (JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_update_size_curve   (JceVfxGraph *g, float x, float y);
/* Output context. */
JCE_API uint16_t jce_vfxn_output_quad_billboard(JceVfxGraph *g, float x, float y);
JCE_API uint16_t jce_vfxn_output_mesh         (JceVfxGraph *g, float x, float y);

/* JSON I/O. */
JCE_API bool jce_vfx_graph_save_json(const JceVfxGraph *g, const char *path);
JCE_API bool jce_vfx_graph_load_json(JceVfxGraph *g, const char *path);

JCE_EXTERN_C_END

#endif /* JCE_VFX_GRAPH_H */
