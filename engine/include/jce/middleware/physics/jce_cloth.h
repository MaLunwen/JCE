/*
 * jce_cloth.h  Cloth / soft-body simulation (Bullet btSoftBody backend).
 *
 * Mass-spring cloth patch built from a regular u×v grid.  Wraps Bullet's
 * btSoftRigidDynamicsWorld behind the C ABI; the soft-world is an
 * isolated *secondary* world (it does not replace the rigid-only
 * btDiscreteDynamicsWorld used by jce_physics).  Rationale: keeping the
 * rigid world unchanged guarantees zero behavioural drift for existing
 * rigid bodies.  Cross-world anchoring is supported via appendAnchor
 * (Bullet records the rigid pointer and applies forces only — both
 * sides keep their own broadphase / dispatcher).
 *
 * HW-caps gate: cloth simulation is globally OFF by default.  The Render
 * Pipeline Asset (P3-E.4) opts cloth in for MID+/HIGH/ULTRA tiers via
 * `enable_cloth`.  When disabled, cloth_create still succeeds (so
 * authoring still works in the editor) but no nodes are stepped.
 *
 * Layer: Physics (Layer 4 middleware).  Public.
 */

#ifndef JCE_CLOTH_H
#define JCE_CLOTH_H

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

typedef uint32_t JceClothHandle;
#define JCE_CLOTH_INVALID ((JceClothHandle)0)

/* Spec uses "JcePhysicsBody" — alias to the existing handle type. */
typedef JceBodyHandle JcePhysicsBody;

typedef struct JceClothDesc {
    /* Source: regular grid patch defined by 4 corners + resolution.
     * (Mesh-from-asset variant is a documented follow-up.) */
    jce_vec3 corner_00;
    jce_vec3 corner_10;
    jce_vec3 corner_01;
    jce_vec3 corner_11;
    uint32_t res_u;            /* >=2 */
    uint32_t res_v;            /* >=2 */

    float    mass_total;       /* kg, distributed across nodes */
    float    stiffness_linear; /* 0..1 (Bullet "kLST") */
    float    stiffness_angular;/* 0..1 (Bullet "kAST") */
    float    damping;          /* 0..1 (Bullet "kDP") */
    uint32_t iterations;       /* solver iters; default 4 — keep low for baseline */

    /* Pinned indices: vertex indices in row-major (i = v*res_u + u) that are kinematic. */
    const uint32_t *pinned_indices;
    uint32_t        pinned_count;

    bool     self_collision;   /* expensive — off by default */
    bool     wind_enabled;
    jce_vec3 wind_velocity;
} JceClothDesc;

JCE_API JceClothHandle jce_cloth_create(const JceClothDesc *desc);
JCE_API void           jce_cloth_destroy(JceClothHandle h);

/* Update wind dynamically. */
JCE_API void jce_cloth_set_wind(JceClothHandle h, jce_vec3 velocity, bool enabled);

/* Number of nodes (= res_u * res_v). */
JCE_API uint32_t jce_cloth_node_count(JceClothHandle h);

/* Copy current node positions into an out buffer (size >= node_count * 3 floats). */
JCE_API bool jce_cloth_get_positions(JceClothHandle h,
                                     float *out_positions,
                                     uint32_t out_capacity_floats);

/* Connect this cloth's node to a rigid body anchor (e.g. attach cape to character).
 * `body` must belong to the most recently created JcePhysicsWorld (the engine's
 * default rigid world).  v1 limitation: only one rigid world is tracked. */
JCE_API bool jce_cloth_anchor_to_body(JceClothHandle cloth,
                                      uint32_t node_index,
                                      JcePhysicsBody body,
                                      jce_vec3 local_pivot,
                                      bool disable_collision);

/* Globally enable/disable cloth simulation (used by HW-caps gate).
 * When disabled: creation still works, but no node positions advance. */
JCE_API void jce_cloth_set_simulation_enabled(bool enabled);
JCE_API bool jce_cloth_is_simulation_enabled(void);

/* Stats */
JCE_API uint32_t jce_cloth_active_count(void);
JCE_API uint32_t jce_cloth_total_nodes(void);

/* ── Internal-ish ───────────────────────────────────────────────────
 * Step the cloth world by dt seconds.  Called by jce_physics_step()
 * automatically; exposed here so consumers running their own physics
 * cadence can still drive cloth.  Trailing underscore = "private API". */
JCE_API void jce_cloth_step_(float dt);

/* Tear down all soft bodies + the soft world.  Called on engine
 * shutdown by jce_physics_destroy().  Safe to call multiple times. */
JCE_API void jce_cloth_shutdown_(void);

JCE_EXTERN_C_END

#endif /* JCE_CLOTH_H */
