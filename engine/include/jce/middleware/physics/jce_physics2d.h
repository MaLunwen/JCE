/*
 * jce_physics2d.h  2D rigid-body physics (Box2D 3.x backend).
 *
 * Thin wrapper around Box2D v3 for 2D games and UI physics.
 * Registered as an optional subsystem (priority 101).
 *
 * Thread safety: NOT thread-safe.  Call from the main thread only.
 *
 * Layer: Physics (Layer 3 — optional subsystem).
 */

#ifndef JCE_PHYSICS2D_H
#define JCE_PHYSICS2D_H


#include <jce/middleware/physics/jce_physics_types.h>
#include <jce/os/core/jce_defs.h>
#include <jce/os/core/jce_math.h>

JCE_EXTERN_C_BEGIN

/* ================================================================== */
/* World lifecycle                                                     */
/* ================================================================== */

typedef struct JcePhysics2D JcePhysics2D;

typedef struct {
    jce_vec2 gravity;          /* default: (0, -9.81) */
    uint32_t max_bodies;       /* default: 4096 */
} JcePhysics2DDesc;

JCE_API JcePhysics2D *jce_physics2d_create(const JcePhysics2DDesc *desc);
JCE_API void          jce_physics2d_destroy(JcePhysics2D *world);

/* ================================================================== */
/* Simulation                                                          */
/* ================================================================== */

/* Step the 2D simulation forward by dt seconds. */
JCE_API void jce_physics2d_step(JcePhysics2D *world, float dt);

/* ================================================================== */
/* 2D body shapes                                                      */
/* ================================================================== */

typedef enum {
    JCE_SHAPE2D_BOX     = 0,
    JCE_SHAPE2D_CIRCLE  = 1,
    JCE_SHAPE2D_CAPSULE = 2,
    JCE_SHAPE2D_SEGMENT = 3
} JceShape2DType;

/* ================================================================== */
/* 2D rigid bodies                                                     */
/* ================================================================== */

typedef struct {
    JceBodyType    type;
    JceShape2DType shape;
    jce_vec2       position;
    float          angle;           /* radians */
    jce_vec2       half_extents;    /* box: half-size; circle: (radius,0);
                                       capsule: (radius, half_length) */
    float          mass;            /* 0 = static */
    float          friction;        /* default: 0.5 */
    float          restitution;     /* default: 0.0 */
    float          linear_damping;
    float          angular_damping;
    bool           fixed_rotation;
} JceBody2DDesc;

JCE_API JceBodyHandle jce_physics2d_body_create(JcePhysics2D *world, const JceBody2DDesc *desc);
JCE_API void          jce_physics2d_body_destroy(JcePhysics2D *world, JceBodyHandle body);

/* Create a body with NO shapes attached.  Callers compose multi-shape
 * bodies (e.g. tilemap colliders) by attaching boxes afterwards with
 * jce_physics2d_body_add_box(). */
JCE_API JceBodyHandle jce_physics2d_body_create_empty(JcePhysics2D *world,
                                                      jce_vec2 pos, float angle,
                                                      JceBodyType type);

/* Attach one axis-aligned box shape (offset in body-local space) to an
 * existing body.  `sensor` shapes detect overlaps without collision
 * response.  Returns false on invalid world/body/extents. */
JCE_API bool          jce_physics2d_body_add_box(JcePhysics2D *world,
                                                 JceBodyHandle body,
                                                 jce_vec2 center_local,
                                                 jce_vec2 half_extents,
                                                 float friction,
                                                 float restitution,
                                                 bool sensor);

/* ================================================================== */
/* 2D body state queries                                               */
/* ================================================================== */

void     jce_physics2d_body_get_transform(const JcePhysics2D *world, JceBodyHandle body,
                                          jce_vec2 *out_pos, float *out_angle);
void     jce_physics2d_body_set_transform(JcePhysics2D *world, JceBodyHandle body,
                                          jce_vec2 pos, float angle);

