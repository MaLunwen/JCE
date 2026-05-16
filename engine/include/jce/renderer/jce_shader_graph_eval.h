/*
 * jce_shader_graph_eval.h  Topo-sorted DAG → flat uniform list.
 *
 * Walks a Shader Graph from the master node and collects every node
 * input that resolves to a constant or a named material parameter
 * into a flat uniform table.  The renderer's material-bind path
 * uploads this table per draw call.
 *
 * Layer: renderer (Layer 3) — public.
 */

#ifndef JCE_SHADER_GRAPH_EVAL_H
#define JCE_SHADER_GRAPH_EVAL_H

#include <jce/renderer/jce_shader_graph.h>

JCE_EXTERN_C_BEGIN

#define JCE_SHADER_GRAPH_EVAL_MAX_UNIFORMS 32

typedef struct {
    char  name[JCE_SHADER_SLOT_NAME_LEN];
    float value[4];
    JceShaderSlotType type;
} JceShaderUniform;

typedef struct {
    JceShaderUniform uniforms[JCE_SHADER_GRAPH_EVAL_MAX_UNIFORMS];
    uint32_t         uniform_count;
} JceShaderEvalResult;

/* Walk `g` and flatten reachable inputs into `out`.  Returns the
 * count.  Unconnected input slots contribute their literal default;
 * connected inputs walk further.  Cycles aren't expected (graph is
 * a DAG by construction) but a visited-set guards anyway. */
JCE_API uint32_t jce_shader_graph_eval(const JceShaderGraph *g,
                                         JceShaderEvalResult *out);

JCE_EXTERN_C_END

#endif /* JCE_SHADER_GRAPH_EVAL_H */
