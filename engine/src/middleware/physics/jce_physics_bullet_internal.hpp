/*
 * jce_physics_bullet_internal.hpp — struct JceBulletWorld, shared by the
 * Bullet backend's translation units.
 *
 * WHY IT IS A .hpp AND NOT IN jce_physics_internal.h.  That header is
 * included by jce_physics.c and jce_physics_joint_query.c, which are compiled
 * as C; this struct's members are btDiscreteDynamicsWorld*, btRigidBody* and
 * friends.  The C consumers only ever need the forward declaration that
 * header already carries, so the definition belongs in a C++-only sibling
 * rather than behind an #ifdef __cplusplus in a shared one.
 *
 * WHY IT EXISTS AT ALL.  jce_physics_bullet.cpp was 3,164 lines, past the
 * 3,000-line cap and frozen at the size gate's baseline.  Moving the vehicle
 * controller into its own TU needed this struct on both sides.
 */

#ifndef JCE_PHYSICS_BULLET_INTERNAL_HPP
#define JCE_PHYSICS_BULLET_INTERNAL_HPP

#include "jce_physics_internal.h"
#include "os/core/jce_memory.h"
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>
#include <BulletCollision/CollisionDispatch/btInternalEdgeUtility.h>
#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>
#include <BulletDynamics/Character/btKinematicCharacterController.h>
#include <BulletDynamics/Vehicle/btRaycastVehicle.h>
#include <cstdint>
#include <cstring>

static inline btVector3 to_bt(jce_vec3 v) { return btVector3(v.x, v.y, v.z); }

static inline jce_vec3 from_bt_v3(const btVector3 &v)
{
    jce_vec3 r;
    r.x = v.x();
    r.y = v.y();
    r.z = v.z();
    return r;
}

static inline btQuaternion to_bt_q(jce_quat q)
{
    return btQuaternion(q.x, q.y, q.z, q.w);
}

static inline jce_quat from_bt_q(const btQuaternion &q)
{
    jce_quat r;
    r.x = q.x();
    r.y = q.y();
    r.z = q.z();
    r.w = q.w();
    return r;
}

/* ================================================================== */
/* Generation-packed handles (D-gen-handles → full handles)            */
/*                                                                    */
/* A handle is a uint32_t partitioned as [ gen | slot ] using the     */
/* same field widths as JceBodyHandle (see jce_physics_types.h):       */
/* low JCE_BODY_HANDLE_INDEX_BITS = pool slot, high bits = generation. */
/* The public create funcs hand back encode(slot, generations[slot]);  */
/* every accessor below treats its incoming uint32_t as a packed       */
/* handle, splits out the slot, and rejects it unless the slot is in   */
/* range, alive, and still on the handle's generation.  A bare slot    */
/* index (gen 0) decodes to itself and matches a never-recycled slot,  */
/* so pre-existing handles keep working.                               */
/* ================================================================== */

static inline uint32_t handle_encode(uint32_t slot, uint32_t gen)
{
    return jce_body_handle_pack(slot, gen).idx;
}

static inline uint32_t handle_slot(uint32_t handle)
{
    JceBodyHandle h; h.idx = handle;
    return jce_body_handle_slot(h);
}

static inline uint32_t handle_gen(uint32_t handle)
{
    JceBodyHandle h; h.idx = handle;
    return jce_body_handle_gen(h);
}

/* resolve_body() needs JceBulletWorld's layout — defined just after the
   struct below. */
static inline uint32_t resolve_body(JceBulletWorld *bw, uint32_t handle);

/* btCollisionObject user-INDEX sentinel marking a raycast-vehicle CHASSIS.  A
 * chassis is created by the vehicle API (not jce_bullet_body_create), so its
 * user-POINTER is NOT a generation-packed body handle — feeding it to the
 * contact dispatch (which decodes user-pointers as body handles) aliases a real
 * body slot and, once that slot is recycled by a spawn, resolves to a dead body
 * and crashes.  The contact dispatch skips objects carrying this tag.  Default
 * btCollisionObject user-index is -1, so normal bodies never match it. */
