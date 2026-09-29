/*
 * jce_physics_bullet.cpp  C++ Bullet3 back-end for jce_physics.
 *
 * Wraps btDiscreteDynamicsWorld behind the extern "C" bridge declared
 * in jce_physics_internal.h.  Compiled as C++20.
 */

#include <jce/middleware/physics/jce_physics.h>  /* JceCapsuleAxis */
#include "jce_physics_internal.h"
#include "os/core/jce_memory.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
}

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>
#include <BulletCollision/CollisionDispatch/btInternalEdgeUtility.h>
#include <BulletCollision/CollisionShapes/btHeightfieldTerrainShape.h>
#include <BulletDynamics/Character/btKinematicCharacterController.h>
#include <BulletDynamics/Vehicle/btRaycastVehicle.h>

/* --- Opt-in multithreaded solver path (default OFF) -----------------
 *
 * Define JCE_PHYSICS_MT to compile in the parallel Bullet pipeline
 * (btDiscreteDynamicsWorldMt + btCollisionDispatcherMt +
 * btConstraintSolverPoolMt driven by Bullet's built-in task scheduler).
 *
 * This ALSO requires Bullet to be built thread-safe
 * (bt2_thread_locks=True in conanfile.py, which defines BT_THREADSAFE in
 * the Bullet headers/lib).  Without that, the "Mt" classes link but their
 * internal mutexes are no-ops and parallel stepping is unsafe — so we
 * additionally gate the *real* MT pipeline on BT_THREADSAFE and otherwise
 * keep the single-threaded world even when JCE_PHYSICS_MT is set.
 *
 * The MT path is intentionally left OFF by default: its island /
 * constraint ordering is non-deterministic and conflicts with the
 * fixed-timestep determinism the rest of the engine relies on. */
#if defined(JCE_PHYSICS_MT)
#  include <BulletCollision/CollisionDispatch/btCollisionDispatcherMt.h>
#  include <BulletDynamics/Dynamics/btDiscreteDynamicsWorldMt.h>
#  include <BulletDynamics/ConstraintSolver/btSequentialImpulseConstraintSolverMt.h>
#  include <LinearMath/btThreads.h>
#  if BT_THREADSAFE
#    define JCE_PHYSICS_MT_ACTIVE 1
#  else
#    define JCE_PHYSICS_MT_ACTIVE 0
#  endif
#else
#  define JCE_PHYSICS_MT_ACTIVE 0
#endif

#include <cstdint>
#include <cstring>

/* Max vertices kept for a plain CONVEX_HULL child.  Beyond this the hull
   is reduced via btShapeHull so an un-decimated mesh cannot bloat the
   dynamic solver. */
#define JCE_HULL_MAX_VERTS 64u

/* ================================================================== */
/* Allocator integration                                               */
/*                                                                    */
/* Bullet exposes btAlignedAllocSetCustom* hooks; once installed every */
/* internal `new` (via BT_DECLARE_ALIGNED_ALLOCATOR) routes through    */
/* our engine allocator.  Aligned variant over-allocates and stashes  */
/* the original pointer one slot before the user pointer.             */
/* ================================================================== */

namespace {
void *bt_jce_alloc_unaligned(size_t size)
{
    return jce_malloc(size);
}

void  bt_jce_free_unaligned(void *ptr)
{
    jce_free(ptr);
}

void *bt_jce_alloc_aligned(size_t size, int alignment)
{
    if (alignment < (int)sizeof(void *)) alignment = (int)sizeof(void *);
    size_t total = size + alignment + sizeof(void *);
    void *raw = jce_malloc(total);
    if (!raw) return nullptr;
    uintptr_t base = reinterpret_cast<uintptr_t>(raw) + sizeof(void *);
    uintptr_t aligned = (base + alignment - 1) & ~(uintptr_t)(alignment - 1);
    void **slot = reinterpret_cast<void **>(aligned) - 1;
    *slot = raw;
    return reinterpret_cast<void *>(aligned);
}

void  bt_jce_free_aligned(void *ptr)
{
    if (!ptr) return;
    void *raw = reinterpret_cast<void **>(ptr)[-1];
    jce_free(raw);
}

} /* namespace */

/* External-linkage allocator installer (declared in jce_physics_internal.h).
 * Kept out of the anonymous namespace so the cloth TU can install the hook
 * before it allocates its own secondary Bullet world.  References the
 * anon-namespace trampolines above (same TU). */
void jce_bullet_install_allocator_(void)
{
    static bool installed = false;
    if (installed) return;
    btAlignedAllocSetCustom(&bt_jce_alloc_unaligned, &bt_jce_free_unaligned);
    btAlignedAllocSetCustomAligned(&bt_jce_alloc_aligned, &bt_jce_free_aligned);
    installed = true;
}

/* ================================================================== */
/* Conversion helpers                                                  */
/* ================================================================== */

#include "jce_physics_bullet_internal.hpp"

/* Resolve a packed handle to a live pool slot, or UINT32_MAX if the
   handle is stale / out of range / dead.  Centralises the slot+gen
   validation every body accessor needs. */
static inline uint32_t resolve_body(JceBulletWorld *bw, uint32_t handle)
{
    if (!bw) return UINT32_MAX;
    uint32_t slot = handle_slot(handle);
    if (slot >= bw->capacity || !bw->alive[slot]) return UINT32_MAX;
    uint32_t cur = bw->generations ? bw->generations[slot] : 0u;
    if (cur != handle_gen(handle)) return UINT32_MAX;
    return slot;
}

/* ================================================================== */
/* Post-tick contact dispatch                                          */
/* ================================================================== */

static void post_tick_callback(btDynamicsWorld *dyn_world, btScalar /*ts*/)
{
    auto *bw = static_cast<JceBulletWorld *>(dyn_world->getWorldUserInfo());
    if (!bw) return;

    jce_bullet_contact_fn begin_fn = bw->contact_begin_fn;
    void *begin_ud                = bw->contact_begin_ud;
    if (!begin_fn) return;

    btDispatcher *dp = dyn_world->getDispatcher();
    int num_manifolds = dp->getNumManifolds();

    for (int i = 0; i < num_manifolds; ++i) {
        btPersistentManifold *manifold = dp->getManifoldByIndexInternal(i);
        int num_contacts = manifold->getNumContacts();
        if (num_contacts == 0) continue;

        /* Identify body indices by scanning the pool.  The user-pointer
           stores the pool index set during body creation. */
        const btCollisionObject *obj_a = manifold->getBody0();
        const btCollisionObject *obj_b = manifold->getBody1();

        /* Skip vehicle chassis: their user-pointer is a vehicle index, not a
           packed body handle, so decoding it would alias / stale-resolve a real
           body slot (crash after a spawn recycles that slot). */
        if (obj_a->getUserIndex() == JCE_BULLET_VEHICLE_CHASSIS_USERINDEX ||
            obj_b->getUserIndex() == JCE_BULLET_VEHICLE_CHASSIS_USERINDEX)
            continue;

        uint32_t idx_a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            obj_a->getUserPointer()));
        uint32_t idx_b = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            obj_b->getUserPointer()));

        /* The impulse a break threshold cares about is the HIT's, not one
         * point's: a box landing flat produces four points that each carry a
         * quarter of it. */
        float impulse_sum = 0.0f;
        for (int c = 0; c < num_contacts; ++c) {
            const btManifoldPoint &pt = manifold->getContactPoint(c);
            if (pt.getDistance() > 0.0f) continue;
            impulse_sum += (float)pt.m_appliedImpulse;
        }

        /* Same test the pair enumerator uses (obj_is_trigger is defined
           further down this file).  A trigger's overlap is not a collision
           and must not be read as one by whoever is listening. */
        bool pair_is_trigger =
            (obj_a->getCollisionFlags() &
             btCollisionObject::CF_NO_CONTACT_RESPONSE) ||
            (obj_b->getCollisionFlags() &
             btCollisionObject::CF_NO_CONTACT_RESPONSE);

        for (int c = 0; c < num_contacts; ++c) {
            const btManifoldPoint &pt = manifold->getContactPoint(c);
            if (pt.getDistance() > 0.0f) continue; /* separating */

            const btVector3 &n   = pt.m_normalWorldOnB;
            const btVector3 &pos = pt.getPositionWorldOnB();
            float normal[3] = { n.x(), n.y(), n.z() };
            float point[3]  = { pos.x(), pos.y(), pos.z() };

            begin_fn(idx_a, idx_b, normal, point,
                     -pt.getDistance(), impulse_sum, pair_is_trigger,
                     begin_ud);
        }
    }
}

/* ================================================================== */
/* Create / Destroy                                                    */
/* ================================================================== */

#if JCE_PHYSICS_MT_ACTIVE
/* Install Bullet's built-in (Win32/pthreads) task scheduler exactly once
   per process.  btSetTaskScheduler() is global and must be called before
   any "Mt" class is used.  No enkiTS dependency: btCreateDefaultTaskScheduler
   ships inside Bullet itself. */
static void install_bullet_task_scheduler_once()
{
    static bool installed = false;
    if (installed) return;
    if (btGetTaskScheduler() == nullptr) {
        btITaskScheduler *ts = btCreateDefaultTaskScheduler();
        if (ts) btSetTaskScheduler(ts);
    }
    installed = true;
}
#endif /* JCE_PHYSICS_MT_ACTIVE */

