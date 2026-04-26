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


#include <jce/os/core/jce_defs.h>

#include <stdbool.h>
#include <stdint.h>

JCE_EXTERN_C_BEGIN

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

/* Constraint handle. */
typedef struct { uint32_t idx; } JceConstraintHandle;
#define JCE_CONSTRAINT_INVALID ((JceConstraintHandle){ UINT32_MAX })

static inline bool jce_constraint_valid(JceConstraintHandle h) { return h.idx != UINT32_MAX; }

/* Character controller handle. */
typedef struct { uint32_t idx; } JceCharacterHandle;
#define JCE_CHARACTER_INVALID ((JceCharacterHandle){ UINT32_MAX })

static inline bool jce_character_valid(JceCharacterHandle h) { return h.idx != UINT32_MAX; }

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

typedef enum {
    JCE_CONSTRAINT_POINT2POINT = 0,
    JCE_CONSTRAINT_HINGE       = 1,
    JCE_CONSTRAINT_SLIDER      = 2,
    JCE_CONSTRAINT_GENERIC6DOF = 3
} JceConstraintType;

/* ================================================================== */
/* Collision layer defaults                                            */
/* ================================================================== */

#define JCE_COLLISION_DEFAULT_GROUP  0x0001
#define JCE_COLLISION_ALL_MASK      0xFFFF

/* ================================================================== */
/* Contact / collision event                                           */
/* ================================================================== */

typedef struct {
    JceBodyHandle body_a;
    JceBodyHandle body_b;
    float         normal[3];     /* contact normal (A→B) */
    float         point[3];      /* world-space contact point */
    float         depth;         /* penetration depth */
    bool          is_trigger;    /* true if one of the bodies is a trigger */
} JceContactEvent;

/* Callback invoked on collision begin / end. */
typedef void (*jce_contact_fn)(const JceContactEvent *event, void *userdata);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_TYPES_H */