#define JCE_BULLET_VEHICLE_CHASSIS_USERINDEX 0x5645  /* 'VE' */

/* ================================================================== */
/* World definition                                                    */
/* ================================================================== */

/* Per-character movement-feel state (parallel to char_bodies). The
 * grounded probe is refreshed once per move() and cached here so the
 * same fixed tick's jump/animation queries don't re-raycast. */
struct JceBulletCharFeel {
    float     accel;          /* m/s^2 toward commanded velocity (ground) */
    float     air_control;    /* 0..1 accel scale while airborne */
    float     step_height;    /* max auto-step (m) */
    float     max_slope_cos;  /* cos(max walkable slope) */
    /* Capsule's broadphase filter.  Cached per character because the ground /
     * step / head-clearance probes are separate rayTests that do NOT inherit
     * the body's proxy, and must not read the world through another filter. */
    int       filter_group, filter_mask;
    bool      grounded;       /* cached probe result: WALKABLE contact */
    bool      touching;       /* raw probe contact (any steepness) */
    bool      probe_valid;    /* probe ran at least once this session */
    bool      jumping;        /* jump() fired; cleared on next grounded */
    btVector3 ground_normal;
    /* The body under the feet this tick, and its velocity AT THE CONTACT
     * POINT -- not its linear velocity, so a platform that rotates carries the
     * character tangentially rather than only when it translates.  Both are
     * cleared whenever the probe is not grounded, and that is what makes the
     * carry stop at the edge instead of persisting into the fall. */
    const btCollisionObject *ground_body;
    btVector3                platform_vel;
};

struct JceBulletWorld {
    /* Bullet pipeline objects (owned, deleted in reverse order).
     *
     * Field static types are the common base classes so the same slots can
     * hold either the single-threaded objects or, when JCE_PHYSICS_MT is
     * compiled in and requested, their "Mt" subclasses:
     *   dispatcher : btCollisionDispatcher  | btCollisionDispatcherMt
     *   solver     : btSequentialImpulseConstraintSolver | btConstraintSolverPoolMt
     *   world      : btDiscreteDynamicsWorld | btDiscreteDynamicsWorldMt
     * All three subclasses derive from the base type stored here, and every
     * method we call (stepSimulation, etc.) is virtual, so the rest of the
     * back-end is identical regardless of the path taken at create time. */
    btDefaultCollisionConfiguration  *config;
    btCollisionDispatcher            *dispatcher;
    btDbvtBroadphase                 *broadphase;
    btConstraintSolver               *solver;
    btDiscreteDynamicsWorld          *world;
    bool                              multithreaded;  /* true only on the MT path */

    /* Body / shape pool (parallel arrays). */
    btRigidBody      **bodies;
    btCollisionShape  **shapes;
    bool               *alive;
    /* Per-body auxiliary ownership for compound / mesh bodies.  NULL for
       primitive bodies.  owned_shapes holds compound child shapes to be
       deleted; owned_meshes holds the striding mesh interfaces backing
       btBvhTriangleMeshShape children (must outlive the shape). */
    btAlignedObjectArray<btCollisionShape *>        **owned_shapes;
    btAlignedObjectArray<btStridingMeshInterface *> **owned_meshes;
    /* Per-slot generation counter (D-gen-handles).  Bumped on destroy so
       a handle minted from an older generation can be detected as stale.
       Starts at 0, so a bare slot index (gen 0) handle remains valid. */
    uint32_t           *generations;
    uint32_t            capacity;
    uint32_t            count;
    uint32_t            alloc_cursor;  /* rotating free-slot search hint → mass-spawn is O(1) amortized, not O(n^2) */

    /* Constraint pool. */
    btTypedConstraint **constraints;
    bool               *con_alive;
    /* Per-constraint joint feedback, indexed with `constraints`.
     *
     * enableFeedback(true) makes Bullet accumulate m_appliedImpulse -- ONE
     * combined magnitude with no angular part, which is exactly why a torque
     * break could not be built on it.  The separable force and torque only
     * appear if the constraint has storage to write them into, and that
     * storage must outlive every solver step, so it is owned here rather than
     * by the caller.  Allocated with the arrays it is indexed by so the two
     * lifetimes cannot diverge. */
    btJointFeedback    *con_feedback;
    uint32_t            con_capacity;
    uint32_t            con_count;
    uint32_t            con_alloc_cursor;