JceBulletWorld *jce_bullet_create(jce_vec3 gravity, uint32_t max_bodies,
                                  int solver_iterations, int split_impulse,
                                  float deactivation_time,
                                  float linear_sleep_threshold,
                                  float angular_sleep_threshold,
                                  bool multithreaded)
{
    jce_bullet_install_allocator_();

    /* Install the contact hook HERE, not where a heightfield happens to be
     * created.  It used to be armed only inside
     * jce_bullet_body_create_heightfield under `smooth_internal_edges`, which
     * meant a world with no smooth heightfield had no contact callback at
     * all -- and a per-contact material combine written into it would have
     * reached nothing in most scenes while working perfectly in the terrain
     * scene anyone would have tested it in.  Chaining, not stomping: Bullet
     * has exactly one global hook and other code may already own it. */
    jce_bullet_install_contact_hook();

    auto *bw = static_cast<JceBulletWorld *>(
        JCE_CALLOC(1, sizeof(JceBulletWorld)));
    if (!bw) return nullptr;

    bw->config     = new btDefaultCollisionConfiguration();
    bw->broadphase = new btDbvtBroadphase();

#if JCE_PHYSICS_MT_ACTIVE
    if (multithreaded) {
        /* --- Parallel pipeline (opt-in, compiled in + thread-safe Bullet).
           The dispatcher, the solver-pool, and the world are all "Mt"
           subclasses; they slot into the same base-typed fields and the
           rest of the back-end is unchanged. */
        install_bullet_task_scheduler_once();
        int num_solvers = btGetTaskScheduler()
                              ? btGetTaskScheduler()->getNumThreads()
                              : 1;
        if (num_solvers < 1) num_solvers = 1;

        auto *dispatcher_mt = new btCollisionDispatcherMt(bw->config);
        auto *solver_pool   = new btConstraintSolverPoolMt(num_solvers);
        /* Optional single MT solver for very large islands; pass NULL to
           keep memory/complexity down (the pool handles per-island work). */
        bw->dispatcher = dispatcher_mt;
        bw->solver     = solver_pool;
        bw->world      = new btDiscreteDynamicsWorldMt(
            dispatcher_mt, bw->broadphase, solver_pool,
            /*constraintSolverMt=*/nullptr, bw->config);
        bw->multithreaded = true;
        LOG_INFO("physics", "Bullet multithreaded solver active (%d worker solvers)",
                 num_solvers);
    } else
#else
    if (multithreaded) {
        /* MT requested but not compiled / Bullet not thread-safe: fall back
           to the single-threaded world (no behavioral change, just a note). */
        LOG_WARN("physics",
                 "multithreaded physics requested but JCE_PHYSICS_MT/BT_THREADSAFE "
                 "not compiled in; using single-threaded solver");
    }
#endif /* JCE_PHYSICS_MT_ACTIVE */
    {
        /* --- Single-threaded pipeline (default, unchanged) --- */
        bw->dispatcher = new btCollisionDispatcher(bw->config);
        bw->solver     = new btSequentialImpulseConstraintSolver();
        bw->world      = new btDiscreteDynamicsWorld(
            bw->dispatcher, bw->broadphase, bw->solver, bw->config);
        bw->multithreaded = false;
    }

    bw->world->setGravity(to_bt(gravity));

    /* --- Solver / sleeping tunables (sentinels keep Bullet defaults) --- */
    btContactSolverInfo &si = bw->world->getSolverInfo();
    if (solver_iterations > 0) si.m_numIterations = solver_iterations;
    if (split_impulse >= 0)    si.m_splitImpulse  = (split_impulse != 0);

    /* gDeactivationTime is a process-global btScalar in Bullet — there is
       no per-world setting.  Documented as global in the public header. */
    if (deactivation_time > 0.0f)
        gDeactivationTime = static_cast<btScalar>(deactivation_time);

    /* Sleep thresholds are per-body in Bullet; stash them so each body
       created in this world picks them up at create time. */
    bw->linear_sleep_threshold  = linear_sleep_threshold;
    bw->angular_sleep_threshold = angular_sleep_threshold;

    /* Store back-pointer so the tick callback can reach JceBulletWorld. */
    bw->world->setWorldUserInfo(bw);
    bw->world->setInternalTickCallback(post_tick_callback,
                                       bw, /*isPreTick=*/false);

    /* Register ghost pair callback for character controllers. */
    bw->broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(
        new btGhostPairCallback());

    /* Allocate body pool. */
    bw->capacity = max_bodies;
    bw->count    = 0;
    bw->alloc_cursor = 0;
    bw->bodies   = static_cast<btRigidBody **>(
        JCE_CALLOC(max_bodies, sizeof(btRigidBody *)));
    bw->shapes   = static_cast<btCollisionShape **>(
        JCE_CALLOC(max_bodies, sizeof(btCollisionShape *)));
    bw->alive    = static_cast<bool *>(
        JCE_CALLOC(max_bodies, sizeof(bool)));
    bw->owned_shapes = static_cast<btAlignedObjectArray<btCollisionShape *> **>(
        JCE_CALLOC(max_bodies, sizeof(void *)));
    bw->owned_meshes = static_cast<btAlignedObjectArray<btStridingMeshInterface *> **>(
        JCE_CALLOC(max_bodies, sizeof(void *)));
    /* Generation counters start at 0 (JCE_CALLOC zero-fills). */
    bw->generations = static_cast<uint32_t *>(
        JCE_CALLOC(max_bodies, sizeof(uint32_t)));

    if (!bw->bodies || !bw->shapes || !bw->alive ||
        !bw->owned_shapes || !bw->owned_meshes || !bw->generations) {
        jce_bullet_destroy(bw);
        return nullptr;
    }

    /* Allocate constraint pool. */
    bw->con_capacity = max_bodies / 2;
    if (bw->con_capacity < 64) bw->con_capacity = 64;
    bw->con_count = 0;
    bw->con_alloc_cursor = 0;
    bw->constraints = static_cast<btTypedConstraint **>(
        JCE_CALLOC(bw->con_capacity, sizeof(btTypedConstraint *)));
    bw->con_alive = static_cast<bool *>(
        JCE_CALLOC(bw->con_capacity, sizeof(bool)));
    /* Per-constraint joint feedback; see the struct field for why it is
     * separate from enableFeedback and why it is allocated here. */
    bw->con_feedback = static_cast<btJointFeedback *>(
        JCE_CALLOC(bw->con_capacity, sizeof(btJointFeedback)));

    /* ALL THREE OR NO WORLD.  These are one pool in three arrays -- same
     * capacity, same allocator, same block -- and every user of them indexes
     * all three with the same idx.  A world that has `constraints` but not
     * `con_feedback` is not a degraded world, it is a world whose joints
     * report a flat 0.0000 N*m of applied torque while looking entirely
     * healthy: enableFeedback still accumulates m_appliedImpulse (a scalar on
     * the constraint itself), so break-by-force keeps working and
     * break-by-torque goes silently inert.  That is exactly the defect this
     * pool was added to fix, and "no storage" and "genuinely zero torque" are
     * BIT-IDENTICAL readings with no test anywhere able to tell them apart.
     * Failing creation here is what lets jce_bullet_con_register treat the
     * storage as guaranteed instead of guarding one of the three. */
    if (!bw->constraints || !bw->con_alive || !bw->con_feedback) {
        jce_bullet_destroy(bw);
        return nullptr;
    }

    /* Allocate character controller pool. */
    bw->char_capacity = 32;
    bw->char_count = 0;
    bw->char_alloc_cursor = 0;
    bw->characters = static_cast<btKinematicCharacterController **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btKinematicCharacterController *)));
    bw->ghosts = static_cast<btPairCachingGhostObject **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btPairCachingGhostObject *)));
    bw->char_bodies = static_cast<btRigidBody **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btRigidBody *)));
    bw->char_jump = static_cast<float *>(
        JCE_CALLOC(bw->char_capacity, sizeof(float)));
    bw->char_shapes = static_cast<btConvexShape **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btConvexShape *)));
    bw->char_alive = static_cast<bool *>(
        JCE_CALLOC(bw->char_capacity, sizeof(bool)));
    bw->char_feel = static_cast<JceBulletCharFeel *>(
        JCE_CALLOC(bw->char_capacity, sizeof(JceBulletCharFeel)));

    /* Allocate vehicle controller pool. */
    bw->vehicle_capacity = 16;
    bw->vehicle_count = 0;
    bw->vehicle_alloc_cursor = 0;
    bw->vehicles = static_cast<btRaycastVehicle **>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(btRaycastVehicle *)));
    bw->vehicle_raycasters = static_cast<btDefaultVehicleRaycaster **>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(btDefaultVehicleRaycaster *)));
    bw->vehicle_chassis = static_cast<btRigidBody **>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(btRigidBody *)));
    bw->vehicle_chassis_shapes = static_cast<btCollisionShape **>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(btCollisionShape *)));
    bw->vehicle_alive = static_cast<bool *>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(bool)));
    bw->vehicle_max_engine = static_cast<float *>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(float)));
    bw->vehicle_max_brake = static_cast<float *>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(float)));
    bw->vehicle_max_steer = static_cast<float *>(
        JCE_CALLOC(bw->vehicle_capacity, sizeof(float)));

    return bw;
}

/* Delete the compound child shapes and triangle-mesh interfaces owned by
   body `idx` (if any) and release the holder arrays.  Safe to call on a
   primitive body (no-op).  Does NOT touch bw->shapes[idx] — the caller
   deletes the top-level shape separately. */
static void free_body_aux(JceBulletWorld *bw, uint32_t idx)
{
    if (bw->owned_shapes && bw->owned_shapes[idx]) {
        btAlignedObjectArray<btCollisionShape *> *arr = bw->owned_shapes[idx];
        for (int c = 0; c < arr->size(); ++c) delete (*arr)[c];
        delete arr;
        bw->owned_shapes[idx] = nullptr;
    }
    if (bw->owned_meshes && bw->owned_meshes[idx]) {
        btAlignedObjectArray<btStridingMeshInterface *> *arr = bw->owned_meshes[idx];
        for (int c = 0; c < arr->size(); ++c) delete (*arr)[c];
        delete arr;
        bw->owned_meshes[idx] = nullptr;
    }
}

void jce_bullet_destroy(JceBulletWorld *bw)
{
    if (!bw) return;

    /* Detach any debug drawer attached to this world (P3-C.5). */
    jce_bullet_debug_world_destroyed_(bw);

    /* Remove and delete all live vehicles (must come before bodies). */
    if (bw->vehicles && bw->vehicle_alive) {
        for (uint32_t i = 0; i < bw->vehicle_capacity; ++i) {
            if (!bw->vehicle_alive[i]) continue;
            if (bw->vehicles[i]) {
                bw->world->removeVehicle(bw->vehicles[i]);
                delete bw->vehicles[i];
            }
            delete bw->vehicle_raycasters[i];
            if (bw->vehicle_chassis[i]) {
                bw->world->removeRigidBody(bw->vehicle_chassis[i]);
                delete bw->vehicle_chassis[i]->getMotionState();
                delete bw->vehicle_chassis[i];
            }
            delete bw->vehicle_chassis_shapes[i];
            bw->vehicle_alive[i] = false;
        }
    }

    /* Remove and delete all live characters (dynamic capsule rigid bodies). */
    if (bw->char_bodies && bw->char_shapes && bw->char_alive) {
        for (uint32_t i = 0; i < bw->char_capacity; ++i) {
            if (!bw->char_alive[i]) continue;
            if (bw->char_bodies[i]) {
                bw->world->removeRigidBody(bw->char_bodies[i]);
                delete bw->char_bodies[i];
            }
            delete bw->char_shapes[i];
            bw->char_alive[i] = false;
        }
    }

    /* Remove and delete all live constraints. */
    if (bw->constraints && bw->con_alive) {
        for (uint32_t i = 0; i < bw->con_capacity; ++i) {
            if (!bw->con_alive[i]) continue;
            if (bw->constraints[i]) {
                bw->world->removeConstraint(bw->constraints[i]);
                delete bw->constraints[i];
            }
            bw->con_alive[i] = false;
        }
    }

    /* Remove and delete all live bodies. */
    if (bw->bodies && bw->shapes && bw->alive) {
        for (uint32_t i = 0; i < bw->capacity; ++i) {
            if (!bw->alive[i]) continue;
            btRigidBody *body = bw->bodies[i];
            if (body) {
                bw->world->removeRigidBody(body);
                delete body->getMotionState();
                delete body;
            }
            delete bw->shapes[i];
            free_body_aux(bw, i);
            bw->alive[i] = false;
        }
    }

    /* Tear down Bullet pipeline in reverse order. */
    delete bw->world;
    delete bw->solver;
    delete bw->broadphase;
    delete bw->dispatcher;
    delete bw->config;

    JCE_FREE(bw->char_feel);
    JCE_FREE(bw->char_alive);
    JCE_FREE(bw->char_shapes);
    JCE_FREE(bw->char_jump);
    JCE_FREE(bw->char_bodies);
    JCE_FREE(bw->ghosts);
    JCE_FREE(bw->characters);
    JCE_FREE(bw->vehicle_max_steer);
    JCE_FREE(bw->vehicle_max_brake);
    JCE_FREE(bw->vehicle_max_engine);
    JCE_FREE(bw->vehicle_alive);
    JCE_FREE(bw->vehicle_chassis_shapes);
    JCE_FREE(bw->vehicle_chassis);
    JCE_FREE(bw->vehicle_raycasters);
    JCE_FREE(bw->vehicles);
    JCE_FREE(bw->con_alive);
    JCE_FREE(bw->con_feedback);
    JCE_FREE(bw->constraints);
    JCE_FREE(bw->generations);
    JCE_FREE(bw->alive);
    JCE_FREE(bw->owned_meshes);
    JCE_FREE(bw->owned_shapes);
    JCE_FREE(bw->shapes);
    JCE_FREE(bw->bodies);
    JCE_FREE(bw);
}

/* ================================================================== */
/* Step                                                                */
/* ================================================================== */

void jce_bullet_step(JceBulletWorld *bw, float dt, float fixed_dt,
                     int max_sub_steps)
{
    if (!bw || !bw->world) return;
    bw->world->stepSimulation(static_cast<btScalar>(dt),
                              max_sub_steps,
                              static_cast<btScalar>(fixed_dt));
}

/* ================================================================== */
/* Body create / destroy                                               */
/* ================================================================== */

/* Apply the world's configured sleep thresholds to a freshly created
   body.  A sentinel value <= 0 leaves Bullet's per-body default for that
   axis (linear 0.8, angular 1.0), so an unset world reproduces prior
   behavior exactly. */
/* One capsule, three Bullet classes.  Kept in one place so the two shape
 * factories below cannot drift -- they already differed in nothing but
 * spelling, which is how a fix lands in one and not the other. */
static btCollisionShape *make_capsule(btScalar radius, btScalar height,
                                      uint8_t axis)
{
    switch (axis) {
    case JCE_CAPSULE_AXIS_X: return new btCapsuleShapeX(radius, height);
    case JCE_CAPSULE_AXIS_Z: return new btCapsuleShapeZ(radius, height);
    default:                 return new btCapsuleShape(radius, height);
    }
}

static void apply_sleep_thresholds(JceBulletWorld *bw, btRigidBody *body)
{
    if (!bw || !body) return;
    if (bw->linear_sleep_threshold <= 0.0f &&
        bw->angular_sleep_threshold <= 0.0f)
        return;
    btScalar lin = bw->linear_sleep_threshold > 0.0f
                       ? static_cast<btScalar>(bw->linear_sleep_threshold)
                       : body->getLinearSleepingThreshold();
    btScalar ang = bw->angular_sleep_threshold > 0.0f
                       ? static_cast<btScalar>(bw->angular_sleep_threshold)
                       : body->getAngularSleepingThreshold();
    body->setSleepingThresholds(lin, ang);
}

