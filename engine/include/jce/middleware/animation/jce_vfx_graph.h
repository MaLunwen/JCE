/*
 * jce_vfx_graph.h  VFX graph asset + runtime instance (stub for P5).
 */

#ifndef JCE_VFX_GRAPH_H
#define JCE_VFX_GRAPH_H

#include <jce/os/core/jce_defs.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceVfxGraph    JceVfxGraph;
typedef struct JceVfxInstance JceVfxInstance;

JCE_API JceVfxGraph    *jce_vfx_graph_load(const char *path);
JCE_API void            jce_vfx_graph_unload(JceVfxGraph *g);

JCE_API JceVfxInstance *jce_vfx_instance_create(JceVfxGraph *g);
JCE_API void            jce_vfx_instance_destroy(JceVfxInstance *inst);
JCE_API void            jce_vfx_instance_tick(JceVfxInstance *inst, float dt);
JCE_API void            jce_vfx_instance_submit(JceVfxInstance *inst, uint16_t view_id);

JCE_EXTERN_C_END

#endif /* JCE_VFX_GRAPH_H */