    /* Character controller pool. Now a DYNAMIC rigid-body capsule (char_bodies)
     * with locked rotation, velocity-driven horizontally — the solver resolves
     * character↔crate↔crate↔floor together, so standing on (stacked) dynamic
     * bodies is stable with no jitter. The legacy kinematic controller/ghost
     * arrays are retired (kept NULL) so the rest of the pool bookkeeping and
     * any stale references stay valid. */
    btKinematicCharacterController **characters;   /* unused (legacy, NULL) */
    btPairCachingGhostObject       **ghosts;       /* unused (legacy, NULL) */
    btRigidBody                    **char_bodies;  /* the dynamic capsule */
    float                           *char_jump;    /* per-character jump speed */
    btConvexShape                  **char_shapes;
    bool                            *char_alive;
    JceBulletCharFeel               *char_feel;    /* movement-feel state */
    uint32_t                         char_capacity;
    uint32_t                         char_count;
    uint32_t                         char_alloc_cursor;

    /* Vehicle controller pool (parallel arrays).  Each slot owns a
     * chassis btRigidBody, the box collision shape, the raycaster,
     * and the btRaycastVehicle.  Per-vehicle drive state is cached
     * here so set_input() can apply the same force to every wheel
     * without the user having to track wheel indices. */
    btRaycastVehicle              **vehicles;
    btDefaultVehicleRaycaster     **vehicle_raycasters;
    btRigidBody                   **vehicle_chassis;
    btCollisionShape              **vehicle_chassis_shapes;
    bool                           *vehicle_alive;
    float                          *vehicle_max_engine;
    float                          *vehicle_max_brake;
    float                          *vehicle_max_steer;
    uint32_t                        vehicle_capacity;
    uint32_t                        vehicle_count;
    uint32_t                        vehicle_alloc_cursor;

    /* Contact callbacks forwarded to the C layer. */
    jce_bullet_contact_fn contact_begin_fn;
    void                 *contact_begin_ud;
    jce_bullet_contact_fn contact_end_fn;
    void                 *contact_end_ud;

    /* Per-body sleeping thresholds applied at create time.  <= 0 means
       "leave Bullet's per-body default" (linear 0.8, angular 1.0). */
    float linear_sleep_threshold;
    float angular_sleep_threshold;
};


/* Add a finished constraint to the world + registry, WITH its joint feedback.
 *
 * Declared here rather than left static in jce_physics_bullet.cpp because both
 * constraint creators must reach it and they now live in a different TU from
 * the feedback storage they depend on.  Defined in
 * jce_physics_bullet_con_query.cpp beside the two queries that READ what this
 * installs -- installing and reading are one mechanism, and keeping them in
 * separate files is exactly how they drifted apart once already. */
void jce_bullet_con_register(JceBulletWorld *bw, uint32_t idx,
                             btTypedConstraint *con, bool disable_collision);


/* ── Contact hook (jce_physics_bullet_contact.cpp) ─────────────────────── */

/* Install Bullet's single global contact-added hook, chaining to whatever
 * already owned it.  Idempotent; called once per world create.  Everything
 * that influences a contact -- internal-edge smoothing, the authored material
 * combine -- runs inside it. */
void jce_bullet_install_contact_hook(void);

/* Pack the two JCE combine modes into the value carried on a body's
 * btCollisionObject::setUserIndex2.  ZERO means "no material was applied",
 * which has to stay distinct from AVERAGE -- AVERAGE is a real authored
 * choice, and a default-constructed body must not be mistaken for one. */
int jce_bullet_pack_combine(JcePhysicsCombine f, JcePhysicsCombine r);

#endif /* JCE_PHYSICS_BULLET_INTERNAL_HPP */