uint32_t jce_bullet_body_create(JceBulletWorld *bw,
                                uint8_t type, uint8_t shape,
                                jce_vec3 pos, jce_quat rot,
                                jce_vec3 half_ext, float mass,
                                float friction, float restitution,
                                float lin_damp, float ang_damp,
                                uint32_t col_group, uint32_t col_mask,
                                bool is_trigger,
                                /* JceCapsuleAxis */ uint8_t capsule_axis)
{
    if (!bw) return UINT32_MAX;

    /* Find a free slot, scanning from a rotating cursor so a burst of creates
       (mass-spawn) is O(n) total instead of O(n^2) re-scanning from 0. Freed
       slots are still reused once the cursor wraps. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->capacity; ++n) {
        uint32_t i = (bw->alloc_cursor + n) % bw->capacity;
        if (!bw->alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX; /* pool exhausted */
    bw->alloc_cursor = (idx + 1u) % bw->capacity;

    /* ---- Collision shape ---- */
    btCollisionShape *col_shape = nullptr;

    switch (static_cast<JceShapeType>(shape)) {
    case JCE_SHAPE_BOX:
        col_shape = new btBoxShape(to_bt(half_ext));
        break;
    case JCE_SHAPE_SPHERE:
        col_shape = new btSphereShape(static_cast<btScalar>(half_ext.x));
        break;
    case JCE_SHAPE_CAPSULE:
        col_shape = make_capsule(static_cast<btScalar>(half_ext.x),
                                 static_cast<btScalar>(half_ext.y * 2.0f),
                                 capsule_axis);
        break;
    case JCE_SHAPE_PLANE:
        col_shape = new btStaticPlaneShape(btVector3(0, 1, 0), 0);
        break;
    default:
        col_shape = new btBoxShape(to_bt(half_ext));
        break;
    }

    /* ---- Mass and inertia ---- */
    btScalar bt_mass = 0.0f;
    if (static_cast<JceBodyType>(type) == JCE_BODY_DYNAMIC) {
        bt_mass = static_cast<btScalar>(mass);
    }

    btVector3 local_inertia(0, 0, 0);
    if (bt_mass > 0.0f) {
        col_shape->calculateLocalInertia(bt_mass, local_inertia);
    }

    /* ---- Motion state ---- */
    btTransform start_xf;
    start_xf.setOrigin(to_bt(pos));
    start_xf.setRotation(to_bt_q(rot));

    auto *motion = new btDefaultMotionState(start_xf);

    /* ---- Construction info ---- */
    btRigidBody::btRigidBodyConstructionInfo ci(
        bt_mass, motion, col_shape, local_inertia);
    ci.m_friction    = static_cast<btScalar>(friction);
    ci.m_restitution = static_cast<btScalar>(restitution);
    ci.m_linearDamping  = static_cast<btScalar>(lin_damp);
    ci.m_angularDamping = static_cast<btScalar>(ang_damp);

    auto *body = new btRigidBody(ci);

    /* Apply world-configured sleep thresholds (sentinel <=0 keeps the
       body's existing Bullet default per axis). */
    apply_sleep_thresholds(bw, body);

    /* Tag kinematic bodies so Bullet treats them correctly. */
    if (static_cast<JceBodyType>(type) == JCE_BODY_KINEMATIC) {
        body->setCollisionFlags(
            body->getCollisionFlags() |
            btCollisionObject::CF_KINEMATIC_OBJECT);
        body->setActivationState(DISABLE_DEACTIVATION);
    }

    /* Trigger bodies: no contact response (overlap only). */
    if (is_trigger) {
        body->setCollisionFlags(
            body->getCollisionFlags() |
            btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }

    /* Store the PACKED handle (slot + current generation) in the
       user-pointer so contact / raycast / overlap resolve back to a
       handle the C layer can use directly. */
    uint32_t handle = handle_encode(idx, bw->generations ? bw->generations[idx] : 0u);
    body->setUserPointer(reinterpret_cast<void *>(
        static_cast<uintptr_t>(handle)));

    /* Add to world with collision group/mask. */
    bw->world->addRigidBody(body,
                             static_cast<int>(col_group),
                             static_cast<int>(col_mask));
    bw->bodies[idx] = body;
    bw->shapes[idx] = col_shape;
    bw->alive[idx]  = true;
    bw->count++;

    return handle;
}

void jce_bullet_body_destroy(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;

    btRigidBody *body = bw->bodies[slot];
    if (body) {
        /* Destroy any constraints that reference this body BEFORE deleting it —
         * a live btTypedConstraint holds btRigidBody& to both ends, so leaving
         * one behind makes the next solver step dereference freed memory
         * (audit F41).  Mirrors jce_bullet_constraint_destroy's cleanup. */
        if (bw->constraints && bw->con_alive) {
            for (uint32_t i = 0; i < bw->con_capacity; ++i) {
                if (!bw->con_alive[i] || !bw->constraints[i]) continue;
                btTypedConstraint *con = bw->constraints[i];
                if (&con->getRigidBodyA() == body ||
                    &con->getRigidBodyB() == body) {
                    bw->world->removeConstraint(con);
                    delete con;
                    bw->constraints[i] = nullptr;
                    bw->con_alive[i]   = false;
                    bw->con_count--;
                }
            }
        }
        bw->world->removeRigidBody(body);
        delete body->getMotionState();
        delete body;
    }
    delete bw->shapes[slot];
    free_body_aux(bw, slot);

    bw->bodies[slot] = nullptr;
    bw->shapes[slot] = nullptr;
    bw->alive[slot]  = false;
    /* Bump the slot generation so any handle still holding the old gen is
       rejected by resolve_body once this slot is reused. */
    if (bw->generations) bw->generations[slot]++;
    bw->count--;
}

uint32_t jce_bullet_body_generation(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = handle_slot(idx);
    if (!bw || slot >= bw->capacity || !bw->generations) return 0u;
    return bw->generations[slot];
}

bool jce_bullet_body_alive_gen(JceBulletWorld *bw, uint32_t idx, uint32_t gen)
{
    uint32_t slot = handle_slot(idx);
    if (!bw || slot >= bw->capacity || !bw->alive[slot]) return false;
    /* generations may be NULL only on a half-constructed world; treat the
       implied generation as 0 in that case so a gen-0 handle still passes. */
    uint32_t cur = bw->generations ? bw->generations[slot] : 0u;
    return cur == gen;
}

/* ================================================================== */
/* Compound / mesh body create                                         */
/* ================================================================== */

/* Build a single child collision shape from a descriptor.  Convex-hull
   and triangle-mesh children record their owned sub-objects so the body
   can release them later.  Returns nullptr on bad input. */
static btCollisionShape *build_child_shape(const JceBulletColliderChild *c,
                                           btAlignedObjectArray<btStridingMeshInterface *> *meshes)
{
    switch (static_cast<JceShapeType>(c->shape)) {
    case JCE_SHAPE_BOX:
        return new btBoxShape(to_bt(c->half_extents));
    case JCE_SHAPE_SPHERE:
        return new btSphereShape(static_cast<btScalar>(c->half_extents.x));
    case JCE_SHAPE_CAPSULE:
        return make_capsule(static_cast<btScalar>(c->half_extents.x),
                            static_cast<btScalar>(c->half_extents.y * 2.0f),
                            c->capsule_axis);
    case JCE_SHAPE_CONVEX_HULL: {
        if (!c->vertices || c->vertex_count == 0) return nullptr;
        auto *hull = new btConvexHullShape();
        for (uint32_t v = 0; v < c->vertex_count; ++v) {
            const float *p = &c->vertices[v * 3];
            hull->addPoint(btVector3(p[0], p[1], p[2]), false);
        }
        hull->recalcLocalAabb();
        /* Cap the hull vertex count so an un-simplified mesh dumped as a
           point cloud cannot bloat the dynamic solver.  btShapeHull
           recomputes a reduced hull (<= JCE_HULL_MAX_VERTS) from the
           silhouette. */
        if (c->vertex_count > JCE_HULL_MAX_VERTS) {
            btShapeHull sh(hull);
            sh.buildHull(hull->getMargin());
            if (sh.numVertices() > 0) {
                auto *reduced = new btConvexHullShape(
                    reinterpret_cast<const btScalar *>(sh.getVertexPointer()),
                    sh.numVertices(), sizeof(btVector3));
                reduced->recalcLocalAabb();
                delete hull;
                hull = reduced;
            }
        }
        /* Light simplification keeps the dynamic solver fast on the
           512MB / single-core baseline without changing the silhouette. */
        hull->optimizeConvexHull();
        return hull;
    }
    case JCE_SHAPE_TRIANGLE_MESH: {
        if (!c->vertices || c->vertex_count == 0 ||
            !c->indices  || c->index_count < 3) return nullptr;
        auto *mesh = new btTriangleMesh();
        uint32_t added = 0;
        for (uint32_t t = 0; t + 2 < c->index_count; t += 3) {
            uint32_t i0 = c->indices[t + 0];
            uint32_t i1 = c->indices[t + 1];
            uint32_t i2 = c->indices[t + 2];
            /* Reject triangles referencing vertices outside the buffer —
               a corrupt blob would otherwise OOB-read. */
            if (i0 >= c->vertex_count || i1 >= c->vertex_count ||
                i2 >= c->vertex_count)
                continue;
            const float *a = &c->vertices[i0 * 3];
            const float *b = &c->vertices[i1 * 3];
            const float *d = &c->vertices[i2 * 3];
            mesh->addTriangle(btVector3(a[0], a[1], a[2]),
                              btVector3(b[0], b[1], b[2]),
                              btVector3(d[0], d[1], d[2]), true);
            ++added;
        }
        if (added == 0) { delete mesh; return nullptr; }
        if (meshes) meshes->push_back(mesh);
        return new btBvhTriangleMeshShape(mesh, /*useQuantizedAabb=*/true);
    }
    default:
        return nullptr;
    }
}

uint32_t jce_bullet_body_create_compound(JceBulletWorld *bw,
                                         uint8_t type,
                                         jce_vec3 pos, jce_quat rot,
                                         float mass, float friction,
                                         float restitution,
                                         float lin_damp, float ang_damp,
                                         uint32_t col_group, uint32_t col_mask,
                                         bool is_trigger,
                                         const JceBulletColliderChild *children,
                                         uint32_t child_count)
{
    if (!bw || !children || child_count == 0) return UINT32_MAX;

    /* Find a free slot from the rotating cursor (same scheme as the primitive
       create path) so a burst of compound creates at scene load is O(n) total
       instead of O(n^2) re-scanning from index 0. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->capacity; ++n) {
        uint32_t i = (bw->alloc_cursor + n) % bw->capacity;
        if (!bw->alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->alloc_cursor = (idx + 1u) % bw->capacity;

    auto *meshes = new btAlignedObjectArray<btStridingMeshInterface *>();
    auto *owned  = new btAlignedObjectArray<btCollisionShape *>();

    /* Build children. */
    btCollisionShape *top = nullptr;
    bool single_identity =
        (child_count == 1 &&
         children[0].position.x == 0.0f && children[0].position.y == 0.0f &&
         children[0].position.z == 0.0f &&
         children[0].rotation.x == 0.0f && children[0].rotation.y == 0.0f &&
         children[0].rotation.z == 0.0f && children[0].rotation.w == 1.0f);

    if (single_identity) {
        top = build_child_shape(&children[0], meshes);
        if (!top) { delete owned; for (int m=0;m<meshes->size();++m) delete (*meshes)[m]; delete meshes; return UINT32_MAX; }
    } else {
        auto *compound = new btCompoundShape();
        for (uint32_t i = 0; i < child_count; ++i) {
            btCollisionShape *cs = build_child_shape(&children[i], meshes);
            if (!cs) continue; /* skip malformed child, keep the rest */
            owned->push_back(cs);
            btTransform xf;
            xf.setOrigin(to_bt(children[i].position));
            xf.setRotation(to_bt_q(children[i].rotation));
            compound->addChildShape(xf, cs);
        }
        if (compound->getNumChildShapes() == 0) {
            delete compound; delete owned;
            for (int m=0;m<meshes->size();++m) delete (*meshes)[m];
            delete meshes;
            return UINT32_MAX;
        }
        top = compound;
    }

    btScalar bt_mass = 0.0f;
    if (static_cast<JceBodyType>(type) == JCE_BODY_DYNAMIC)
        bt_mass = static_cast<btScalar>(mass);

    btVector3 local_inertia(0, 0, 0);
    if (bt_mass > 0.0f) top->calculateLocalInertia(bt_mass, local_inertia);

    btTransform start_xf;
    start_xf.setOrigin(to_bt(pos));
    start_xf.setRotation(to_bt_q(rot));
    auto *motion = new btDefaultMotionState(start_xf);

    btRigidBody::btRigidBodyConstructionInfo ci(bt_mass, motion, top, local_inertia);
    ci.m_friction       = static_cast<btScalar>(friction);
    ci.m_restitution    = static_cast<btScalar>(restitution);
    ci.m_linearDamping  = static_cast<btScalar>(lin_damp);
    ci.m_angularDamping = static_cast<btScalar>(ang_damp);

    auto *body = new btRigidBody(ci);

    /* World-configured sleep thresholds (sentinel <=0 keeps default). */
    apply_sleep_thresholds(bw, body);

    if (static_cast<JceBodyType>(type) == JCE_BODY_KINEMATIC) {
        body->setCollisionFlags(body->getCollisionFlags() |
                                btCollisionObject::CF_KINEMATIC_OBJECT);
        body->setActivationState(DISABLE_DEACTIVATION);
    }
    if (is_trigger) {
        body->setCollisionFlags(body->getCollisionFlags() |
                                btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }
    uint32_t handle = handle_encode(idx, bw->generations ? bw->generations[idx] : 0u);
    body->setUserPointer(reinterpret_cast<void *>(static_cast<uintptr_t>(handle)));

    bw->world->addRigidBody(body, static_cast<int>(col_group),
                            static_cast<int>(col_mask));

    bw->bodies[idx] = body;
    bw->shapes[idx] = top;
    bw->alive[idx]  = true;
    /* Keep child + mesh holders only when non-empty (compound path). */
    if (owned->size() > 0) bw->owned_shapes[idx] = owned; else delete owned;
    if (meshes->size() > 0) bw->owned_meshes[idx] = meshes; else delete meshes;
    bw->count++;

    return handle;
}

/* ================================================================== */
/* Transform                                                           */
/* ================================================================== */

void jce_bullet_body_get_transform(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 *pos, jce_quat *rot)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;

    btTransform xf;
    btRigidBody *body = bw->bodies[slot];
    if (body->getMotionState()) {
        body->getMotionState()->getWorldTransform(xf);
    } else {
        xf = body->getWorldTransform();
    }

    if (pos) *pos = from_bt_v3(xf.getOrigin());
    if (rot) *rot = from_bt_q(xf.getRotation());
}

void jce_bullet_body_set_transform(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 pos, jce_quat rot)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;

    btTransform xf;
    xf.setOrigin(to_bt(pos));
    xf.setRotation(to_bt_q(rot));

    btRigidBody *body = bw->bodies[slot];
    body->setWorldTransform(xf);
    if (body->getMotionState()) {
        body->getMotionState()->setWorldTransform(xf);
    }
    /* Refresh the broadphase AABB so moved static/kinematic bodies collide
     * at their new pose (dynamic bodies do this during the step anyway). */
    if (bw->world) bw->world->updateSingleAabb(body);
    body->activate();
}

