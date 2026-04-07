/*
 * jce_physics_types.h  Physics handle and configuration types.
 *
 * Shared by both 2D (Box2D) and 3D (Bullet) physics modules.
 * No implementation details leak through this header.
 *
 * Layer: Physics (Layer 3).
 */

#ifndef JCE_PHYSICS_TYPES_H
#define JCE_PHYSICS_TYPES_H

#include <stdint.h>
#include <stdbool.h>

#ifdef __cplusplus
extern "C" {
#endif

/* ================================================================== */
/* Handle types                                                        */
/* ================================================================== */

/* Rigid body handle — index into internal pool. */
typedef struct { uint32_t idx; } JceBodyHandle;
#define JCE_BODY_INVALID ((JceBodyHandle){ UINT32_MAX })

static inline bool jce_body_valid(JceBodyHandle h) { return h.idx != UINT32_MAX; }

/* Collider / shape handle. */
typedef struct { uint32_t idx; } JceColliderHandle;
#define JCE_COLLIDER_INVALID ((JceColliderHandle){ UINT32_MAX })

static inline bool jce_collider_valid(JceColliderHandle h) { return h.idx != UINT32_MAX; }

/* ================================================================== */
/* Shape descriptors                                                   */
/* ================================================================== */

typedef enum {
    JCE_SHAPE_BOX      = 0,
    JCE_SHAPE_SPHERE   = 1,
    JCE_SHAPE_CAPSULE  = 2,
    JCE_SHAPE_PLANE    = 3
} JceShapeType;

typedef enum {
    JCE_BODY_STATIC    = 0,
    JCE_BODY_DYNAMIC   = 1,
    JCE_BODY_KINEMATIC = 2
} JceBodyType;

/* ================================================================== */
/* Contact / collision event                                           */
/* ================================================================== */

typedef struct {
    JceBodyHandle body_a;
    JceBodyHandle body_b;
    float         normal[3];     /* contact normal (A→B) */
    float         point[3];      /* world-space contact point */
    float         depth;         /* penetration depth */
} JceContactEvent;

/* Callback invoked on collision begin / end. */
typedef void (*jce_contact_fn)(const JceContactEvent *event, void *userdata);

#ifdef __cplusplus
}
#endif

#endif /* JCE_PHYSICS_TYPES_H */
