/*
 * jce_shader_graph_eval.c  Collect uniforms reachable from master.
 *
 * Walks the graph depth-first from the master node, recording every
 * leaf input's value (unconnected pin's default, or a const node's
 * value).  This is the minimal-viable evaluator — full codegen +
 * intermediate-value computation lands in a downstream batch.
 */

#include <jce/renderer/jce_shader_graph_eval.h>

#include <stdio.h>
#include <string.h>

static void emit_uniform(JceShaderEvalResult *r, const char *name,
                          const float *v, JceShaderSlotType t)
{
    if (r->uniform_count >= JCE_SHADER_GRAPH_EVAL_MAX_UNIFORMS) return;
    JceShaderUniform *u = &r->uniforms[r->uniform_count++];
    strncpy(u->name, name, JCE_SHADER_SLOT_NAME_LEN - 1);
    u->name[JCE_SHADER_SLOT_NAME_LEN - 1] = '\0';
    memcpy(u->value, v, sizeof(u->value));
    u->type = t;
}

static void walk_node(const JceShaderGraph *g, uint16_t node_id,
                       uint8_t *visited, JceShaderEvalResult *out)
{
    if (node_id >= JCE_SHADER_GRAPH_MAX_NODES) return;
    if (visited[node_id]) return;
    visited[node_id] = 1;
    const JceShaderNode *n = &g->nodes[node_id];
    if (!n->active) return;
    for (uint8_t s = 0; s < JCE_SHADER_NODE_MAX_SLOTS; ++s) {
        const JceShaderSlot *slot = &n->slots[s];
        if (!slot->active || slot->dir != JCE_SHADER_SLOT_DIR_IN) continue;
        uint16_t edge = jce_shader_graph_input_edge(g, node_id, s);
        if (edge == JCE_SHADER_EDGE_INVALID) {
            /* Unconnected input → emit literal as a uniform. */
            char key[JCE_SHADER_SLOT_NAME_LEN];
            snprintf(key, sizeof(key), "%s.%s", n->type_name, slot->name);
            emit_uniform(out, key, slot->default_value, slot->type);
        } else {
            const JceShaderEdge *e = &g->edges[edge];
            walk_node(g, e->src_node, visited, out);
        }
    }
}

uint32_t jce_shader_graph_eval(const JceShaderGraph *g,
                                JceShaderEvalResult *out)
{
    if (!g || !out) return 0;
    memset(out, 0, sizeof(*out));
    if (g->master_node == JCE_SHADER_NODE_INVALID) return 0;
    uint8_t visited[JCE_SHADER_GRAPH_MAX_NODES] = {0};
    walk_node(g, g->master_node, visited, out);
    return out->uniform_count;
}