void jce_bullet_body_set_scale(JceBulletWorld *bw, uint32_t idx, jce_vec3 scale)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    btCollisionShape *shape = body->getCollisionShape();
    if (!shape) return;

    shape->setLocalScaling(to_bt(scale));

    /* Recompute inertia for dynamic bodies (invMass != 0 ⇒ mass > 0). */
    if (body->getInvMass() != btScalar(0)) {
        btScalar mass = btScalar(1.0) / body->getInvMass();
        btVector3 inertia(0, 0, 0);
        shape->calculateLocalInertia(mass, inertia);
        body->setMassProps(mass, inertia);
        body->updateInertiaTensor();
    }
    if (bw->world) bw->world->updateSingleAabb(body);
    body->activate();
}

/* ================================================================== */
/* Linear velocity                                                     */
/* ================================================================== */

void jce_bullet_body_get_velocity(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 *vel)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX || !vel) return;
    *vel = from_bt_v3(bw->bodies[slot]->getLinearVelocity());
}

void jce_bullet_body_set_velocity(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 vel)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    bw->bodies[slot]->setLinearVelocity(to_bt(vel));
    bw->bodies[slot]->activate();
}

/* Sim-LOD physics gating: force a body to sleep (far tier) or wake (near tier).
 * Inactive => DISABLE_SIMULATION (the solver/island manager skips it entirely)
 * with velocities zeroed so it freezes in place; active => ACTIVE_TAG + activate
 * so it resumes normal integration.  forceActivationState (not setActivationState)
 * so Bullet does not auto-wake a deliberately-slept far body on contact. */
void jce_bullet_body_set_active(JceBulletWorld *bw, uint32_t idx, bool active)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *b = bw->bodies[slot];
    if (active) {
        if (b->getActivationState() == DISABLE_SIMULATION)
            b->forceActivationState(ACTIVE_TAG);
        b->activate(true);
    } else {
        if (b->getActivationState() != DISABLE_SIMULATION) {
            b->setLinearVelocity(btVector3(0, 0, 0));
            b->setAngularVelocity(btVector3(0, 0, 0));
            b->forceActivationState(DISABLE_SIMULATION);
        }
    }
}

/* ================================================================== */
/* Angular velocity                                                    */
/* ================================================================== */

void jce_bullet_body_get_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 *vel)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX || !vel) return;
    *vel = from_bt_v3(bw->bodies[slot]->getAngularVelocity());
}

void jce_bullet_body_set_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 vel)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    bw->bodies[slot]->setAngularVelocity(to_bt(vel));
    bw->bodies[slot]->activate();
}

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

bool jce_bullet_body_is_dynamic(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return false;
    /* invMass != 0 ⇒ a movable (dynamic) rigid body; static/kinematic are 0. */
    return bw->bodies[slot]->getInvMass() != btScalar(0);
}

void jce_bullet_body_apply_force(JceBulletWorld *bw, uint32_t idx,
                                 jce_vec3 force)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    bw->bodies[slot]->applyCentralForce(to_bt(force));
    bw->bodies[slot]->activate();
}

void jce_bullet_body_apply_impulse(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 impulse)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    bw->bodies[slot]->applyCentralImpulse(to_bt(impulse));
    bw->bodies[slot]->activate();
}

void jce_bullet_body_apply_torque(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 torque)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    bw->bodies[slot]->applyTorque(to_bt(torque));
    bw->bodies[slot]->activate();
}

void jce_bullet_body_apply_force_at_point(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 force, jce_vec3 world_point)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    btVector3 rel = to_bt(world_point) - body->getCenterOfMassPosition();
    body->applyForce(to_bt(force), rel);
    body->activate();
}

void jce_bullet_body_apply_impulse_at_point(JceBulletWorld *bw, uint32_t idx,
                                            jce_vec3 impulse, jce_vec3 world_point)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    btVector3 rel = to_bt(world_point) - body->getCenterOfMassPosition();
    body->applyImpulse(to_bt(impulse), rel);
    body->activate();
}

void jce_bullet_body_set_gravity_factor(JceBulletWorld *bw, uint32_t idx,
                                        float factor)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body || !bw->world) return;
    /* Per-body gravity = world gravity * factor.  addRigidBody() resets a
     * body's gravity to the world value, so this must run post-create. */
    body->setGravity(bw->world->getGravity() * static_cast<btScalar>(factor));
    body->activate();
}

/* Per-axis angular factor.  (0,0,0) locks all rotation so a dynamic body never
 * tips/rolls but still collides linearly (Unity FreezeRotation); (1,1,1) frees
 * it.  Must run post-create (addRigidBody does not reset it, but the component
 * default is applied here for symmetry with gravity). */
void jce_bullet_body_set_angular_factor(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 factor)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    body->setAngularFactor(to_bt(factor));
    body->activate();
}

void jce_bullet_body_set_mass(JceBulletWorld *bw, uint32_t idx, float mass)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    btVector3 inertia(0, 0, 0);
    if (mass > 0.0f && body->getCollisionShape())
        body->getCollisionShape()->calculateLocalInertia(
            static_cast<btScalar>(mass), inertia);
    body->setMassProps(static_cast<btScalar>(mass), inertia);
    body->updateInertiaTensor();
    body->activate();
}

/* ================================================================== */
/* Raycast                                                             */
/* ================================================================== */

JceBulletRayResult jce_bullet_raycast(JceBulletWorld *bw, jce_vec3 origin,
                                      jce_vec3 dir, float max_dist)
{
    JceBulletRayResult result;
    std::memset(&result, 0, sizeof(result));
    result.body_idx = UINT32_MAX;

    if (!bw || !bw->world) return result;

    /* Guard a zero-length direction / non-positive distance: normalizing a
     * zero vector yields NaN and corrupts the broadphase query. */
    btVector3 d = to_bt(dir);
    if (max_dist <= 0.0f || d.length2() < SIMD_EPSILON) return result;

    btVector3 from = to_bt(origin);
    btVector3 to   = from + d.normalized() * static_cast<btScalar>(max_dist);

    btCollisionWorld::ClosestRayResultCallback cb(from, to);
    bw->world->rayTest(from, to, cb);

    if (!cb.hasHit()) return result;

    result.hit      = true;
    result.point    = from_bt_v3(cb.m_hitPointWorld);
    result.normal   = from_bt_v3(cb.m_hitNormalWorld);
    result.distance = static_cast<float>(
        (cb.m_hitPointWorld - from).length());

    /* Resolve body index via the user-pointer stored during creation. */
    const btCollisionObject *hit_obj = cb.m_collisionObject;
    if (hit_obj) {
        result.body_idx = static_cast<uint32_t>(
            reinterpret_cast<uintptr_t>(hit_obj->getUserPointer()));
    }

    return result;
}

/* ================================================================== */
/* Filtered spatial queries                                            */
/* ================================================================== */

namespace {

static uint32_t body_idx_of(const btCollisionObject *obj)
{
    if (!obj) return UINT32_MAX;
    return static_cast<uint32_t>(
        reinterpret_cast<uintptr_t>(obj->getUserPointer()));
}

static bool obj_is_trigger(const btCollisionObject *obj)
{
    return obj && (obj->getCollisionFlags() &
                   btCollisionObject::CF_NO_CONTACT_RESPONSE);
}

/* layer_mask is applied as the query's filter mask (query group = all), so a
 * body is hit when its group bit (1<<layer) is in layer_mask.  Triggers are
 * rejected in needsCollision unless hit_triggers is set. */
struct FilteredClosestRay : public btCollisionWorld::ClosestRayResultCallback {
    bool skip_trig;
    FilteredClosestRay(const btVector3 &f, const btVector3 &t,
                       uint32_t layer_mask, bool hit_triggers)
        : ClosestRayResultCallback(f, t), skip_trig(!hit_triggers)
    {
        m_collisionFilterGroup = -1;
        m_collisionFilterMask  = static_cast<int>(layer_mask);
    }
    bool needsCollision(btBroadphaseProxy *proxy) const override
    {
        if (!ClosestRayResultCallback::needsCollision(proxy)) return false;
        return !(skip_trig && obj_is_trigger(
                     static_cast<btCollisionObject *>(proxy->m_clientObject)));
    }
};

struct FilteredAllRay : public btCollisionWorld::AllHitsRayResultCallback {
    bool skip_trig;
    FilteredAllRay(const btVector3 &f, const btVector3 &t,
                   uint32_t layer_mask, bool hit_triggers)
        : AllHitsRayResultCallback(f, t), skip_trig(!hit_triggers)
    {
        m_collisionFilterGroup = -1;
        m_collisionFilterMask  = static_cast<int>(layer_mask);
    }
    bool needsCollision(btBroadphaseProxy *proxy) const override
    {
        if (!AllHitsRayResultCallback::needsCollision(proxy)) return false;
        return !(skip_trig && obj_is_trigger(
                     static_cast<btCollisionObject *>(proxy->m_clientObject)));
    }
};

struct FilteredClosestConvex : public btCollisionWorld::ClosestConvexResultCallback {
    bool skip_trig;
    FilteredClosestConvex(const btVector3 &f, const btVector3 &t,
                          uint32_t layer_mask, bool hit_triggers)
        : ClosestConvexResultCallback(f, t), skip_trig(!hit_triggers)
    {
        m_collisionFilterGroup = -1;
        m_collisionFilterMask  = static_cast<int>(layer_mask);
    }
    bool needsCollision(btBroadphaseProxy *proxy) const override
    {
        if (!ClosestConvexResultCallback::needsCollision(proxy)) return false;
        return !(skip_trig && obj_is_trigger(
                     static_cast<btCollisionObject *>(proxy->m_clientObject)));
    }
};

struct OverlapCollector : public btCollisionWorld::ContactResultCallback {
    const btCollisionObject *probe = nullptr;
    uint32_t *out = nullptr;
    uint32_t  max = 0;
    uint32_t  count = 0;
    bool      skip_trig = true;
    btScalar addSingleResult(btManifoldPoint & /*cp*/,
                             const btCollisionObjectWrapper *a, int, int,
                             const btCollisionObjectWrapper *b, int, int) override
    {
        const btCollisionObject *oa = a->getCollisionObject();
        const btCollisionObject *ob = b->getCollisionObject();
        const btCollisionObject *other = (oa == probe) ? ob : oa;
        if (!other || (skip_trig && obj_is_trigger(other))) return 0;
        uint32_t idx = body_idx_of(other);
        if (idx == UINT32_MAX || count >= max) return 0;
        for (uint32_t i = 0; i < count; ++i)
            if (out[i] == idx) return 0;   /* dedup */
        out[count++] = idx;
        return 0;
    }
};

} /* namespace */

JceBulletRayResult jce_bullet_raycast_filtered(JceBulletWorld *bw,
                                               jce_vec3 origin, jce_vec3 dir,
                                               float max_dist,
                                               uint32_t layer_mask,
                                               bool hit_triggers)
{
    JceBulletRayResult result;
    std::memset(&result, 0, sizeof(result));
    result.body_idx = UINT32_MAX;
    if (!bw || !bw->world) return result;

    btVector3 d = to_bt(dir);
    if (max_dist <= 0.0f || d.length2() < SIMD_EPSILON) return result;
    btVector3 from = to_bt(origin);
    btVector3 to   = from + d.normalized() * static_cast<btScalar>(max_dist);

    FilteredClosestRay cb(from, to, layer_mask, hit_triggers);
    bw->world->rayTest(from, to, cb);
    if (!cb.hasHit()) return result;

    result.hit      = true;
    result.point    = from_bt_v3(cb.m_hitPointWorld);
    result.normal   = from_bt_v3(cb.m_hitNormalWorld);
    result.distance = static_cast<float>((cb.m_hitPointWorld - from).length());
    result.body_idx = body_idx_of(cb.m_collisionObject);
    return result;
}

