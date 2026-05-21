/*
 * jce_physics_material.h  Stand-alone Physics Material asset.
 *
 * Unity-parity PhysicsMaterial: friction (static + dynamic), restitution
 * (bounciness), and combine modes that decide how the per-body values
 * collapse to a single contact value when two bodies touch.
 *
 * Combine-mode precedence when the two bodies disagree (Unity rule):
 *
 *     MAX  >  MULTIPLY  >  MIN  >  AVERAGE
 *
 * The "higher" mode wins.  Two bodies set to AVERAGE keep AVERAGE; one
 * set to MAX and one to MIN collapse to MAX, etc.
 *
 * The struct is POD; loads use a stack copy and never allocate.
 *
 * Layer: middleware/physics (L4).  Pure C99.
 */

#ifndef JCE_PHYSICS_MATERIAL_H
#define JCE_PHYSICS_MATERIAL_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>

JCE_EXTERN_C_BEGIN

typedef enum JcePhysicsCombine {
    JCE_PHYS_COMBINE_AVERAGE  = 0,
    JCE_PHYS_COMBINE_MIN      = 1,
    JCE_PHYS_COMBINE_MAX      = 2,
    JCE_PHYS_COMBINE_MULTIPLY = 3
} JcePhysicsCombine;

typedef struct JcePhysicsMaterial {
    float             dynamic_friction;     /* 0..1 typical */
    float             static_friction;      /* 0..1 typical */
    float             restitution;          /* 0..1 bounciness */
    JcePhysicsCombine friction_combine;
    JcePhysicsCombine restitution_combine;
} JcePhysicsMaterial;

/* Reset to engine defaults: friction 0.6 / 0.6, restitution 0,
 * AVERAGE combines.  Safe to call with `m == NULL` (no-op). */
JCE_API void jce_physics_material_init_default(JcePhysicsMaterial *m);

/* Combine two materials per their combine modes, producing the contact
 * friction and restitution that would be used between bodies wearing
 * those materials.  Either material may be NULL — a NULL input is
 * treated as engine defaults.  Output pointers may be NULL.
 *
 * Used at material-set time (when the user wants JCE-level semantics)
 * or by future contact callbacks.  Bullet itself runs its own combine
 * (multiply for friction, max for restitution) on the per-body values
 * set via jce_physics_body_set_material(). */
JCE_API void jce_physics_material_combine(const JcePhysicsMaterial *a,
                                          const JcePhysicsMaterial *b,
                                          float *out_friction,
                                          float *out_restitution);

/* JSON I/O via the engine's jce_json facade.  Schema:
 *   { "$schema": "jce.physmat.v1",
 *     "dynamic_friction": 0.6, "static_friction": 0.6,
 *     "restitution": 0.0,
 *     "friction_combine":    "average"|"min"|"max"|"multiply",
 *     "restitution_combine": "average"|"min"|"max"|"multiply" }
 *
 * Both functions use jce_fs_host_* under the hood (no raw C runtime).
 * Returns false on I/O or parse error; `out` is left as defaults on
 * load failure. */
JCE_API bool jce_physics_material_load(const char *vfs_path,
                                       JcePhysicsMaterial *out);
JCE_API bool jce_physics_material_save(const char *vfs_path,
                                       const JcePhysicsMaterial *m);

/* String <-> enum helpers (lowercase canonical strings).
 * Unknown input falls back to AVERAGE / "average". */
JCE_API const char       *jce_physics_combine_to_string(JcePhysicsCombine c);
JCE_API JcePhysicsCombine jce_physics_combine_from_string(const char *s);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_MATERIAL_H */
