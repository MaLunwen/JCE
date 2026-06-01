/*
 * jce_collider_asset.h  Serialize / deserialize / instantiate cooked
 *                       compound colliders.
 *
 * The cooker (jce_collider_cook.h) produces a JceCookedCollider — a small
 * POD tree of child shapes. This module turns that tree into a compact,
 * deterministic byte blob suitable for storing inside a .pak / save
 * snapshot, reads it back, and instantiates it into a live physics body.
 *
 * Determinism: the blob is little-endian with a fixed field order, so the
 * same cooked input always serializes to identical bytes (§10.5).
 */

#ifndef JCE_COLLIDER_ASSET_H
#define JCE_COLLIDER_ASSET_H

#include <stdbool.h>
#include <stdint.h>

#include <jce/os/core/jce_defs.h>
#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_collider_cook.h>

JCE_EXTERN_C_BEGIN

/* Body-level parameters used when instantiating a cooked collider.
 * (The cooked tree only describes shape; placement / mass / filtering
 * come from the owning entity.) */
typedef struct {
    JceBodyType type;            /* static / dynamic / kinematic           */
    jce_vec3    position;        /* body origin in world space             */
    jce_quat    rotation;        /* body rotation                          */
    float       mass;            /* 0 = static                             */
    float       friction;        /* default 0.5 when <= 0                  */
    float       restitution;
    float       linear_damping;
    float       angular_damping;
    uint32_t    collision_group; /* default group when 0                   */
    uint32_t    collision_mask;  /* default all when 0                     */
    bool        is_trigger;
} JceColliderInstanceDesc;

/* Serialize a cooked collider into a freshly allocated byte blob.
 * On success *out_bytes (jce_malloc'd, free with jce_free) and *out_size
 * are set. Returns false on bad input or allocation failure. */
JCE_API bool jce_collider_serialize(const JceCookedCollider *c,
                                    void                   **out_bytes,
                                    uint32_t                *out_size);

/* Rebuild a cooked collider from a blob produced by jce_collider_serialize.
 * `out` receives owned arrays; release with jce_collider_cooked_free().
 * Returns false on truncated / malformed input. */
JCE_API bool jce_collider_deserialize(const void        *bytes,
                                      uint32_t           size,
                                      JceCookedCollider *out);

/* Instantiate a cooked collider as a compound body in `world`.
 * Returns JCE_BODY_INVALID on failure. */
JCE_API JceBodyHandle jce_collider_instantiate(JcePhysicsWorld               *world,
                                               const JceCookedCollider       *c,
                                               const JceColliderInstanceDesc *desc);

JCE_EXTERN_C_END

#endif /* JCE_COLLIDER_ASSET_H */
