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

/* Vehicle controller handle. */
typedef struct { uint32_t idx; } JceVehicleHandle;
#define JCE_VEHICLE_INVALID ((JceVehicleHandle){ UINT32_MAX })

static inline bool jce_vehicle_valid(JceVehicleHandle h) { return h.idx != UINT32_MAX; }

/* Maximum wheels per vehicle (front 2 + rear 2 typical; allow 6×6 trucks). */
#define JCE_VEHICLE_MAX_WHEELS 8

/* ================================================================== */
/* Shape descriptors                                                   */
/* ================================================================== */

typedef enum {
    JCE_SHAPE_BOX           = 0,
    JCE_SHAPE_SPHERE        = 1,
    JCE_SHAPE_CAPSULE       = 2,
    JCE_SHAPE_PLANE         = 3,
    JCE_SHAPE_CONVEX_HULL   = 4,  /* point cloud → convex hull (dynamic-safe) */
    JCE_SHAPE_TRIANGLE_MESH = 5,  /* exact triangle soup (static bodies only) */
    JCE_SHAPE_COMPOUND      = 6   /* container of child shapes (per-object) */
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

#define JCE_COLLISION_DEFAULT_GROUP  0x00000001u
#define JCE_COLLISION_ALL_MASK      0xFFFFFFFFu

/* ================================================================== */
/* Contact / collision event                                           */
/* ================================================================== */

/* Forward-declared in jce_physics_debug.h — kept as int8_t storage here
 * so this POD header stays free of additional includes. */
typedef int8_t JceContactEventTypeRaw;

typedef struct {
    JceBodyHandle body_a;
    JceBodyHandle body_b;
    float         normal[3];     /* contact normal (A→B) */
    float         point[3];      /* world-space contact point */
    float         depth;         /* penetration depth */
    bool          is_trigger;    /* true if one of the bodies is a trigger */

    /* --- P3-C.5 additive fields --------------------------------- */
    /* Holds a JceContactEventType (see jce_physics_debug.h).  Stored
     * as int8_t to keep this header dependency-free.  Defaults to
     * JCE_CONTACT_BEGIN (0) for legacy callers. */
    JceContactEventTypeRaw type;
    uint8_t       _pad[3];

    /* Opaque per-body tags (e.g. ecs_entity_t cast to u64).  Populated
     * from jce_physics_body_set_entity(); 0 if unset. */
    uint64_t      entity_a;
    uint64_t      entity_b;
} JceContactEvent;

/* Callback invoked on collision begin / end. */
typedef void (*jce_contact_fn)(const JceContactEvent *event, void *userdata);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_TYPES_H */
