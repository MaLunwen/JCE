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

/* Rigid body handle — index into internal pool.
 *
 * The 32-bit `idx` is partitioned into a low slot index and a high
 * generation counter so a stale handle to a recycled slot can be
 * detected (D-gen-handles).  The split is 20 bits slot (up to ~1M
 * bodies) + 12 bits generation (4096 reuses before wrap).
 *
 * BACKWARD COMPATIBILITY: a bare slot index (generation 0) packs to
 * itself, so handles minted before this scheme — and every code path
 * that still treats `idx` as a raw slot — keep working unchanged.
 * JCE_BODY_INVALID stays UINT32_MAX (all index + all gen bits set) and
 * is filtered by jce_body_valid before any slot/gen extraction. */
#define JCE_BODY_HANDLE_INDEX_BITS 20u
#define JCE_BODY_HANDLE_INDEX_MASK ((1u << JCE_BODY_HANDLE_INDEX_BITS) - 1u)
#define JCE_BODY_HANDLE_GEN_MASK   (0xFFFFFFFFu >> JCE_BODY_HANDLE_INDEX_BITS)

typedef struct { uint32_t idx; } JceBodyHandle;
#define JCE_BODY_INVALID ((JceBodyHandle){ UINT32_MAX })

static inline bool jce_body_valid(JceBodyHandle h) { return h.idx != UINT32_MAX; }

/* Pack a pool slot + generation into a handle.  Slot/gen are masked to
 * their field widths; passing gen 0 yields a bare slot index. */
static inline JceBodyHandle jce_body_handle_pack(uint32_t slot, uint32_t gen)
{
    JceBodyHandle h;
    h.idx = (slot & JCE_BODY_HANDLE_INDEX_MASK) |
            ((gen & JCE_BODY_HANDLE_GEN_MASK) << JCE_BODY_HANDLE_INDEX_BITS);
    return h;
}

/* Extract the pool slot index from a handle (low bits). */
static inline uint32_t jce_body_handle_slot(JceBodyHandle h)
{
    return h.idx & JCE_BODY_HANDLE_INDEX_MASK;
}

/* Extract the generation counter from a handle (high bits). */
static inline uint32_t jce_body_handle_gen(JceBodyHandle h)
{
    return (h.idx >> JCE_BODY_HANDLE_INDEX_BITS) & JCE_BODY_HANDLE_GEN_MASK;
}

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

/*
 * What a 3D RigidBody COMPONENT may say about its own kind.  Deliberately not
 * JceBodyType above, and the difference is the whole reason this exists.
 *
 * JceBodyType's STATIC is 0 -- and 0 is also what a memset'd component carries
 * and what every scene ever written carries, because that field was never
 * parsed and never serialised.  A field encoded that way cannot tell "the
 * author said static" from "nobody said anything", so honouring it would have
 * frozen every dynamic body in the tree.  Not hypothetical: the soft-body
 * static-ground mirror read it that way and therefore treated every dynamic
 * BoxCollider in every scene as immovable ground.
 *
 * AUTO at 0 is what every existing component holds and means exactly what the
 * engine already did -- derive from is_kinematic, then mass.  The three named
 * values are only reachable by an author who picked one.
 *
 * THE NUMBERING IS NOT ARBITRARY, and the first version got it wrong.  I gave
 * it AUTO/STATIC/KINEMATIC/DYNAMIC = 0/1/2/3 on the reasoning that the field
 * is never parsed and never serialised, so no FILE could carry a stale value.
 * That checked the data path and not the code path: C callers set this field
 * through the public setter with JceBodyType constants, and
 * tests/application/test_jce_headless_boot.c does exactly that --
 * `rb.body_type = JCE_BODY_DYNAMIC` (1), which under 0/1/2/3 became STATIC.
 * The body stopped falling.
 *
 * So DYNAMIC and KINEMATIC keep JceBodyType's numbers.  Only STATIC moves, off
 * 0, and 0 was the value that could never be honoured anyway -- it was
 * indistinguishable from "unset", which is the whole reason this enum exists.
 * Every existing caller therefore keeps its meaning:
 *
 *   wrote JCE_BODY_STATIC (0)    -> AUTO, and AUTO is what the engine already
 *                                   did for that component, byte for byte
 *   wrote JCE_BODY_DYNAMIC (1)   -> DYNAMIC, as they asked
 *   wrote JCE_BODY_KINEMATIC (2) -> KINEMATIC, as they asked
 *
 * The 2D sibling keeps JceBodyType: ITS body_type has always been parsed,
 * serialised and shown in a combo, so 0 there really is what the author picked.
 */
typedef enum {
    JCE_RB_KIND_AUTO      = 0,   /* derive: is_kinematic, then mass <= 0 */
    JCE_RB_KIND_DYNAMIC   = 1,   /* == JCE_BODY_DYNAMIC, on purpose */
    JCE_RB_KIND_KINEMATIC = 2,   /* == JCE_BODY_KINEMATIC, on purpose */
    JCE_RB_KIND_STATIC    = 3    /* moved off 0, which meant "unset" */
} JceRigidBodyKind;

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

    /* Impulse this contact applied, N.s, summed over the manifold points.
     * Bullet has had it all along (btManifoldPoint::m_appliedImpulse); the
     * event carried normal, point and depth and no measure of how HARD the
     * hit was, so JceFractureComponent.break_impulse -- authored, serialised,
     * in the Inspector -- had nothing to compare against and fracture stayed
     * script-only.  APPENDED, never inserted. */
    float         applied_impulse;
} JceContactEvent;

/* Callback invoked on collision begin / end. */
typedef void (*jce_contact_fn)(const JceContactEvent *event, void *userdata);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS_TYPES_H */
