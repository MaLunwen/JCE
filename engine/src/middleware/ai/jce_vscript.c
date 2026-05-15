/*
 * jce_vscript.c  Visual scripting DAG management.
 *
 * Storage + topology operations.  Type-compatibility table covers
 * implicit int↔float / bool→int promotion; exec pins must match
 * exactly (no value coercion on control flow).
 */

#include <jce/middleware/ai/jce_vscript.h>

#include <string.h>

void jce_vs_init(JceVsGraph *g)
{
    if (!g) return;
    memset(g, 0, sizeof(*g));
    g->entry_node = 0xFFFFu;
}

void jce_vs_clear(JceVsGraph *g) { jce_vs_init(g); }

static bool types_compatible(JceVsType src, JceVsType dst)
{
    if (src == dst) return true;
    if (src == JCE_VS_TYPE_EXEC || dst == JCE_VS_TYPE_EXEC) return false;
    if (src == JCE_VS_TYPE_INT  && dst == JCE_VS_TYPE_FLOAT) return true;
    if (src == JCE_VS_TYPE_BOOL && dst == JCE_VS_TYPE_INT  ) return true;
    if (src == JCE_VS_TYPE_BOOL && dst == JCE_VS_TYPE_FLOAT) return true;
    return false;
}

uint16_t jce_vs_add_node(JceVsGraph *g, const char *type_name,
                          const JceVsPin *protos, uint8_t pin_count,
                          float x, float y)
{
    if (!g || !type_name) return 0xFFFFu;
    if (pin_count > JCE_VSCRIPT_PINS_PER_NODE)
        pin_count = JCE_VSCRIPT_PINS_PER_NODE;
    uint16_t id = 0xFFFFu;
    for (uint16_t i = 0; i < g->node_count; ++i)
        if (!g->nodes[i].active) { id = i; break; }
    if (id == 0xFFFFu) {
        if (g->node_count >= JCE_VSCRIPT_MAX_NODES) return 0xFFFFu;
        id = g->node_count++;
    }
    JceVsNode *n = &g->nodes[id];
    memset(n, 0, sizeof(*n));
    strncpy(n->type_name, type_name, JCE_VSCRIPT_NODE_TYPE_LEN - 1);
    n->canvas_x = x; n->canvas_y = y;
    n->active = true;
    if (protos) {
        for (uint8_t i = 0; i < pin_count; ++i) {
            n->pins[i] = protos[i];
            n->pins[i].active = true;
        }
    }
    return id;
}

JceVsNode *jce_vs_get_node(JceVsGraph *g, uint16_t id)
{
    if (!g || id >= g->node_count || !g->nodes[id].active) return NULL;
    return &g->nodes[id];
}

bool jce_vs_remove_node(JceVsGraph *g, uint16_t node)
{
    if (!g || node >= g->node_count || !g->nodes[node].active) return false;
    g->nodes[node].active = false;
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) continue;
        if (g->edges[i].src_node == node || g->edges[i].dst_node == node)
            g->edges[i].active = false;
    }
    if (g->entry_node == node) g->entry_node = 0xFFFFu;
    return true;
}

uint16_t jce_vs_input_edge(const JceVsGraph *g, uint16_t dst, uint8_t pin)
{
    if (!g) return 0xFFFFu;
    for (uint16_t i = 0; i < g->edge_count; ++i) {
        if (!g->edges[i].active) continue;
        if (g->edges[i].dst_node == dst && g->edges[i].dst_pin == pin)
            return i;
    }
    return 0xFFFFu;
}

uint16_t jce_vs_connect(JceVsGraph *g, uint16_t src, uint8_t sp,
                          uint16_t dst, uint8_t dp)
{
    JceVsNode *sn = jce_vs_get_node(g, src);
    JceVsNode *dn = jce_vs_get_node(g, dst);
    if (!sn || !dn) return 0xFFFFu;
    if (sp >= JCE_VSCRIPT_PINS_PER_NODE) return 0xFFFFu;
    if (dp >= JCE_VSCRIPT_PINS_PER_NODE) return 0xFFFFu;
    JceVsPin *so = &sn->pins[sp];
    JceVsPin *si = &dn->pins[dp];
    if (!so->active || !si->active) return 0xFFFFu;
    if (so->dir != JCE_VS_PIN_OUT) return 0xFFFFu;
    if (si->dir != JCE_VS_PIN_IN)  return 0xFFFFu;
    if (!types_compatible(so->type, si->type)) return 0xFFFFu;
    /* Exec destination input may have only one source; data inputs same. */
    if (jce_vs_input_edge(g, dst, dp) != 0xFFFFu) return 0xFFFFu;

    uint16_t id = 0xFFFFu;
    for (uint16_t i = 0; i < g->edge_count; ++i)
        if (!g->edges[i].active) { id = i; break; }
    if (id == 0xFFFFu) {
        if (g->edge_count >= JCE_VSCRIPT_MAX_EDGES) return 0xFFFFu;
        id = g->edge_count++;
    }
    g->edges[id].src_node = src;
    g->edges[id].src_pin  = sp;
    g->edges[id].dst_node = dst;
    g->edges[id].dst_pin  = dp;
    g->edges[id].active   = true;
    return id;
}

bool jce_vs_disconnect(JceVsGraph *g, uint16_t edge)
{
    if (!g || edge >= g->edge_count || !g->edges[edge].active) return false;
    g->edges[edge].active = false;
    return true;
}

void jce_vs_set_entry(JceVsGraph *g, uint16_t node)
{
    if (jce_vs_get_node(g, node)) g->entry_node = node;
}

uint16_t jce_vs_active_node_count(const JceVsGraph *g)
{
    if (!g) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < g->node_count; ++i)
        if (g->nodes[i].active) n++;
    return n;
}

uint16_t jce_vs_active_edge_count(const JceVsGraph *g)
{
    if (!g) return 0;
    uint16_t n = 0;
    for (uint16_t i = 0; i < g->edge_count; ++i)
        if (g->edges[i].active) n++;
    return n;
}
