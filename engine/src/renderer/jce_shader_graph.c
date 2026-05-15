/*
 * jce_shader_graph.c  Shader Graph DAG implementation.
 *
 * Pure data-layer; no bgfx, no codegen.  Storage is contiguous fixed
 * arrays — adding/removing a node flips its `active` flag rather
 * than shifting indices, so external references (canvas selection,
 * undo stacks) stay stable for a graph's lifetime.
 */

#include <jce/renderer/jce_shader_graph.h>

#include <string.h>

void jce_shader_graph_init(JceShaderGraph *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->master_node = JCE_SHADER_NODE_INVALID;
}

void jce_shader_graph_clear(JceShaderGraph *g) { jce_shader_graph_init(g); }

uint8_t jce_shader_slot_arity(JceShaderSlotType t)
{
    switch (t) {
    case JCE_SHADER_SLOT_FLOAT:   return 1;
    case JCE_SHADER_SLOT_VEC2:    return 2;
    case JCE_SHADER_SLOT_VEC3:    return 3;
    case JCE_SHADER_SLOT_VEC4:    return 4;
    case JCE_SHADER_SLOT_INT:     return 1;
    case JCE_SHADER_SLOT_BOOL:    return 1;
    default:                      return 0;
    }
}

bool jce_shader_slot_types_compatible(JceShaderSlotType src,
                                       JceShaderSlotType dst)
{
    if (src == dst) return true;
    /* Scalar broadcast: float drives any vec type. */
    if (src == JCE_SHADER_SLOT_FLOAT &&
        (dst == JCE_SHADER_SLOT_VEC2 ||
         dst == JCE_SHADER_SLOT_VEC3 ||
         dst == JCE_SHADER_SLOT_VEC4)) return true;
    /* Vector truncation: vec4 drives vec3, vec3 drives vec2.  This
     * matches Unity Shader Graph's auto-conversion behaviour. */
    if (src == JCE_SHADER_SLOT_VEC4 && dst == JCE_SHADER_SLOT_VEC3) return true;
    if (src == JCE_SHADER_SLOT_VEC3 && dst == JCE_SHADER_SLOT_VEC2) return true;
    /* Int → float promotion. */
    if (src == JCE_SHADER_SLOT_INT && dst == JCE_SHADER_SLOT_FLOAT) return true;
    return false;
}

uint16_t jce_shader_graph_add_node(JceShaderGraph *g,
                                    const char *type_name,
                                    const JceShaderSlot *slot_protos,
                                    uint8_t slot_count,
                                    float canvas_x, float canvas_y)
{
    if (!g || !type_name) return JCE_SHADER_NODE_INVALID;
    if (slot_count > JCE_SHADER_NODE_MAX_SLOTS) slot_count = JCE_SHADER_NODE_MAX_SLOTS;

    /* Find a free slot — prefer reusing inactive entries. */
    uint16_t id = JCE_SHADER_NODE_INVALID;
    for (uint16_t i = 0; i < g->node_count; ++i) {
        if (!g->nodes[i].active) { id = i; break; }
    }
    if (id == JCE_SHADER_NODE_INVALID) {
        if (g->node_count >= JCE_SHADER_GRAPH_MAX_NODES)
            return JCE_SHADER_NODE_INVALID;
        id = g->node_count++;
    }

    JceShaderNode *n = &g->nodes[id];
    memset(n, 0, sizeof(*n));
    strncpy(n->type_name, type_name, JCE_SHADER_NODE_TYPE_LEN - 1);
    n->canvas_x = canvas_x;
    n->canvas_y = canvas_y;
    n->active = true;

    if (slot_protos) {
        for (uint8_t i = 0; i < slot_count; ++i) {
            n->slots[i] = slot_protos[i];
            n->slots[i].active = true;
        }
    }
    return id;
}

JceShaderNode *jce_shader_graph_get_node(JceShaderGraph *g, uint16_t id)
{
    if (!g || id >= g->node_count) return NULL;
    if (!g->nodes[id].active) return NULL;
    return &g->nodes[id];
}

