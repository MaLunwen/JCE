/*
 * jce_softbody.h  Volumetric / pressure soft-body simulation.
 *
 * A CLOSED-volume "squishy" body (an ellipsoid mass-spring shell with an
 * internal pressure term) that compresses on impact and rebounds, built on
 * Bullet's btSoftBody pressure model (kPR > 0).  Complements the surface
 * cloth in jce_cloth.h: cloth is an open mass-spring sheet, a soft body is a
 * sealed pressurised volume.
 *
 * SHARED WORLD: soft bodies and cloth live in the SAME isolated secondary
 * btSoftRigidDynamicsWorld owned by jce_cloth.cpp and share its slot pool.
 * Handles from jce_softbody_* and jce_cloth_* are therefore drawn from the
 * same 1-based index space and never collide.  The neutral controls below
 * (set_simulation_enabled / is_simulation_enabled / step_) FORWARD to that
 * single shared world — there is no second soft world.  Stepping is normally
 * driven automatically by jce_physics_step() -> jce_cloth_step_() (the SAME
 * world step), so callers rarely need jce_softbody_step_ directly.
 *
 * STATIC COLLISION: because the soft world is a btSoftRigidDynamicsWorld it
 * can hold zero-mass static btRigidBody proxies.  jce_softbody_add_static_box
 * adds a static box so a pressure body can rest / bounce on the ground; the
 * ellipsoid is created with SDF-vs-rigid collision so it collides with them.
 *
 * GATE: like cloth, soft-body simulation is globally OFF by default (the
 * shared g_sim_enabled gate).  Creating a body still succeeds when disabled
 * (authoring works) but no nodes advance — baseline-friendly, zero regression.
 *
 * Layer: Physics (Layer 4 middleware).  Public.  POD descs only; no Bullet
 * types leak across this header.
 */

#ifndef JCE_SOFTBODY_H
#define JCE_SOFTBODY_H

#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

/* Handle into the shared cloth/soft-body slot pool.  1-based; 0 == invalid.
 * Interchangeable with JceClothHandle internally (same slot space). */
typedef uint32_t JceSoftBodyHandle;
#define JCE_SOFTBODY_INVALID ((JceSoftBodyHandle)0)

/* Closed pressurised ellipsoid soft body.  POD. */
typedef struct JceSoftBodyDesc {
    jce_vec3 center;            /* world-space centre of the ellipsoid          */
    jce_vec3 radius;            /* per-axis radii (a sphere has equal x/y/z)    */
    int      resolution;        /* node density (e.g. 64..256); higher = softer */
    float    mass;              /* kg, distributed across the shell nodes       */
    float    pressure;          /* Bullet kPR; >0 resists volume loss (squish)  */
    float    stiffness_linear;  /* 0..1 (Bullet material kLST)                  */
    float    stiffness_volume;  /* 0..1 (Bullet material kVST)                  */
    float    damping;           /* 0..1 (Bullet kDP)                            */
    float    friction;          /* 0..1 (Bullet kDF — dynamic friction)         */
    bool     self_collision;    /* expensive cluster self-collision; off = cheap*/
} JceSoftBodyDesc;

/* Create a closed pressurised ellipsoid.  Returns JCE_SOFTBODY_INVALID on
 * failure (NULL/invalid desc, OOM).  Creation works even when simulation is
 * globally disabled (the body just will not advance). */
JCE_API JceSoftBodyHandle jce_softbody_create_ellipsoid(const JceSoftBodyDesc *desc);

/* Destroy a soft body.  No-op on JCE_SOFTBODY_INVALID / already-destroyed. */
JCE_API void jce_softbody_destroy(JceSoftBodyHandle h);

/* Number of shell nodes (0 for an invalid handle). */
JCE_API uint32_t jce_softbody_node_count(JceSoftBodyHandle h);

/* Copy current node positions into out_xyz (>= node_count*3 floats).
 * Returns false on invalid handle / insufficient capacity. */
JCE_API bool jce_softbody_get_positions(JceSoftBodyHandle h,
                                        float *out_xyz,
                                        uint32_t out_capacity_floats);

/* Mean of all node positions = the visible centroid.  False on invalid handle. */
JCE_API bool jce_softbody_get_center(JceSoftBodyHandle h, jce_vec3 *out_center);

/* Axis-aligned bounds over all node positions.  False on invalid handle. */
JCE_API bool jce_softbody_get_aabb(JceSoftBodyHandle h,
                                   jce_vec3 *out_min,
                                   jce_vec3 *out_max);

/* ── Static collision proxies ────────────────────────────────────────
 * Add a zero-mass static box to the soft world so soft bodies can rest /
 * bounce on it.  Returns a proxy id, or UINT32_MAX on failure. */
JCE_API uint32_t jce_softbody_add_static_box(jce_vec3 center,
                                             jce_vec3 half_extents);

/* The other two primitive proxies.  A box was the only shape a soft body
 * could touch, so a sphere or capsule collider in the scene simply did not
 * exist for cloth -- it fell through them, and nothing reported it.
 *
 * `height` is the capsule's TOTAL length including both hemispheres, matching
 * JceCapsuleColliderComponent and the character capsule; Bullet wants the
 * cylinder section, so the conversion is done inside.  `axis` is 0=X, 1=Y,
 * 2=Z, the same encoding the component uses.
 *
 * Still PRIMITIVES ONLY: convex-hull, mesh and terrain colliders remain
 * invisible to the soft world, and so does every DYNAMIC body -- the soft
 * simulation runs in a separate btSoftRigidDynamicsWorld and rigid-vs-soft is
 * one-way by construction. Both are named here so the next reader does not
 * have to rediscover the boundary from the absence of a function. */
JCE_API uint32_t jce_softbody_add_static_sphere(jce_vec3 center, float radius);
JCE_API uint32_t jce_softbody_add_static_capsule(jce_vec3 center, float radius,
                                                 float height, int axis);

/* Remove + free ALL static proxies (call on scene unload).  Idempotent. */
JCE_API void jce_softbody_clear_statics(void);

/* ── Neutral simulation controls (forward to the shared soft world) ───
 * Callers can drive the shared world without naming the cloth API.  These
 * touch the SAME global gate / SAME world step the cloth uses. */
JCE_API void jce_softbody_set_simulation_enabled(bool enabled);
JCE_API bool jce_softbody_is_simulation_enabled(void);

/* Step the shared soft world by dt seconds (same step cloth uses).  Called
 * automatically by jce_physics_step() via jce_cloth_step_(); exposed for
 * consumers running their own cadence.  Trailing underscore = private API. */
JCE_API void jce_softbody_step_(float dt);

JCE_EXTERN_C_END

#endif /* JCE_SOFTBODY_H */
