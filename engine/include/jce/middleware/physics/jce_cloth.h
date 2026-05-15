/*
 * jce_cloth.h  Cloth / soft-body authoring descriptor.
 *
 * Data-layer wrapper for btSoftBody — described in terms of generic
 * particles + distance / bending constraints + sphere-and-capsule
 * collision colliders.  The actual Bullet soft-body instance is
 * owned by the physics wrapper (forward-declared as void* here so
 * we don't drag the Bullet header into game code).
 *
 * Workflow
 *   - Game / importer authors a JceClothDesc (particle positions,
 *     distance constraints derived from mesh edges, optional
 *     wind/damping)
 *   - Physics middleware reads the desc + creates a btSoftBody at
 *     simulation start
 *   - Each frame: physics middleware advances the soft body and the
 *     renderer reads the resulting positions via the descriptor's
 *     `runtime_handle`
 *
 * Mirrors Unity Cloth component data fields (stretching / bending
 * stiffness, damping, sphere collider list).
 *
 * Layer: physics (Layer 4) — public.
 */

#ifndef JCE_CLOTH_H
#define JCE_CLOTH_H

#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_CLOTH_MAX_SPHERE_COLLIDERS  8
#define JCE_CLOTH_MAX_CAPSULE_COLLIDERS 4

typedef struct {
    float center[3];
    float radius;
    bool  active;
} JceClothSphereCollider;

typedef struct {
    float p0[3];
    float p1[3];
    float radius;
    bool  active;
} JceClothCapsuleCollider;

typedef struct {
    uint32_t a;
    uint32_t b;
    float    rest_length;
} JceClothConstraint;

typedef struct {
    /* Particle positions (count == particle_count, xyz triples). */
    float    *positions;
    uint32_t  particle_count;
    /* Per-particle inverse mass (0 = pinned).  May be NULL → all 1. */
    float    *inv_mass;

    /* Mesh-derived distance constraints. */
    JceClothConstraint *distance_constraints;
    uint32_t            distance_count;
    /* Bending constraints (cross-edge for stiffer cloth). */
    JceClothConstraint *bending_constraints;
    uint32_t            bending_count;

    float damping;            /* 0..1 velocity damping per step */
    float stretching_stiff;   /* 0..1 */
    float bending_stiff;      /* 0..1 */
    float wind[3];            /* world-space force */
    float gravity_mul;        /* 1.0 = world gravity */

    JceClothSphereCollider  spheres [JCE_CLOTH_MAX_SPHERE_COLLIDERS];
    JceClothCapsuleCollider capsules[JCE_CLOTH_MAX_CAPSULE_COLLIDERS];

    /* Filled in by the physics middleware when the simulation
     * instance is created.  Opaque to game code. */
    void *runtime_handle;
} JceClothDesc;

/* ── Lifecycle ───────────────────────────────────────────────── */

JCE_API void jce_cloth_desc_init   (JceClothDesc *d);
JCE_API void jce_cloth_desc_dispose(JceClothDesc *d);

/* Allocate particle storage with `count` particles; clears any
 * previous data.  Returns false on OOM. */
JCE_API bool jce_cloth_set_particle_count(JceClothDesc *d, uint32_t count);

/* Bulk setters (copy semantics).  Sizes must match particle_count
 * for positions / inv_mass. */
JCE_API bool jce_cloth_set_positions     (JceClothDesc *d,
                                            const float *xyz_triples);
JCE_API bool jce_cloth_set_inv_mass      (JceClothDesc *d,
                                            const float *inv_mass);

JCE_API bool jce_cloth_set_distance_constraints(JceClothDesc *d,
                                                 const JceClothConstraint *c,
                                                 uint32_t count);
JCE_API bool jce_cloth_set_bending_constraints (JceClothDesc *d,
                                                 const JceClothConstraint *c,
                                                 uint32_t count);

/* Mesh-edge helper — derive distance constraints from triangle
 * indices (each triangle's three edges become one constraint).
 * Deduplicates so an edge shared between two triangles makes one
 * constraint.  Caller still owns positions; rest_length is computed
 * from the position array. */
JCE_API bool jce_cloth_build_distance_from_triangles(JceClothDesc   *d,
                                                       const uint32_t *indices,
                                                       uint32_t        triangle_count);

JCE_EXTERN_C_END

#endif /* JCE_CLOTH_H */
