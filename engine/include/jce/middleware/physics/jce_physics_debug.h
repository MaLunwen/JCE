/*
 * jce_physics_debug.h  Per-frame physics statistics.
 *
 * Mirrors Unity's Physics Debugger info — broadphase pair count,
 * narrowphase manifold count, contact points, active rigid bodies,
 * etc.  Storage is a single global snapshot updated by the physics
 * step; consumers read it via jce_physics_debug_get().
 *
 * Bullet integration sets the values from inside the simulation
 * tick (a downstream TU); this header is the read-side contract.
 *
 * Layer: middleware/physics (Layer 4) — public.
 */

#ifndef JCE_PHYSICS_DEBUG_H
#define JCE_PHYSICS_DEBUG_H

#include <jce/os/core/jce_defs.h>

#include <stdint.h>

JCE_EXTERN_C_BEGIN

#define JCE_PHYSICS_DEBUG_TOP_CONTACTS 8

typedef struct {
    float position[3];
    float normal[3];
    float impulse;
    uint64_t body_a;
    uint64_t body_b;
} JcePhysicsContact;

typedef struct {
    /* Counts. */
    uint32_t broadphase_pairs;
    uint32_t narrowphase_manifolds;
    uint32_t total_contact_points;
    uint32_t active_rigid_bodies;
    uint32_t sleeping_rigid_bodies;
    uint32_t static_rigid_bodies;
    uint32_t kinematic_rigid_bodies;
    /* Times in nanoseconds. */
    uint64_t broadphase_ns;
    uint64_t narrowphase_ns;
    uint64_t solver_ns;

    /* Top-N contact points by impulse (for visualisation). */
    JcePhysicsContact top_contacts[JCE_PHYSICS_DEBUG_TOP_CONTACTS];
    uint32_t          top_contact_count;
} JcePhysicsDebugStats;

/* Read the latest snapshot.  Returns NULL only on out-of-process
 * issues; normally returns a pointer to internal storage. */
JCE_API const JcePhysicsDebugStats *jce_physics_debug_get(void);

/* Write helpers used by the physics step. */
JCE_API void jce_physics_debug_set(const JcePhysicsDebugStats *s);

/* Append a contact point to the rolling top-N (orders by impulse). */
JCE_API void jce_physics_debug_push_contact(const JcePhysicsContact *c);

/* Clear contact list (called at frame start by physics step). */
JCE_API void jce_physics_debug_clear_contacts(void);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_DEBUG_H */