uint32_t jce_bullet_raycast_all(JceBulletWorld *bw,
                                jce_vec3 origin, jce_vec3 dir, float max_dist,
                                uint32_t layer_mask, bool hit_triggers,
                                JceBulletRayResult *out, uint32_t max_hits)
{
    if (!bw || !bw->world || !out || max_hits == 0) return 0;
    btVector3 d = to_bt(dir);
    if (max_dist <= 0.0f || d.length2() < SIMD_EPSILON) return 0;
    btVector3 from = to_bt(origin);
    btVector3 to   = from + d.normalized() * static_cast<btScalar>(max_dist);

    FilteredAllRay cb(from, to, layer_mask, hit_triggers);
    bw->world->rayTest(from, to, cb);
    int n = cb.m_collisionObjects.size();
    if (n == 0) return 0;

    /* Emit the nearest max_hits hits in ascending fraction order without
     * an auxiliary allocation (max_hits is small in practice). */
    uint32_t written = 0;
    float last_frac = -1.0f;
    int   last_i    = -1;
    while (written < max_hits) {
        int   best      = -1;
        float best_frac = 1e30f;
        for (int i = 0; i < n; ++i) {
            float f = cb.m_hitFractions[i];
            if (f < last_frac) continue;
            if (f == last_frac && i <= last_i) continue;
            if (f < best_frac) { best_frac = f; best = i; }
        }
        if (best < 0) break;
        out[written].hit      = true;
        out[written].point    = from_bt_v3(cb.m_hitPointWorld[best]);
        out[written].normal   = from_bt_v3(cb.m_hitNormalWorld[best]);
        out[written].distance = static_cast<float>(
            (cb.m_hitPointWorld[best] - from).length());
        out[written].body_idx = body_idx_of(cb.m_collisionObjects[best]);
        written++;
        last_frac = best_frac;
        last_i    = best;
    }
    return written;
}

uint32_t jce_bullet_overlap_sphere(JceBulletWorld *bw, jce_vec3 center,
                                   float radius, uint32_t layer_mask,
                                   bool hit_triggers,
                                   uint32_t *out_idx, uint32_t max)
{
    if (!bw || !bw->world || !out_idx || max == 0 || radius <= 0.0f) return 0;
    btSphereShape shape(static_cast<btScalar>(radius));
    btCollisionObject probe;
    probe.setCollisionShape(&shape);
    btTransform xf;
    xf.setIdentity();
    xf.setOrigin(to_bt(center));
    probe.setWorldTransform(xf);

    OverlapCollector cb;
    cb.probe = &probe;
    cb.out = out_idx;
    cb.max = max;
    cb.skip_trig = !hit_triggers;
    cb.m_collisionFilterGroup = -1;
    cb.m_collisionFilterMask  = static_cast<int>(layer_mask);
    bw->world->contactTest(&probe, cb);
    return cb.count;
}

uint32_t jce_bullet_overlap_box(JceBulletWorld *bw, jce_vec3 center,
                                jce_vec3 half_ext, jce_quat rot,
                                uint32_t layer_mask, bool hit_triggers,
                                uint32_t *out_idx, uint32_t max)
{
    if (!bw || !bw->world || !out_idx || max == 0) return 0;
    btBoxShape shape(btVector3(static_cast<btScalar>(half_ext.x > 0 ? half_ext.x : 0.01f),
                               static_cast<btScalar>(half_ext.y > 0 ? half_ext.y : 0.01f),
                               static_cast<btScalar>(half_ext.z > 0 ? half_ext.z : 0.01f)));
    btCollisionObject probe;
    probe.setCollisionShape(&shape);
    btTransform xf;
    xf.setIdentity();
    xf.setOrigin(to_bt(center));
    xf.setRotation(btQuaternion(rot.x, rot.y, rot.z, rot.w));
    probe.setWorldTransform(xf);

    OverlapCollector cb;
    cb.probe = &probe;
    cb.out = out_idx;
    cb.max = max;
    cb.skip_trig = !hit_triggers;
    cb.m_collisionFilterGroup = -1;
    cb.m_collisionFilterMask  = static_cast<int>(layer_mask);
    bw->world->contactTest(&probe, cb);
    return cb.count;
}

JceBulletRayResult jce_bullet_sweep_sphere(JceBulletWorld *bw, jce_vec3 origin,
                                           float radius, jce_vec3 dir,
                                           float max_dist, uint32_t layer_mask,
                                           bool hit_triggers)
{
    JceBulletRayResult result;
    std::memset(&result, 0, sizeof(result));
    result.body_idx = UINT32_MAX;
    if (!bw || !bw->world || radius <= 0.0f) return result;

    btVector3 d = to_bt(dir);
    if (max_dist <= 0.0f || d.length2() < SIMD_EPSILON) return result;
    btVector3 from_o = to_bt(origin);
    btVector3 to_o   = from_o + d.normalized() * static_cast<btScalar>(max_dist);

    btTransform from_xf;
    from_xf.setIdentity();
    from_xf.setOrigin(from_o);
    btTransform to_xf;
    to_xf.setIdentity();
    to_xf.setOrigin(to_o);

    btSphereShape shape(static_cast<btScalar>(radius));
    FilteredClosestConvex cb(from_o, to_o, layer_mask, hit_triggers);
    bw->world->convexSweepTest(&shape, from_xf, to_xf, cb);
    if (!cb.hasHit()) return result;

    result.hit      = true;
    result.point    = from_bt_v3(cb.m_hitPointWorld);
    result.normal   = from_bt_v3(cb.m_hitNormalWorld);
    result.distance = static_cast<float>(cb.m_closestHitFraction) * max_dist;
    result.body_idx = body_idx_of(cb.m_hitCollisionObject);
    return result;
}

/* ================================================================== */
/* Contact callback registration                                       */
/* ================================================================== */

void jce_bullet_set_contact_begin(JceBulletWorld *bw,
                                  jce_bullet_contact_fn fn, void *ud)
{
    if (!bw) return;
    bw->contact_begin_fn = fn;
    bw->contact_begin_ud = ud;
}

void jce_bullet_set_contact_end(JceBulletWorld *bw,
                                jce_bullet_contact_fn fn, void *ud)
{
    if (!bw) return;
    bw->contact_end_fn = fn;
    bw->contact_end_ud = ud;
}

/* ================================================================== */
/* Utility                                                             */
/* ================================================================== */

uint32_t jce_bullet_body_count(JceBulletWorld *bw)
{
    return bw ? bw->count : 0;
}

/* ================================================================== */
/* Collision filter                                                    */
/* ================================================================== */

void jce_bullet_body_set_collision_filter(JceBulletWorld *bw, uint32_t idx,
                                          uint32_t group, uint32_t mask)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;

    btRigidBody *body = bw->bodies[slot];
    if (!body) return;

    /* Must remove and re-add to change filter group/mask. */
    bw->world->removeRigidBody(body);
    bw->world->addRigidBody(body,
                             static_cast<int>(group),
                             static_cast<int>(mask));
}

void jce_bullet_body_set_material(JceBulletWorld *bw, uint32_t idx,
                                  float friction, float restitution,
                                  JcePhysicsCombine friction_combine,
                                  JcePhysicsCombine restitution_combine)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    body->setFriction(static_cast<btScalar>(friction));
    body->setRestitution(static_cast<btScalar>(restitution));
    /* Arm the per-contact combine for this body.  CF_CUSTOM_MATERIAL_CALLBACK
     * makes Bullet route this body's contacts through the global hook; it is
     * set only for bodies that actually carry a material, so nothing else in
     * the world pays for the feature. */
    body->setUserIndex2(jce_bullet_pack_combine(friction_combine,
                                                restitution_combine));
    body->setCollisionFlags(body->getCollisionFlags() |
                            btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
    body->activate();
}

/* ================================================================== */
/* Continuous Collision Detection (CCD)  (P3-C.3)                      */
/*                                                                    */
/* Bullet enables CCD on a body when its motion threshold is > 0; the */
/* swept-sphere radius defines an embedded sphere used by the         */
/* time-of-impact (TOI) solver.  Setting threshold to 0 disables CCD. */
/* ================================================================== */

float jce_bullet_body_compute_auto_swept_radius(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return 0.0f;
    btCollisionShape *shape = bw->shapes[slot];
    if (!shape) return 0.0f;

    btTransform t;
    t.setIdentity();
    btVector3 aabb_min, aabb_max;
    shape->getAabb(t, aabb_min, aabb_max);
    btVector3 half = (aabb_max - aabb_min) * btScalar(0.5);

    btScalar min_half = half.x();
    if (half.y() < min_half) min_half = half.y();
    if (half.z() < min_half) min_half = half.z();
    if (min_half <= btScalar(0)) return 0.0f;

    /* Slightly smaller than the smallest half-extent so the embedded
     * sphere stays inside the shape and TOI queries remain stable. */
    return static_cast<float>(min_half * btScalar(0.5));
}

void jce_bullet_body_set_ccd(JceBulletWorld *bw, uint32_t idx,
                             float motion_threshold,
                             float swept_sphere_radius)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;

    if (motion_threshold < 0.0f) motion_threshold = 0.0f;

    if (motion_threshold > 0.0f && swept_sphere_radius <= 0.0f) {
        swept_sphere_radius = jce_bullet_body_compute_auto_swept_radius(bw, idx);
    }
    if (swept_sphere_radius < 0.0f) swept_sphere_radius = 0.0f;

    body->setCcdMotionThreshold(static_cast<btScalar>(motion_threshold));
    body->setCcdSweptSphereRadius(static_cast<btScalar>(swept_sphere_radius));
}

float jce_bullet_body_get_ccd_motion_threshold(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return 0.0f;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return 0.0f;
    return static_cast<float>(body->getCcdMotionThreshold());
}

float jce_bullet_body_get_ccd_swept_sphere_radius(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return 0.0f;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return 0.0f;
    return static_cast<float>(body->getCcdSweptSphereRadius());
}

/* ================================================================== */
/* Constraints                                                         */
/* ================================================================== */

uint32_t jce_bullet_constraint_create(JceBulletWorld *bw,
                                       uint8_t type,
                                       uint32_t body_a, uint32_t body_b,
                                       jce_vec3 pivot_a, jce_vec3 pivot_b,
                                       jce_vec3 axis,
                                       float lower, float upper,
                                       bool disable_collision)
{
    if (!bw) return UINT32_MAX;
    /* body_a / body_b arrive as packed handles — resolve to live slots. */
    uint32_t slot_a = resolve_body(bw, body_a);
    if (slot_a == UINT32_MAX) return UINT32_MAX;

    /* Find a free constraint slot (rotating cursor → O(1) amortized bursts). */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->con_capacity; ++n) {
        uint32_t i = (bw->con_alloc_cursor + n) % bw->con_capacity;
        if (!bw->con_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->con_alloc_cursor = (idx + 1u) % bw->con_capacity;

    btRigidBody *rb_a = bw->bodies[slot_a];
    btRigidBody *rb_b = nullptr;
    uint32_t slot_b = resolve_body(bw, body_b);
    bool has_b = (slot_b != UINT32_MAX);
    if (has_b) rb_b = bw->bodies[slot_b];

    btTypedConstraint *con = nullptr;

    switch (static_cast<JceConstraintType>(type)) {
    case JCE_CONSTRAINT_POINT2POINT: {
        if (has_b && rb_b) {
            con = new btPoint2PointConstraint(
                *rb_a, *rb_b, to_bt(pivot_a), to_bt(pivot_b));
        } else {
            con = new btPoint2PointConstraint(*rb_a, to_bt(pivot_a));
        }
        break;
    }
    case JCE_CONSTRAINT_HINGE: {
        btVector3 bt_axis = to_bt(axis);
        if (bt_axis.length2() < 0.001f) bt_axis = btVector3(0, 1, 0);
        bt_axis.normalize();
        if (has_b && rb_b) {
            con = new btHingeConstraint(
                *rb_a, *rb_b, to_bt(pivot_a), to_bt(pivot_b),
                bt_axis, bt_axis);
        } else {
            con = new btHingeConstraint(*rb_a, to_bt(pivot_a), bt_axis);
        }
        auto *hinge = static_cast<btHingeConstraint *>(con);
        if (lower < upper)
            hinge->setLimit(static_cast<btScalar>(lower),
                            static_cast<btScalar>(upper));
        break;
    }
    case JCE_CONSTRAINT_SLIDER: {
        btTransform frame_a, frame_b;
        frame_a.setIdentity();
        frame_a.setOrigin(to_bt(pivot_a));
        frame_b.setIdentity();
        frame_b.setOrigin(to_bt(pivot_b));
        if (has_b && rb_b) {
            auto *slider = new btSliderConstraint(
                *rb_a, *rb_b, frame_a, frame_b, true);
            slider->setLowerLinLimit(static_cast<btScalar>(lower));
            slider->setUpperLinLimit(static_cast<btScalar>(upper));
            con = slider;
        } else {
            auto *slider = new btSliderConstraint(
                *rb_a, frame_a, true);
            slider->setLowerLinLimit(static_cast<btScalar>(lower));
            slider->setUpperLinLimit(static_cast<btScalar>(upper));
            con = slider;
        }
        break;
    }
    case JCE_CONSTRAINT_GENERIC6DOF: {
        btTransform frame_a, frame_b;
        frame_a.setIdentity();
        frame_a.setOrigin(to_bt(pivot_a));
        frame_b.setIdentity();
        frame_b.setOrigin(to_bt(pivot_b));
        if (has_b && rb_b) {
            auto *dof = new btGeneric6DofConstraint(
                *rb_a, *rb_b, frame_a, frame_b, true);
            dof->setLinearLowerLimit(btVector3(lower, lower, lower));
            dof->setLinearUpperLimit(btVector3(upper, upper, upper));
            con = dof;
        } else {
            auto *dof = new btGeneric6DofConstraint(
                *rb_a, frame_a, true);
            dof->setLinearLowerLimit(btVector3(lower, lower, lower));
            dof->setLinearUpperLimit(btVector3(upper, upper, upper));
            con = dof;
        }
        break;
    }
    default:
        return UINT32_MAX;
    }

    if (!con) return UINT32_MAX;

    /* Every joint is queryable through jce_bullet_constraint_applied_impulse
     * (break monitors poll it each tick).  Bullet only accumulates
     * m_appliedImpulse when feedback is enabled; without this the query
     * btAsserts in Debug and silently returns 0 in Release — which made
     * impulse-based joint breaking inert.  Cost is one scalar store per
     * solver iteration. */
    jce_bullet_con_register(bw, idx, con, disable_collision);

    return idx;
}

void jce_bullet_constraint_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->con_capacity || !bw->con_alive[idx]) return;

    btTypedConstraint *con = bw->constraints[idx];
    if (con) {
        bw->world->removeConstraint(con);
        delete con;
    }
    bw->constraints[idx] = nullptr;
    bw->con_alive[idx] = false;
    bw->con_count--;
}