JCE_API jce_vec2 jce_physics2d_body_get_velocity(const JcePhysics2D *world, JceBodyHandle body);
JCE_API void     jce_physics2d_body_set_velocity(JcePhysics2D *world, JceBodyHandle body, jce_vec2 vel);

/* ================================================================== */
/* 2D forces & impulses                                                */
/* ================================================================== */

JCE_API void jce_physics2d_body_apply_force(JcePhysics2D *world, JceBodyHandle body, jce_vec2 force);
JCE_API void jce_physics2d_body_apply_impulse(JcePhysics2D *world, JceBodyHandle body, jce_vec2 impulse);
JCE_API void jce_physics2d_body_apply_torque(JcePhysics2D *world, JceBodyHandle body, float torque);

/* ================================================================== */
/* 2D ray casting                                                      */
/* ================================================================== */

typedef struct {
    bool          hit;
    jce_vec2      point;
    jce_vec2      normal;
    float         fraction;
    JceBodyHandle body;
} JceRaycast2DResult;

JceRaycast2DResult jce_physics2d_raycast(const JcePhysics2D *world,
                                         jce_vec2 origin, jce_vec2 direction,
                                         float max_distance);

/* ================================================================== */
/* 2D joints (Distance / Hinge / Spring)                               */
/* ================================================================== */

/* Joint kinds (mirrors JCE_JOINT_2D_* in jce_scene.h, but the engine
 * physics layer stays scene-agnostic so it carries its own enum). */
typedef enum {
    JCE_PHYSICS2D_JOINT_DISTANCE = 0, /* rigid distance (fixed length)  */
    JCE_PHYSICS2D_JOINT_HINGE    = 1, /* revolute (motor + angle limit) */
    JCE_PHYSICS2D_JOINT_SPRING   = 2  /* distance joint with a spring   */
} JcePhysics2DJointKind;

/* One 2D joint between two bodies.  body_b INVALID anchors body_a to the
 * world (an implicit static ground body the joint owns).  Anchors are in
 * each body's LOCAL space.  Angles are radians; frequency is Hz.  Distance
 * is the rest length for DISTANCE/SPRING. */
typedef struct {
    int           kind;             /* JcePhysics2DJointKind             */
    JceBodyHandle body_a;
    JceBodyHandle body_b;           /* JCE_BODY_INVALID => world anchor  */
    jce_vec2      anchor_a;         /* local anchor on body_a            */
    jce_vec2      anchor_b;         /* local anchor on body_b / world    */
    float         distance;         /* DISTANCE/SPRING rest length       */
    float         frequency_hz;     /* SPRING stiffness (Hz)             */
    float         damping_ratio;    /* SPRING damping (non-dimensional)  */
    bool          use_motor;        /* HINGE                             */
    float         motor_speed_rad_s;/* HINGE                             */
    float         motor_max_torque; /* HINGE (N·m)                       */
    bool          use_limits;       /* HINGE                             */
    float         lower_angle_rad;  /* HINGE limit                       */
    float         upper_angle_rad;  /* HINGE limit                       */
} JcePhysics2DJointDesc;

/* Create a 2D joint.  Returns JCE_CONSTRAINT_INVALID on failure (NULL
 * world/desc, invalid body_a, or joint-pool exhaustion).  The handle maps
 * back to the b2JointId so it can be torn down with jce_physics2d_joint_
 * destroy; the world also auto-destroys all joints on jce_physics2d_destroy. */
JCE_API JceConstraintHandle JCE_CALL jce_physics2d_joint_create(JcePhysics2D *world,
                                                                const JcePhysics2DJointDesc *desc);

/* Destroy a 2D joint created above.  Safe on INVALID / already-freed
 * handles (idempotent no-op). */
JCE_API void JCE_CALL jce_physics2d_joint_destroy(JcePhysics2D *world,
                                                  JceConstraintHandle joint);

/* ================================================================== */
/* Debug                                                               */
/* ================================================================== */

JCE_API uint32_t jce_physics2d_body_count(const JcePhysics2D *world);

JCE_EXTERN_C_END

#endif /* JCE_PHYSICS2D_H */
