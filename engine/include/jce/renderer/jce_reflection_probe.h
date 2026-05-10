/*
 * jce_reflection_probe.h  Per-fragment reflection-probe selection.
 *
 * Given a world position and a list of authored reflection probes
 * (Box or Sphere influence volumes), return the 1–2 probes whose
 * volumes contain the position with their blend weights.  The shader-
 * side code uses these to fetch + lerp two cubemaps for environment
 * reflections, matching Unity's ReflectionProbe priority/blend
 * convention.
 *
 * Pure CPU — does not invoke bgfx; cubemap handles are passed through
 * as opaque uint16 ids.  Layer: renderer (Layer 5) — public.
 */

#ifndef JCE_REFLECTION_PROBE_H
#define JCE_REFLECTION_PROBE_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef enum {
    JCE_REFL_PROBE_BOX    = 0,
    JCE_REFL_PROBE_SPHERE = 1,
} JceReflProbeShape;

/* Single probe record consumed by the selector.  Authoring side
 * (editor reflection-probe component) emits one of these per probe. */
typedef struct {
    JceReflProbeShape shape;
    jce_vec3          center;
    jce_vec3          half_extents;    /* box: half-size; sphere: (r,r,r) */
    float             blend_distance;  /* falloff outside the volume in m */
    int               priority;        /* higher wins ties; -1 = ignore */
    uint16_t          cubemap_handle;  /* opaque id for the renderer */
} JceReflProbe;

/* Result: up to two probes blended by weight.  When `count == 0`,
 * caller should fall back to the global IBL / skybox. */
typedef struct {
    int      count;            /* 0..2 */
    uint16_t cubemap[2];
    float    weight[2];        /* sums to 1 when count > 0 */
} JceReflProbeBlend;

/* Select probes for `world_pos`.  Walks the probe list once, prefers
 * probes whose volume contains the position (weight by 1/distance to
 * surface) and falls back to the closest probe if none fully contain.
 * Higher-priority probes override lower at full overlap. */
JCE_API JceReflProbeBlend jce_reflection_probe_select(jce_vec3 world_pos,
                                                       const JceReflProbe *probes,
                                                       uint32_t            count);

JCE_EXTERN_C_END

#endif /* JCE_REFLECTION_PROBE_H */