void jce_bullet_constraint_set_limits(JceBulletWorld *bw, uint32_t idx,
                                       float lower, float upper)
{
    if (!bw || idx >= bw->con_capacity || !bw->con_alive[idx]) return;

    btTypedConstraint *con = bw->constraints[idx];
    if (!con) return;

    switch (con->getConstraintType()) {
    case HINGE_CONSTRAINT_TYPE: {
        auto *hinge = static_cast<btHingeConstraint *>(con);
        hinge->setLimit(static_cast<btScalar>(lower),
                        static_cast<btScalar>(upper));
        break;
    }
    case SLIDER_CONSTRAINT_TYPE: {
        auto *slider = static_cast<btSliderConstraint *>(con);
        slider->setLowerLinLimit(static_cast<btScalar>(lower));
        slider->setUpperLinLimit(static_cast<btScalar>(upper));
        break;
    }
    /* BOTH TAGS.  A configurable joint is built as a
     * btGeneric6DofSpringConstraint, whose ctor sets m_objectType to
     * D6_SPRING_CONSTRAINT_TYPE -- so matching only D6_CONSTRAINT_TYPE here
     * would take `default: break` and set no limits at all, silently. */
    case D6_CONSTRAINT_TYPE:
    case D6_SPRING_CONSTRAINT_TYPE: {
        auto *dof = static_cast<btGeneric6DofConstraint *>(con);
        dof->setLinearLowerLimit(btVector3(lower, lower, lower));
        dof->setLinearUpperLimit(btVector3(upper, upper, upper));
        break;
    }
    default:
        break;
    }
}

/* ── Configurable joint (Unity-style per-axis 6DOF) ──────────────────
 *
 * A btGeneric6DofConstraint with INDEPENDENT per-axis limits, mapped from the
 * authored Locked/Limited/Free motion of each linear + angular axis.  Built and
 * registered exactly like jce_bullet_constraint_create's type-3 path (same free-
 * slot search, same registry arrays, same addConstraint flag) so the returned
 * slot is destroyed by jce_bullet_constraint_destroy and queried by
 * jce_bullet_constraint_applied_impulse — no parallel registry.
 *
 * btGeneric6DofConstraint axis indices (verified against the type-3 usage above
 * and getAngular/LinearLimit accessors in the introspection block): 0,1,2 =
 * linear X/Y/Z; 3,4,5 = angular X/Y/Z.  Per-axis setLimit(axis, lo, hi):
 *   LOCKED  -> setLimit(axis, 0, 0)            (lo == hi  -> axis is locked)
 *   FREE    -> setLimit(axis, 1, 0)            (lo  > hi  -> axis is free)
 *   LIMITED -> setLimit(axis, -L, +L)          (symmetric bound) */
static void cfg_apply_axis(btGeneric6DofConstraint *dof, int axis,
                           int motion, btScalar limit)
{
    switch (motion) {
    case 2: /* FREE    */ dof->setLimit(axis, btScalar(1), btScalar(0)); break;
    /* LIMITED.  This case used to fall through to LOCKED and `limit` was an
     * unused parameter, so every LIMITED axis behaved as LOCKED and the
     * authored linear_limit / angular_limit_deg did nothing at all -- while
     * the Inspector drew them and the scene file stored them.  The comment
     * above this function had said `LIMITED -> setLimit(axis, -L, +L)` the
     * whole time; only the code disagreed.
     *
     * A NEGATIVE limit would invert the pair and Bullet reads lo > hi as
     * FREE, turning a mis-authored bound into no bound at all.  Clamped to
     * its magnitude so the worst an author gets is a symmetric limit. */
    case 1: {
        btScalar l = limit < btScalar(0) ? -limit : limit;
        dof->setLimit(axis, -l, l);
        break;
    }
    case 0: /* LOCKED  */
    default:              dof->setLimit(axis, btScalar(0), btScalar(0)); break;
    }
}

