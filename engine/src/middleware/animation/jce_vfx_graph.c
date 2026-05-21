/*
 * jce_vfx_graph.c  Stub VFX graph implementation.
 *
 * Real graph evaluator + GPU particle pipeline will land in P5. The stub
 * lets the ECS component compile + serialize today without dragging in
 * any new third-party code.
 */

#include <jce/middleware/animation/jce_vfx_graph.h>
#include "os/core/jce_memory.h"
#include <stddef.h>

struct JceVfxGraph    { int ref; };
struct JceVfxInstance { JceVfxGraph *graph; float time; };

JceVfxGraph *jce_vfx_graph_load(const char *path)
{
    (void)path;
    JceVfxGraph *g = (JceVfxGraph *)JCE_CALLOC(1, sizeof(JceVfxGraph));
    if (g) g->ref = 1;
    return g;
}

void jce_vfx_graph_unload(JceVfxGraph *g)
{
    if (!g) return;
    if (--g->ref <= 0) JCE_FREE(g);
}

JceVfxInstance *jce_vfx_instance_create(JceVfxGraph *g)
{
    if (!g) return NULL;
    JceVfxInstance *inst = (JceVfxInstance *)JCE_CALLOC(1, sizeof(JceVfxInstance));
    if (!inst) return NULL;
    inst->graph = g;
    g->ref++;
    return inst;
}

void jce_vfx_instance_destroy(JceVfxInstance *inst)
{
    if (!inst) return;
    if (inst->graph) jce_vfx_graph_unload(inst->graph);
    JCE_FREE(inst);
}

void jce_vfx_instance_tick(JceVfxInstance *inst, float dt)
{
    if (!inst) return;
    inst->time += dt;
}

void jce_vfx_instance_submit(JceVfxInstance *inst, uint16_t view_id)
{
    (void)inst;
    (void)view_id;
}