bool jce_shader_graph_remove_node(JceShaderGraph *g, uint16_t node)
{
    if (!g || node >= g->node_count) return false;
    if (!g->nodes[node].active) return false;
    g->nodes[node].active = false;

    /* Drop every edge that touches this node. */
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) continue;
        if (g->edges[i].src_node == node || g->edges[i].dst_node == node)
            g->edges[i].active = false;
    }
    if (g->master_node == node) g->master_node = JCE_SHADER_NODE_INVALID;
    return true;
}

void jce_shader_graph_set_master(JceShaderGraph *g, uint16_t node)
{
    if (!g) return;
    if (jce_shader_graph_get_node(g, node)) g->master_node = node;
}

uint16_t jce_shader_graph_input_edge(const JceShaderGraph *g,
                                      uint16_t dst_node, uint8_t dst_slot)
{
    if (!g) return JCE_SHADER_EDGE_INVALID;
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) continue;
        if (g->edges[i].dst_node == dst_node &&
            g->edges[i].dst_slot == dst_slot)
            return i;
    }
    return JCE_SHADER_EDGE_INVALID;
}

uint16_t jce_shader_graph_connect(JceShaderGraph *g,
                                   uint16_t src_node, uint8_t src_slot,
                                   uint16_t dst_node, uint8_t dst_slot)
{
    JceShaderNode *src = jce_shader_graph_get_node(g, src_node);
    JceShaderNode *dst = jce_shader_graph_get_node(g, dst_node);
    if (!src || !dst) return JCE_SHADER_EDGE_INVALID;
    if (src_slot >= JCE_SHADER_NODE_MAX_SLOTS) return JCE_SHADER_EDGE_INVALID;
    if (dst_slot >= JCE_SHADER_NODE_MAX_SLOTS) return JCE_SHADER_EDGE_INVALID;
    JceShaderSlot *so = &src->slots[src_slot];
    JceShaderSlot *si = &dst->slots[dst_slot];
    if (!so->active || !si->active) return JCE_SHADER_EDGE_INVALID;
    if (so->dir != JCE_SHADER_SLOT_DIR_OUT) return JCE_SHADER_EDGE_INVALID;
    if (si->dir != JCE_SHADER_SLOT_DIR_IN)  return JCE_SHADER_EDGE_INVALID;
    if (!jce_shader_slot_types_compatible(so->type, si->type))
        return JCE_SHADER_EDGE_INVALID;
    /* Reject if destination input is already wired. */
    if (jce_shader_graph_input_edge(g, dst_node, dst_slot) !=
        JCE_SHADER_EDGE_INVALID) return JCE_SHADER_EDGE_INVALID;

    uint16_t id = JCE_SHADER_EDGE_INVALID;
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) { id = i; break; }
    }
    if (id == JCE_SHADER_EDGE_INVALID) {
        if (g->edge_count >= JCE_SHADER_GRAPH_MAX_EDGES)
            return JCE_SHADER_EDGE_INVALID;
        id = g->edge_count++;
    }
    JceShaderEdge *e = &g->edges[id];
    e->src_node = src_node;
    e->src_slot = src_slot;
    e->dst_node = dst_node;
    e->dst_slot = dst_slot;
    e->active = true;
    return id;
}

bool jce_shader_graph_disconnect(JceShaderGraph *g, uint16_t edge)
{
    if (!g || edge >= g->edge_count) return false;
    if (!g->edges[edge].active) return false;
    g->edges[edge].active = false;
    return true;
}

uint16_t jce_shader_graph_active_node_count(const JceShaderGraph *g)
{
    if (!g) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < g->node_count; ++i)
        if (g->nodes[i].active) n++;
    return n;
}

uint16_t jce_shader_graph_active_edge_count(const JceShaderGraph *g)
{
    if (!g) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < g->edge_count; ++i)
        if (g->edges[i].active) n++;
    return n;
}