uint32_t jce_bullet_configurable_joint_create(JceBulletWorld *bw,
                                              uint32_t body_a, uint32_t body_b,
                                              jce_vec3 anchor_a,
                                              jce_vec3 anchor_b,
                                              const int lin_motion[3],
                                              const int ang_motion[3],
                                              float linear_limit,
                                              const float angular_limit_rad[3],
                                              bool disable_collision)
{
    if (!bw) return UINT32_MAX;
    uint32_t slot_a = resolve_body(bw, body_a);
    if (slot_a == UINT32_MAX) return UINT32_MAX;

    /* Free constraint slot (same rotating cursor as the typed-constraint path). */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->con_capacity; ++n) {
        uint32_t i = (bw->con_alloc_cursor + n) % bw->con_capacity;
        if (!bw->con_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->con_alloc_cursor = (idx + 1u) % bw->con_capacity;

    btRigidBody *rb_a = bw->bodies[slot_a];
    btRigidBody *rb_b = nullptr;
    uint32_t slot_b = resolve_body(bw, body_b);
    bool has_b = (slot_b != UINT32_MAX);
    if (has_b) rb_b = bw->bodies[slot_b];

    /* Frames: identity basis + the anchor as origin on A.  For body B we
     * AUTO-CONFIGURE the connected frame to the world position of frame_a at
     * spawn (Unity autoConfigureConnectedAnchor default) so a LOCKED axis HOLDS
     * the bodies' current relative pose instead of collapsing them to anchor
     * coincidence.  (anchor_b is the auto-configured value; an explicit
     * rest-offset connected_anchor is a follow-up.) */
    btTransform frame_a, frame_b;
    frame_a.setIdentity();
    frame_a.setOrigin(to_bt(anchor_a));
    frame_b.setIdentity();
    frame_b.setOrigin(to_bt(anchor_b));
    if (has_b && rb_b) {
        btTransform world_anchor = rb_a->getCenterOfMassTransform() * frame_a;
        frame_b = rb_b->getCenterOfMassTransform().inverse() * world_anchor;
    }

    /* btGeneric6DofSpringConstraint, not btGeneric6DofConstraint -- and this
     * does NOT change what an existing joint does.  It DERIVES from the plain
     * 6DOF; its init() sets every m_springEnabled[i] false; and its getInfo2
     * skips each spring behind that flag before delegating to the base.  With
     * no spring enabled it is the base constraint exactly.  Building it here
     * is what makes JCE_JOINT_DRIVE_SPRING reachable later without a second
     * joint type an author would have to choose between up front. */
    btGeneric6DofSpringConstraint *dof = nullptr;
    if (has_b && rb_b) {
        dof = new btGeneric6DofSpringConstraint(*rb_a, *rb_b, frame_a, frame_b, true);
    } else {
        /* World-anchored: single-body ctor, useLinearReferenceFrameA = true
         * (Bullet substitutes its static fixed body for side B). */
        dof = new btGeneric6DofSpringConstraint(*rb_a, frame_a, true);
    }
    if (!dof) return UINT32_MAX;

    btScalar lin = btScalar(linear_limit);
    for (int a = 0; a < 3; ++a)
        cfg_apply_axis(dof, a, lin_motion ? lin_motion[a] : 0, lin);
    for (int a = 0; a < 3; ++a) {
        btScalar al = angular_limit_rad ? btScalar(angular_limit_rad[a]) : btScalar(0);
        cfg_apply_axis(dof, 3 + a, ang_motion ? ang_motion[a] : 0, al);
    }

    /* Registration, feedback and joint-feedback storage all in one place --
     * see jce_bullet_con_register.  This call site is why it exists: it used to
     * enable feedback and NOT install the storage, which left break_torque
     * reading a flat zero on the only joint type that has one. */
    jce_bullet_con_register(bw, idx, dof, disable_collision);

    return idx;
}

void jce_bullet_constraint_set_motor(JceBulletWorld *bw, uint32_t idx,
                                     bool enabled, float target_velocity,
                                     float max_force, float fixed_dt)
{
    if (!bw || idx >= bw->con_capacity || !bw->con_alive[idx]) return;
    btTypedConstraint *con = bw->constraints[idx];
    if (!con) return;

    switch (con->getConstraintType()) {
    case HINGE_CONSTRAINT_TYPE: {
        auto *hinge = static_cast<btHingeConstraint *>(con);
        /* THE ONE CONVERSION.  enableAngularMotor's third argument is a
         * maximum IMPULSE (btHingeConstraint.cpp:559 puts it straight into
         * the solver row's limit), while the author gave a torque.  A step
         * of 0 would silently disarm the motor, so fall back to 1/60 --
         * jce_physics.c never passes 0, and a caller that somehow did should
         * get a working motor rather than a mute one. */
        const btScalar dt = fixed_dt > 0.0f ? btScalar(fixed_dt)
                                            : btScalar(1.0 / 60.0);
        hinge->enableAngularMotor(enabled,
                                  static_cast<btScalar>(target_velocity),
                                  static_cast<btScalar>(max_force) * dt);
        /* A body already asleep will not wake for a motor that starts this
         * frame, and an author who just switched a door on has no way to
         * know why nothing moved. */
        if (enabled) {
            hinge->getRigidBodyA().activate(true);
            hinge->getRigidBodyB().activate(true);
        }
        break;
    }
    case SLIDER_CONSTRAINT_TYPE: {
        auto *slider = static_cast<btSliderConstraint *>(con);
        /* NO conversion here: btSliderConstraint.cpp:510 divides by info->fps
         * itself, so this really is a force. */
        slider->setPoweredLinMotor(enabled);
        slider->setTargetLinMotorVelocity(static_cast<btScalar>(target_velocity));
        slider->setMaxLinMotorForce(static_cast<btScalar>(max_force));
        if (enabled) {
            slider->getRigidBodyA().activate(true);
            slider->getRigidBodyB().activate(true);
        }
        break;
    }
    default:
        break;
    }
}

void jce_bullet_configurable_joint_set_drive(JceBulletWorld *bw, uint32_t idx,
                                             int axis, int mode, float target,
                                             float spring, float damper,
                                             float max_force)
{
    if (!bw || idx >= bw->con_capacity || !bw->con_alive[idx]) return;
    if (axis < 0 || axis > 5) return;
    btTypedConstraint *con = bw->constraints[idx];
    const int ctype = con->getConstraintType();
    if (ctype != D6_CONSTRAINT_TYPE && ctype != D6_SPRING_CONSTRAINT_TYPE)
        return;

    /* Every configurable joint this bridge builds is a
     * btGeneric6DofSpringConstraint (see the create path).  A D6 built by the
     * TYPED constraint path is a plain btGeneric6DofConstraint and has no
     * springs, so SPRING mode is refused there rather than crashing on a
     * cast -- the velocity motor still works, because it lives on the base. */
    auto *dof = static_cast<btGeneric6DofConstraint *>(con);

    btRotationalLimitMotor    *rot = (axis >= 3) ? dof->getRotationalLimitMotor(axis - 3)
                                                 : nullptr;
    btTranslationalLimitMotor *lin = (axis < 3)  ? dof->getTranslationalLimitMotor()
                                                 : nullptr;

    /* A BULLET SPRING ACTS THROUGH THE LIMIT MOTOR.  internalUpdateSprings
     * writes m_targetVelocity and m_maxMotorForce from Hooke's law every step
     * and NEVER touches m_enableMotor -- and the base getInfo2 emits no motor
     * row unless that flag is set (btTranslationalLimitMotor::needApplyForce).
     * So a spring with the motor left off computes a force each step and
     * applies none of it: measured, the pair did not move by a single
     * millimetre in 300 steps.  Two mechanisms, one switch. */
    const bool velocity = (mode == 1);
    const bool spring_m = (mode == 2);
    const bool motor_on = velocity || spring_m;

    /* THE SPRING FLAG FIRST, AND THAT ORDER IS LOAD-BEARING.
     * btGeneric6DofSpringConstraint::enableSpring writes m_springEnabled AND
     * m_enableMotor -- so calling it after the motor state, with
     * want_spring == false, turns the velocity motor straight back off.
     * Measured: the bridge was reached with the right arguments and the axis
     * did not move, because this call undid the line above it. */
    if (ctype == D6_SPRING_CONSTRAINT_TYPE) {
        auto *sp = static_cast<btGeneric6DofSpringConstraint *>(con);
        sp->enableSpring(axis, spring_m);
        if (spring_m) {
            sp->setStiffness(axis, static_cast<btScalar>(spring));
            sp->setDamping(axis, static_cast<btScalar>(damper));
            /* The equilibrium is the AUTHORED target, set explicitly.  The
             * no-argument overload snapshots wherever the joint happens to be
             * at the moment of the call, which would make a spring's rest
             * pose depend on when the component was applied.
             *
             * ZERO IS THE SPAWN POSE, not the world origin: the create path
             * auto-configures frame_b to frame_a's world position, so the
             * target is an offset FROM where the bodies started. */
            sp->setEquilibriumPoint(axis, static_cast<btScalar>(target));
        }
    }

    if (rot) {
        rot->m_enableMotor  = motor_on;
        rot->m_targetVelocity = static_cast<btScalar>(target);
        /* A FORCE, not an impulse: btGeneric6DofConstraint.cpp:777 divides by
         * info->fps.  Different from the hinge above, on purpose, because
         * Bullet is.  In SPRING mode both of these are overwritten by
         * internalUpdateSprings on every step, so what is written here only
         * matters for VELOCITY. */
        rot->m_maxMotorForce  = static_cast<btScalar>(max_force);
    } else if (lin) {
        lin->m_enableMotor[axis]     = motor_on;
        lin->m_targetVelocity[axis]  = static_cast<btScalar>(target);
        lin->m_maxMotorForce[axis]   = static_cast<btScalar>(max_force);
    }

    if (mode != 0) {
        dof->getRigidBodyA().activate(true);
        dof->getRigidBodyB().activate(true);
    }
}

/* Constraint feedback queries live in jce_physics_bullet_con_query.cpp: pure
 * reads over the registry, moved out when adding the torque one made this a
 * god file. */

/* ================================================================== */
/* Joint introspection (P3-C.6)                                        */
/* ================================================================== */

/* Map a btRigidBody back to its packed handle via the userPointer stash
 * set up at body_create time.  Returns UINT32_MAX when the body was
 * never tagged (e.g. the static "fixed body" Bullet uses internally
 * for world-anchored constraints). */
static uint32_t body_index_from_rb(const btRigidBody *rb)
{
    if (!rb) return UINT32_MAX;
    const void *up = rb->getUserPointer();
    if (!up) return UINT32_MAX;
    uintptr_t v = reinterpret_cast<uintptr_t>(up);
    /* userPointer stores the PACKED handle (slot + generation).  Returned
     * verbatim so the C layer / editor receive a handle, not a bare slot. */
    return static_cast<uint32_t>(v);
}

bool jce_bullet_joint_get_info_for_body(JceBulletWorld *bw,
                                        uint32_t body_idx,
                                        JceBulletJointInfo *out)
{
    if (!bw || !out) return false;
    /* body_idx arrives as a packed handle — resolve to a live slot. */
    uint32_t slot = resolve_body(bw, body_idx);
    if (slot == UINT32_MAX) return false;

    btRigidBody *target = bw->bodies[slot];
    if (!target || !bw->constraints) return false;

    /* Locate the first constraint where either side is `target`. */
    btTypedConstraint *con = nullptr;
    for (uint32_t i = 0; i < bw->con_capacity; ++i) {
        if (!bw->con_alive[i] || !bw->constraints[i]) continue;
        btTypedConstraint *c = bw->constraints[i];
        if (&c->getRigidBodyA() == target || &c->getRigidBodyB() == target) {
            con = c;
            break;
        }
    }
    if (!con) return false;

    btRigidBody &rb_a = con->getRigidBodyA();
    btRigidBody &rb_b = con->getRigidBodyB();

    memset(out, 0, sizeof(*out));
    out->body_a = body_index_from_rb(&rb_a);
    out->body_b = body_index_from_rb(&rb_b);

    /* World-anchored constraints sometimes reference Bullet's static
     * "fixed body" singleton on side B.  Mark it as invalid. */
    if (&rb_b == &btTypedConstraint::getFixedBody()) {
        out->body_b = UINT32_MAX;
    }

    switch (con->getConstraintType()) {
    case POINT2POINT_CONSTRAINT_TYPE: {
        auto *p2p = static_cast<btPoint2PointConstraint *>(con);
        btVector3 wa = rb_a.getCenterOfMassTransform() * p2p->getPivotInA();
        btVector3 wb = rb_b.getCenterOfMassTransform() * p2p->getPivotInB();
        out->kind = 1; /* BALL */
        out->anchor_a = from_bt_v3(wa);
        out->anchor_b = from_bt_v3(wb);
        out->axis = jce_v3(0.0f, 1.0f, 0.0f);
        break;
    }
    case HINGE_CONSTRAINT_TYPE: {
        auto *h = static_cast<btHingeConstraint *>(con);
        const btTransform &fa = h->getAFrame();
        const btTransform &fb = h->getBFrame();
        btTransform wa_xf = rb_a.getCenterOfMassTransform() * fa;
        btTransform wb_xf = rb_b.getCenterOfMassTransform() * fb;
        /* Hinge axis is the Z column of the constraint frame (Bullet
         * convention — see btHingeConstraint.cpp). */
        btVector3 axis_world = wa_xf.getBasis().getColumn(2);
        if (axis_world.length2() > 1e-8f) axis_world.normalize();
        out->kind = 2; /* HINGE */
        out->anchor_a = from_bt_v3(wa_xf.getOrigin());
        out->anchor_b = from_bt_v3(wb_xf.getOrigin());
        out->axis = from_bt_v3(axis_world);
        out->limit_low  = static_cast<float>(h->getLowerLimit());
        out->limit_high = static_cast<float>(h->getUpperLimit());
        break;
    }
    case SLIDER_CONSTRAINT_TYPE: {
        auto *s = static_cast<btSliderConstraint *>(con);
        const btTransform &fa = s->getFrameOffsetA();
        const btTransform &fb = s->getFrameOffsetB();
        btTransform wa_xf = rb_a.getCenterOfMassTransform() * fa;
        btTransform wb_xf = rb_b.getCenterOfMassTransform() * fb;
        /* Slider axis is the X column of frame A (Bullet convention). */
        btVector3 axis_world = wa_xf.getBasis().getColumn(0);
        if (axis_world.length2() > 1e-8f) axis_world.normalize();
        out->kind = 3; /* SLIDER */
        out->anchor_a = from_bt_v3(wa_xf.getOrigin());
        out->anchor_b = from_bt_v3(wb_xf.getOrigin());
        out->axis = from_bt_v3(axis_world);
        out->limit_low  = static_cast<float>(s->getLowerLinLimit());
        out->limit_high = static_cast<float>(s->getUpperLinLimit());
        break;
    }
    /* BOTH TAGS -- see set_limits.  Without D6_SPRING_CONSTRAINT_TYPE the
     * editor's joint gizmo stops drawing every configurable joint, which
     * reads as "the joint is gone" rather than as a missing case. */
    case D6_CONSTRAINT_TYPE:
    case D6_SPRING_CONSTRAINT_TYPE: {
        auto *d = static_cast<btGeneric6DofConstraint *>(con);
        const btTransform &fa = d->getFrameOffsetA();
        const btTransform &fb = d->getFrameOffsetB();
        btTransform wa_xf = rb_a.getCenterOfMassTransform() * fa;
        btTransform wb_xf = rb_b.getCenterOfMassTransform() * fb;
        btVector3 axis_world = wa_xf.getBasis().getColumn(0);
        if (axis_world.length2() > 1e-8f) axis_world.normalize();
        out->kind = 4; /* 6DOF */
        out->anchor_a = from_bt_v3(wa_xf.getOrigin());
        out->anchor_b = from_bt_v3(wb_xf.getOrigin());
        out->axis = from_bt_v3(axis_world);

        btVector3 ll, lu, al, au;
        d->getLinearLowerLimit(ll);
        d->getLinearUpperLimit(lu);
        d->getAngularLowerLimit(al);
        d->getAngularUpperLimit(au);
        out->linear_lower  = from_bt_v3(ll);
        out->linear_upper  = from_bt_v3(lu);
        out->angular_lower = from_bt_v3(al);
        out->angular_upper = from_bt_v3(au);
        break;
    }
    default:
        return false;
    }
    return true;
}

/* Character controller moved to jce_physics_bullet_char.cpp; the world
 * struct and the char_* tables it needs are in
 * jce_physics_bullet_internal.hpp. */
/* Vehicle controller moved to jce_physics_bullet_vehicle.cpp; the world
 * struct it needs is in jce_physics_bullet_internal.hpp. */
/* ================================================================== */
/* P3-C.5: trigger flag query                                          */
/* ================================================================== */

bool jce_bullet_body_is_trigger(JceBulletWorld *bw, uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return false;
    const btRigidBody *body = bw->bodies[slot];
    if (!body) return false;
    return (body->getCollisionFlags() &
            btCollisionObject::CF_NO_CONTACT_RESPONSE) != 0;
}

/* ================================================================== */
/* P3-C.5: manifold pair enumeration                                   */
/*                                                                     */
/* Walks every persistent manifold once and reports the deepest active */
/* contact point per pair.  Trigger pairs (CF_NO_CONTACT_RESPONSE) are */
/* reported with is_trigger=true so the C layer can flag the event.    */
/* ================================================================== */

void jce_bullet_enumerate_pairs(JceBulletWorld *bw,
                                jce_bullet_pair_fn fn, void *ud)
{
    if (!bw || !bw->world || !fn) return;

    btDispatcher *dp = bw->world->getDispatcher();
    int num_manifolds = dp->getNumManifolds();

    for (int i = 0; i < num_manifolds; ++i) {
        btPersistentManifold *manifold = dp->getManifoldByIndexInternal(i);
        int num_contacts = manifold->getNumContacts();
        if (num_contacts == 0) continue;

        const btCollisionObject *obj_a = manifold->getBody0();
        const btCollisionObject *obj_b = manifold->getBody1();
        if (!obj_a || !obj_b) continue;

        /* Skip vehicle chassis (user-pointer is a vehicle index, not a body
           handle) — decoding it aliases / stale-resolves a real body slot. */
        if (obj_a->getUserIndex() == JCE_BULLET_VEHICLE_CHASSIS_USERINDEX ||
            obj_b->getUserIndex() == JCE_BULLET_VEHICLE_CHASSIS_USERINDEX)
            continue;

        /* Pick the deepest (most negative distance) active contact. */
        int  best   = -1;
        float worst = 0.0f;
        for (int c = 0; c < num_contacts; ++c) {
            const btManifoldPoint &pt = manifold->getContactPoint(c);
            float d = pt.getDistance();
            if (d > 0.0f) continue;          /* separating */
            if (best < 0 || d < worst) { best = c; worst = d; }
        }
        if (best < 0) continue;

        const btManifoldPoint &pt = manifold->getContactPoint(best);
        const btVector3 &n   = pt.m_normalWorldOnB;
        const btVector3 &pos = pt.getPositionWorldOnB();

        uint32_t idx_a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            obj_a->getUserPointer()));
        uint32_t idx_b = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            obj_b->getUserPointer()));

        bool is_trigger =
            (obj_a->getCollisionFlags() &
             btCollisionObject::CF_NO_CONTACT_RESPONSE) ||
            (obj_b->getCollisionFlags() &
             btCollisionObject::CF_NO_CONTACT_RESPONSE);

        float normal[3] = { n.x(), n.y(), n.z() };
        float point[3]  = { pos.x(), pos.y(), pos.z() };

        fn(idx_a, idx_b, normal, point,
           -pt.getDistance(), is_trigger, ud);
    }
}

/* ================================================================== */
/* P3-C.5: btIDebugDraw bridge                                         */
/*                                                                     */
/* The drawer is owned by the JceBulletWorld and lazily attached the   */
/* first time set_mode() goes non-zero.  Bullet's draw modes are bit   */
/* flags that line up with JcePhysicsDebugFlag.                        */
/* ================================================================== */

