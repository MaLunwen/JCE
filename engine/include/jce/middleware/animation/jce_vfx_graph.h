/*
 * jce_vfx_graph.h  VFX graph asset + runtime instance.
 *
 * P2-particle-vfx-runtime: a MINIMAL `*.vfx.json` interpreter that runs a
 * self-contained CPU particle simulation (rate / lifetime / speed / size /
 * color / velocity).  It intentionally does NOT depend on the renderer's
 * JceParticleSystem: the VFX graph lives in the animation layer (L4), while
 * JceParticleSystem lives in the renderer (L3) which already depends on the
 * animation layer — linking the other way would be circular.  The interpreter
 * therefore owns a tiny inline simulation; the GPU billboard submit path is
 * still deferred (jce_vfx_instance_submit is a no-op).
 *
 * `*.vfx.json` schema (all keys optional; defaults match the particle editor):
 *   {
 *     "emitRate": 50, "emitBurst": 0,
 *     "lifetimeMin": 1.0, "lifetimeMax": 2.0,
 *     "velocityMin": [x,y,z], "velocityMax": [x,y,z],
 *     "gravity":     [x,y,z],
 *     "sizeStart": 0.1, "sizeEnd": 0.0,
 *     "colorStart": [r,g,b,a], "colorEnd": [r,g,b,a],
 *     "maxParticles": 1024
 *   }
 * `*.particles.json` documents parse identically (shared key set).
 */

#ifndef JCE_VFX_GRAPH_H
#define JCE_VFX_GRAPH_H

#include <jce/os/core/jce_defs.h>
#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef struct JceVfxGraph    JceVfxGraph;
typedef struct JceVfxInstance JceVfxInstance;

JCE_API JceVfxGraph    *jce_vfx_graph_load(const char *path);
JCE_API void            jce_vfx_graph_unload(JceVfxGraph *g);

/* True when the graph parsed an actual `*.vfx.json` (vs. an empty fallback). */
JCE_API bool            jce_vfx_graph_is_valid(const JceVfxGraph *g);

JCE_API JceVfxInstance *jce_vfx_instance_create(JceVfxGraph *g);
JCE_API void            jce_vfx_instance_destroy(JceVfxInstance *inst);
JCE_API void            jce_vfx_instance_tick(JceVfxInstance *inst, float dt);
JCE_API void            jce_vfx_instance_submit(JceVfxInstance *inst, uint16_t view_id);

/* Alive particle count of the instance's CPU simulation. */
JCE_API uint32_t        jce_vfx_instance_alive_count(const JceVfxInstance *inst);

JCE_EXTERN_C_END

#endif /* JCE_VFX_GRAPH_H */