namespace {

class JceBulletDebugDrawer : public btIDebugDraw {
public:
    JceBulletDebugDrawer() : m_mode(0), m_fn(nullptr), m_ud(nullptr) {}

    void set_sink(jce_bullet_line_fn fn, void *ud) { m_fn = fn; m_ud = ud; }

    /* --- btIDebugDraw --- */
    void drawLine(const btVector3 &from, const btVector3 &to,
                  const btVector3 &color) override
    {
        if (!m_fn) return;
        /* Bullet supplies linear RGB in [0,1]; pack as 0xAABBGGRR. */
        auto clamp01 = [](float v) { return v < 0 ? 0.0f : (v > 1 ? 1.0f : v); };
        uint32_t r = (uint32_t)(clamp01(color.x()) * 255.0f);
        uint32_t g = (uint32_t)(clamp01(color.y()) * 255.0f);
        uint32_t b = (uint32_t)(clamp01(color.z()) * 255.0f);
        uint32_t abgr = 0xFF000000u | (b << 16) | (g << 8) | r;
        m_fn(from.x(), from.y(), from.z(),
             to.x(),   to.y(),   to.z(), abgr, m_ud);
    }

    void drawContactPoint(const btVector3 &point, const btVector3 &normal,
                          btScalar distance, int /*lifetime*/,
                          const btVector3 &color) override
    {
        btVector3 tip = point + normal * distance;
        drawLine(point, tip, color);
    }

    void reportErrorWarning(const char * /*warning*/) override {}
    void draw3dText(const btVector3 & /*location*/,
                    const char * /*text*/) override {}

    void  setDebugMode(int mode) override { m_mode = mode; }
    int   getDebugMode() const override   { return m_mode; }

private:
    int                m_mode;
    jce_bullet_line_fn m_fn;
    void              *m_ud;
};

/* One drawer per world — Bullet stores a raw pointer, no ownership. */
struct DebugDrawerSlot {
    JceBulletWorld       *bw;
    JceBulletDebugDrawer *drawer;
};

/* Single-world editor / game today; a 4-slot fixed table is plenty
 * and keeps the bridge alloc-free per frame. */
static DebugDrawerSlot s_drawers[4];

static JceBulletDebugDrawer *get_or_create_drawer(JceBulletWorld *bw)
{
    for (auto &slot : s_drawers) {
        if (slot.bw == bw) return slot.drawer;
    }
    for (auto &slot : s_drawers) {
        if (slot.bw == nullptr) {
            slot.bw     = bw;
            slot.drawer = new JceBulletDebugDrawer();
            return slot.drawer;
        }
    }
    return nullptr; /* table exhausted — debug draw simply disabled */
}

static void release_drawer(JceBulletWorld *bw)
{
    for (auto &slot : s_drawers) {
        if (slot.bw == bw) {
            delete slot.drawer;
            slot.drawer = nullptr;
            slot.bw     = nullptr;
            return;
        }
    }
}

} /* namespace */

/* Translate JcePhysicsDebugFlag bits to Bullet's btIDebugDraw::DebugDrawModes.
 * These do NOT line up 1:1: JCE CONTACTS(1<<2) would hit Bullet's
 * DBG_DrawFeaturesText, CONSTRAINTS(1<<3) its DBG_DrawContactPoints, and
 * NORMALS(1<<4) its DBG_NoDeactivation — the last silently disables sleeping
 * on every body.  Map explicitly. */
static int jce_to_bt_debug_mode(uint32_t flags)
{
    int m = 0;
    if (flags & (1u << 0)) m |= btIDebugDraw::DBG_DrawWireframe;      /* WIREFRAME   */
    if (flags & (1u << 1)) m |= btIDebugDraw::DBG_DrawAabb;           /* AABB        */
    if (flags & (1u << 2)) m |= btIDebugDraw::DBG_DrawContactPoints;  /* CONTACTS    */
    if (flags & (1u << 3)) m |= btIDebugDraw::DBG_DrawConstraints |
                                btIDebugDraw::DBG_DrawConstraintLimits; /* CONSTRAINTS */
    if (flags & (1u << 4)) m |= btIDebugDraw::DBG_DrawNormals;        /* NORMALS     */
    return m;
}

void jce_bullet_debug_set_mode(JceBulletWorld *bw, uint32_t flags)
{
    if (!bw || !bw->world) return;

    if (flags == 0) {
        /* Detach drawer to keep step() fast in the common case. */
        bw->world->setDebugDrawer(nullptr);
        release_drawer(bw);
        return;
    }

    JceBulletDebugDrawer *drawer = get_or_create_drawer(bw);
    if (!drawer) return;
    drawer->setDebugMode(jce_to_bt_debug_mode(flags));
    bw->world->setDebugDrawer(drawer);
}

void jce_bullet_debug_draw(JceBulletWorld *bw,
                           jce_bullet_line_fn fn, void *ud)
{
    if (!bw || !bw->world || !fn) return;

    /* Find the existing drawer — don't create one on the draw path. */
    JceBulletDebugDrawer *drawer = nullptr;
    for (auto &slot : s_drawers) {
        if (slot.bw == bw) { drawer = slot.drawer; break; }
    }
    if (!drawer || drawer->getDebugMode() == 0) return;

    drawer->set_sink(fn, ud);
    bw->world->debugDrawWorld();
    drawer->set_sink(nullptr, nullptr);
}

/* Clean up debug-drawer slot when the world is destroyed.  Tacked
 * onto jce_bullet_destroy via this helper called from the existing
 * destroy path. */
extern "C" void jce_bullet_debug_world_destroyed_(JceBulletWorld *bw)
{
    release_drawer(bw);
}

/* ── P3-C.4: native pointer + default-world tracking ──────────────── */

extern "C" void *jce_bullet_body_get_rigid_native_(JceBulletWorld *bw,
                                                   uint32_t idx)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return nullptr;
    return static_cast<void *>(bw->bodies[slot]);
}

namespace {
JceBulletWorld *g_default_bullet_world = nullptr;
}

extern "C" JceBulletWorld *jce_physics_default_bullet_world_(void)
{
    return g_default_bullet_world;
}

extern "C" void jce_physics_set_default_bullet_world_(JceBulletWorld *bw)
{
    g_default_bullet_world = bw;
}

/* ================================================================== */
/* Heightfield bodies (terrain)                                        */
/* ================================================================== */

/* Bullet does NOT copy the heightfield array -- its own docs say the caller
 * is responsible for maintaining it -- so the shape owns a btScalar copy for
 * its whole lifetime.  btScalar rather than float matters: PHY_FLOAT means
 * btScalar, which is double under BT_USE_DOUBLE_PRECISION, and getting that
 * wrong reads every other sample as garbage. */
struct JceBulletHeightfield : public btHeightfieldTerrainShape {
    btScalar          *samples;
    btTriangleInfoMap *info_map;   /* nullptr unless edge smoothing is on */

    JceBulletHeightfield(int w, int l, btScalar *data,
                         btScalar min_h, btScalar max_h)
        : btHeightfieldTerrainShape(w, l, data, btScalar(1.0),
                                    min_h, max_h, 1 /* up = Y */,
                                    PHY_FLOAT, /*flipQuadEdges=*/false),
          samples(data), info_map(nullptr) {}

    ~JceBulletHeightfield() override
    {
        delete info_map;
        JCE_FREE(samples);
    }
};

uint32_t jce_bullet_body_create_heightfield(JceBulletWorld *bw,
                                            jce_vec3 pos, jce_quat rot,
                                            const float *heights,
                                            uint32_t samples_x,
                                            uint32_t samples_z,
                                            float cell_size_x,
                                            float cell_size_z,
                                            float min_height,
                                            float max_height,
                                            uint8_t diagonal,
                                            float friction, float restitution,
                                            uint32_t col_group,
                                            uint32_t col_mask,
                                            bool is_trigger,
                                            bool smooth_internal_edges)
{
    if (!bw || !bw->world || !heights) return UINT32_MAX;
    if (samples_x < 2u || samples_z < 2u) return UINT32_MAX;
    if (!(cell_size_x > 0.0f) || !(cell_size_z > 0.0f)) return UINT32_MAX;
    if (!(max_height > min_height)) return UINT32_MAX;

    const size_t count = (size_t)samples_x * (size_t)samples_z;

    /* Validate BEFORE allocating.  A non-finite or out-of-range sample would
     * silently corrupt the shape AABB, and Bullet requires min/max to bound
     * the data for the shape's entire lifetime. */
    for (size_t i = 0; i < count; ++i) {
        const float h = heights[i];
        if (!(h == h)) return UINT32_MAX;                    /* NaN */
        if (h < min_height || h > max_height) return UINT32_MAX;
    }

    /* Rotating free-slot search, same as the compound create path. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->capacity; ++n) {
        uint32_t i = (bw->alloc_cursor + n) % bw->capacity;
        if (!bw->alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->alloc_cursor = (idx + 1u) % bw->capacity;

    btScalar *data =
        static_cast<btScalar *>(JCE_MALLOC(count * sizeof(btScalar)));
    if (!data) return UINT32_MAX;
    for (size_t i = 0; i < count; ++i)
        data[i] = static_cast<btScalar>(heights[i]);

    auto *shape = new JceBulletHeightfield(
        static_cast<int>(samples_x), static_cast<int>(samples_z), data,
        static_cast<btScalar>(min_height), static_cast<btScalar>(max_height));

    /* Convention 3: the quad diagonal rule is DATA, not a hidden default.
     * Renderer mesh, CPU raycast and the hole fallback must pick the same one
     * or they disagree by the full corner-to-corner height at cell centres. */
    shape->setUseDiamondSubdivision(diagonal == 1);
    shape->setUseZigzagSubdivision(diagonal == 2);

    /* The shape assumes one world unit between samples; local scaling turns
     * that into the real cell size. */
    shape->setLocalScaling(btVector3(static_cast<btScalar>(cell_size_x),
                                     btScalar(1.0),
                                     static_cast<btScalar>(cell_size_z)));

    if (smooth_internal_edges) {
        shape->info_map = new btTriangleInfoMap();
        btGenerateInternalEdgeInfo(shape, shape->info_map);
        shape->setUserPointer(shape->info_map);
        /* The hook itself is installed in jce_bullet_create; a world always
         * exists before a heightfield can be added to it, so arming it here
         * as well could only ever be a no-op. */
    }

    /* Convention 1: btHeightfieldTerrainShape centres itself on the midpoint
     * of its own AABB in ALL THREE axes -- verified in the Bullet source:
     * m_localOrigin = 0.5*(localAabbMin+localAabbMax) and getVertex()
     * subtracts it.  Callers pass the field's MIN corner, so the centring is
     * undone here rather than left for every caller to rediscover.  Without
     * it the terrain collides half a height-range away from where it is
     * drawn, which reads like a shadow-bias bug and is not one. */
    const btScalar half_x = btScalar(0.5) * btScalar(samples_x - 1u) *
                            static_cast<btScalar>(cell_size_x);
    const btScalar half_z = btScalar(0.5) * btScalar(samples_z - 1u) *
                            static_cast<btScalar>(cell_size_z);
    const btScalar mid_y  = btScalar(0.5) *
                            (static_cast<btScalar>(min_height) +
                             static_cast<btScalar>(max_height));

    btTransform start_xf;
    start_xf.setRotation(to_bt_q(rot));
    start_xf.setOrigin(to_bt(pos) +
                       quatRotate(to_bt_q(rot),
                                  btVector3(half_x, mid_y, half_z)));

    auto *motion = new btDefaultMotionState(start_xf);
    btRigidBody::btRigidBodyConstructionInfo ci(btScalar(0.0), motion, shape,
                                                btVector3(0, 0, 0));
    ci.m_friction    = static_cast<btScalar>(friction);
    ci.m_restitution = static_cast<btScalar>(restitution);

    auto *body = new btRigidBody(ci);
    apply_sleep_thresholds(bw, body);

    if (smooth_internal_edges) {
        body->setCollisionFlags(body->getCollisionFlags() |
                                btCollisionObject::CF_CUSTOM_MATERIAL_CALLBACK);
    }
    if (is_trigger) {
        body->setCollisionFlags(body->getCollisionFlags() |
                                btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }

    uint32_t handle =
        handle_encode(idx, bw->generations ? bw->generations[idx] : 0u);
    body->setUserPointer(reinterpret_cast<void *>(static_cast<uintptr_t>(handle)));

    bw->world->addRigidBody(body, static_cast<int>(col_group),
                            static_cast<int>(col_mask));
    bw->bodies[idx] = body;
    bw->alive[idx]  = true;

    /* Registered as an owned shape so teardown deletes it, and through the
     * destructor the sample copy and the triangle info map with it. */
    if (bw->owned_shapes) {
        auto *owned = new btAlignedObjectArray<btCollisionShape *>();
        owned->push_back(shape);
        bw->owned_shapes[idx] = owned;
    }
    return handle;
}
