/*
 * jce_physics_bullet.cpp  C++ Bullet3 back-end for jce_physics.
 *
 * Wraps btDiscreteDynamicsWorld behind the extern "C" bridge declared
 * in jce_physics_internal.h.  Compiled as C++17.
 */

#include "jce_physics_internal.h"
#include "os/core/jce_memory.h"

extern "C" {
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
}

#include <btBulletDynamicsCommon.h>
#include <BulletCollision/CollisionShapes/btShapeHull.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>
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

void install_bullet_allocator_once()
{
    static bool installed = false;
    if (installed) return;
    btAlignedAllocSetCustom(&bt_jce_alloc_unaligned, &bt_jce_free_unaligned);
    btAlignedAllocSetCustomAligned(&bt_jce_alloc_aligned, &bt_jce_free_aligned);
    installed = true;
}
} /* namespace */

/* ================================================================== */
/* Conversion helpers                                                  */
/* ================================================================== */

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
    bool      grounded;       /* cached probe result: WALKABLE contact */
    bool      touching;       /* raw probe contact (any steepness) */
    bool      probe_valid;    /* probe ran at least once this session */
    bool      jumping;        /* jump() fired; cleared on next grounded */
    btVector3 ground_normal;
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

        uint32_t idx_a = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            obj_a->getUserPointer()));
        uint32_t idx_b = static_cast<uint32_t>(reinterpret_cast<uintptr_t>(
            obj_b->getUserPointer()));

        for (int c = 0; c < num_contacts; ++c) {
            const btManifoldPoint &pt = manifold->getContactPoint(c);
            if (pt.getDistance() > 0.0f) continue; /* separating */

            const btVector3 &n   = pt.m_normalWorldOnB;
            const btVector3 &pos = pt.getPositionWorldOnB();
            float normal[3] = { n.x(), n.y(), n.z() };
            float point[3]  = { pos.x(), pos.y(), pos.z() };

            begin_fn(idx_a, idx_b, normal, point,
                     -pt.getDistance(), begin_ud);
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
    install_bullet_allocator_once();

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
                                bool is_trigger)
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
        col_shape = new btCapsuleShape(
            static_cast<btScalar>(half_ext.x),          /* radius   */
            static_cast<btScalar>(half_ext.y * 2.0f));  /* height   */
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
        return new btCapsuleShape(static_cast<btScalar>(c->half_extents.x),
                                  static_cast<btScalar>(c->half_extents.y * 2.0f));
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
                                  float friction, float restitution)
{
    uint32_t slot = resolve_body(bw, idx);
    if (slot == UINT32_MAX) return;
    btRigidBody *body = bw->bodies[slot];
    if (!body) return;
    body->setFriction(static_cast<btScalar>(friction));
    body->setRestitution(static_cast<btScalar>(restitution));
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

    bw->world->addConstraint(con, disable_collision);
    bw->constraints[idx] = con;
    bw->con_alive[idx] = true;
    bw->con_count++;

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
    case D6_CONSTRAINT_TYPE: {
        auto *dof = static_cast<btGeneric6DofConstraint *>(con);
        dof->setLinearLowerLimit(btVector3(lower, lower, lower));
        dof->setLinearUpperLimit(btVector3(upper, upper, upper));
        break;
    }
    default:
        break;
    }
}

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
    case D6_CONSTRAINT_TYPE: {
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

/* ================================================================== */
/* Character controller                                                */
/* ================================================================== */

uint32_t jce_bullet_character_create(JceBulletWorld *bw,
                                      jce_vec3 pos, float radius,
                                      float height, float step_height,
                                      float max_slope_rad,
                                      float gravity, float jump_speed,
                                      float accel, float air_control)
{
    if (!bw) return UINT32_MAX;

    /* Find a free slot (rotating cursor → O(1) amortized bursts). */
    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->char_capacity; ++n) {
        uint32_t i = (bw->char_alloc_cursor + n) % bw->char_capacity;
        if (!bw->char_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->char_alloc_cursor = (idx + 1u) % bw->char_capacity;

    /* Capsule shape: total height = capsule_height + 2*radius. */
    float capsule_height = height - 2.0f * radius;
    if (capsule_height < 0.01f) capsule_height = 0.01f;

    (void)gravity;  /* dynamic capsule falls under world gravity */

    auto *cap_shape = new btCapsuleShape(
        static_cast<btScalar>(radius),
        static_cast<btScalar>(capsule_height));

    /* DYNAMIC capsule rigid body. The solver resolves it together with whatever
     * it rests on (floor, a crate, a stack of crates) so there is no kinematic-
     * vs-dynamic fight → stacking is stable, no jitter. Rotation is fully locked
     * so it never tips; horizontal motion is driven by setting velocity. A
     * modest mass keeps the mass ratio to light crates solver-stable. */
    btScalar mass = btScalar(10.0);
    btVector3 inertia(0, 0, 0);
    cap_shape->calculateLocalInertia(mass, inertia);

    btTransform start_xf;
    start_xf.setIdentity();
    start_xf.setOrigin(to_bt(pos));

    btRigidBody::btRigidBodyConstructionInfo ci(mass, nullptr, cap_shape, inertia);
    ci.m_startWorldTransform = start_xf;
    ci.m_friction            = btScalar(0.0);  /* horizontal is velocity-driven */
    ci.m_restitution         = btScalar(0.0);
    auto *body = new btRigidBody(ci);
    body->setAngularFactor(btVector3(0, 0, 0));   /* never tip / spin */
    body->setActivationState(DISABLE_DEACTIVATION);
    body->setCollisionFlags(body->getCollisionFlags() |
                            btCollisionObject::CF_CHARACTER_OBJECT);

    bw->world->addRigidBody(body,
                            btBroadphaseProxy::CharacterFilter,
                            btBroadphaseProxy::StaticFilter |
                            btBroadphaseProxy::DefaultFilter);

    bw->characters[idx]  = nullptr;
    bw->ghosts[idx]      = nullptr;
    bw->char_bodies[idx] = body;
    bw->char_jump[idx]   = jump_speed > 0.0f ? jump_speed : 5.0f;
    bw->char_shapes[idx] = cap_shape;
    bw->char_alive[idx]  = true;
    bw->char_count++;

    JceBulletCharFeel *f = &bw->char_feel[idx];
    f->accel       = accel > 0.0f ? accel : 40.0f;
    f->air_control = (air_control > 0.0f) ? air_control : 0.35f;
    if (f->air_control > 1.0f) f->air_control = 1.0f;
    f->step_height = step_height > 0.0f ? step_height : 0.35f;
    btScalar slope = max_slope_rad > 0.0f ? btScalar(max_slope_rad)
                                          : btRadians(btScalar(50.0));
    if (slope > btRadians(btScalar(89.0))) slope = btRadians(btScalar(89.0));
    f->max_slope_cos = (float)btCos(slope);
    f->grounded      = false;
    f->touching      = false;
    f->probe_valid   = false;
    f->jumping       = false;
    f->ground_normal = btVector3(0, 1, 0);

    return idx;
}

void jce_bullet_character_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;

    if (bw->char_bodies[idx]) {
        bw->world->removeRigidBody(bw->char_bodies[idx]);
        delete bw->char_bodies[idx];
        bw->char_bodies[idx] = nullptr;
    }
    delete bw->char_shapes[idx];
    bw->char_shapes[idx] = nullptr;
    bw->char_alive[idx] = false;
    bw->char_count--;
}

/* Down-ray helper shared by the ground probe / snap / step-up.  Casts
 * from `from` straight down `reach` metres against the character mask;
 * fills hit point + normal.  Returns false on miss (or self-hit). */
static bool char_ray_down(JceBulletWorld *bw, const btRigidBody *self,
                          const btVector3 &from, btScalar reach,
                          btVector3 *out_point, btVector3 *out_normal)
{
    btVector3 to = from - btVector3(0, reach, 0);
    btCollisionWorld::ClosestRayResultCallback cb(from, to);
    cb.m_collisionFilterGroup = btBroadphaseProxy::CharacterFilter;
    cb.m_collisionFilterMask  = btBroadphaseProxy::StaticFilter |
                                btBroadphaseProxy::DefaultFilter;
    bw->world->rayTest(from, to, cb);
    if (!cb.hasHit() || cb.m_collisionObject == self) return false;
    if (out_point)  *out_point  = cb.m_hitPointWorld;
    if (out_normal) *out_normal = cb.m_hitNormalWorld;
    return true;
}

/* Refresh the cached grounded state + ground normal: a 5-ray fan
 * (capsule axis + 4 compass points at 0.6 r) so standing on an edge or
 * stair lip still reads as grounded; keeps the most upright normal. */
static bool char_ground_probe(JceBulletWorld *bw, uint32_t idx)
{
    btRigidBody *b = bw->char_bodies[idx];
    auto *cap = static_cast<btCapsuleShape *>(bw->char_shapes[idx]);
    JceBulletCharFeel *f = &bw->char_feel[idx];
    btScalar half = cap->getHalfHeight() + cap->getRadius();  /* centre→foot */
    btScalar ring = cap->getRadius() * btScalar(0.6);
    btVector3 c   = b->getWorldTransform().getOrigin();
    btScalar reach = half + btScalar(0.20);

    static const btScalar offs[5][2] = {
        {0, 0}, {1, 0}, {-1, 0}, {0, 1}, {0, -1}
    };
    bool      hit_any = false;
    btVector3 best_n(0, 1, 0);
    btScalar  best_y = btScalar(-2.0);
    for (int i = 0; i < 5; ++i) {
        btVector3 from = c + btVector3(offs[i][0] * ring, 0, offs[i][1] * ring);
        btVector3 n;
        if (char_ray_down(bw, b, from, reach, nullptr, &n)) {
            hit_any = true;
            if (n.y() > best_y) { best_y = n.y(); best_n = n; }
        }
    }
    bool grounded = hit_any;
    /* Ascending from a jump the feet stay within probe reach for a tick
     * or two — that must NOT read as grounded (it would re-arm coyote
     * time and skip the variable-jump cut). */
    if (f->jumping && b->getLinearVelocity().y() > btScalar(0.5))
        grounded = false;
    /* A face steeper than the slope limit supports no locomotion: report
     * airborne so animation shows the slide and jumps can't pogo up it. */
    if (grounded && best_n.y() < btScalar(f->max_slope_cos))
        grounded = false;
    f->grounded      = grounded;
    f->touching      = hit_any;
    f->probe_valid   = true;
    f->ground_normal = hit_any ? best_n : btVector3(0, 1, 0);
    return grounded;
}

/* `walk_dir` is the desired planar VELOCITY (m/s).  The horizontal
 * velocity ACCELERATES toward it (accel on ground, accel*air_control
 * airborne) for natural starts/stops; the solver-owned vertical velocity
 * (gravity / jump / resting) is preserved.  Also handles, per fixed tick:
 *   - ground probe refresh (cached for is_grounded queries),
 *   - ground snap when walking down steps/slopes (kills the airborne arc),
 *   - max-slope limit (the uphill velocity component is removed on
 *     too-steep faces, so the capsule can't drive up them),
 *   - step-up assist (low blocker ahead + clearance at step height →
 *     teleport up the step, momentum preserved). */
void jce_bullet_character_move(JceBulletWorld *bw, uint32_t idx,
                                jce_vec3 walk_dir, float dt)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    if (dt <= 0.0f) dt = 1.0f / 60.0f;

    JceBulletCharFeel *f = &bw->char_feel[idx];
    auto *cap = static_cast<btCapsuleShape *>(bw->char_shapes[idx]);
    btScalar half   = cap->getHalfHeight() + cap->getRadius();
    btScalar radius = cap->getRadius();

    bool was_grounded = f->probe_valid && f->grounded;
    char_ground_probe(bw, idx);

    btVector3 v = b->getLinearVelocity();
    if (f->grounded && v.y() <= btScalar(0.5)) f->jumping = false;

    /* Ground snap: just walked off a step/slope crest (not a jump, not
     * rising) and the ground is within step_height below the feet →
     * glue the capsule back down instead of arcing off. */
    if (was_grounded && !f->grounded && !f->jumping &&
        v.y() <= btScalar(0.1)) {
        btVector3 c = b->getWorldTransform().getOrigin();
        btVector3 feet = c - btVector3(0, half, 0);
        btVector3 hit, n;
        if (char_ray_down(bw, b, feet, btScalar(f->step_height) + btScalar(0.05),
                          &hit, &n) &&
            n.y() >= btScalar(f->max_slope_cos)) {
            btScalar drop = feet.y() - hit.y();
            if (drop > btScalar(0.0)) {
                btTransform xf = b->getWorldTransform();
                xf.setOrigin(c - btVector3(0, drop - btScalar(0.01), 0));
                b->setWorldTransform(xf);
                b->setInterpolationWorldTransform(xf);
                v.setY(0);
                f->grounded      = true;
                f->ground_normal = n;
            }
        }
    }

    /* Max-slope limit: in contact with a too-steep face — strip the uphill
     * component of the commanded velocity (along/downhill still allowed),
     * so a frictionless capsule cannot power up a cliff face.  Uses the
     * raw `touching` contact (steep faces deliberately don't count as
     * `grounded` for jumps/animation). */
    btVector3 target(static_cast<btScalar>(walk_dir.x), 0,
                     static_cast<btScalar>(walk_dir.z));
    if (f->touching && f->ground_normal.y() < btScalar(f->max_slope_cos)) {
        btVector3 uphill(-f->ground_normal.x(), 0, -f->ground_normal.z());
        btScalar ul = uphill.length();
        if (ul > btScalar(1e-4)) {
            uphill /= ul;
            btScalar into = target.dot(uphill);
            if (into > btScalar(0.0)) target -= uphill * into;
        }
    }

    /* Accelerate the horizontal velocity toward the target. */
    btScalar rate   = btScalar(f->grounded ? f->accel
                                           : f->accel * f->air_control);
    btScalar max_dv = rate * btScalar(dt);
    btVector3 dv(target.x() - v.x(), 0, target.z() - v.z());
    btScalar  dl = dv.length();
    if (dl > max_dv && dl > SIMD_EPSILON) dv *= max_dv / dl;
    v.setX(v.x() + dv.x());
    v.setZ(v.z() + dv.z());
    b->setLinearVelocity(v);
    b->activate();

    /* Step-up assist: pushing into a low blocker while grounded. */
    btVector3 dir = target;
    btScalar  sp  = dir.length();
    if (f->grounded && sp > btScalar(0.1)) {
        dir /= sp;
        btVector3 c    = b->getWorldTransform().getOrigin();
        btScalar  feet = c.y() - half;
        btScalar  step = btScalar(f->step_height);

        auto fwd_hit = [&](btScalar lift_y, btScalar reach,
                           btVector3 *n_out) -> bool {
            btVector3 from(c.x(), feet + lift_y, c.z());
            btVector3 to = from + dir * reach;
            btCollisionWorld::ClosestRayResultCallback cb(from, to);
            cb.m_collisionFilterGroup = btBroadphaseProxy::CharacterFilter;
            cb.m_collisionFilterMask  = btBroadphaseProxy::StaticFilter |
                                        btBroadphaseProxy::DefaultFilter;
            bw->world->rayTest(from, to, cb);
            if (!cb.hasHit() || cb.m_collisionObject == b) return false;
            if (n_out) *n_out = cb.m_hitNormalWorld;
            return true;
        };

        /* Blocked at ankle height by a RISER (a face too steep to walk —
         * a walkable ramp ahead also intersects the ankle ray, but that is
         * the slope/solver's job, not a step) and clear at step height? */
        btVector3 ankle_n(0, 1, 0);
        if (fwd_hit(btScalar(0.05), radius + btScalar(0.12), &ankle_n) &&
            ankle_n.y() < btScalar(f->max_slope_cos) &&
            !fwd_hit(step + btScalar(0.05), radius + btScalar(0.15), nullptr)) {
            /* Find the step's top surface just past the blocker. */
            btVector3 top_from = btVector3(c.x(), feet + step + btScalar(0.05),
                                           c.z()) + dir * (radius + btScalar(0.15));
            btVector3 hit, n;
            if (char_ray_down(bw, b, top_from, step + btScalar(0.10), &hit, &n) &&
                n.y() >= btScalar(f->max_slope_cos)) {
                btScalar lift = hit.y() - feet;
                if (lift > btScalar(0.02) && lift <= step + btScalar(0.01)) {
                    /* Head clearance: test ABOVE the capsule top (a ray from
                     * the center would lie inside our own volume and always
                     * report clear). */
                    btVector3 head_from = c + btVector3(0, half, 0);
                    btVector3 head_to   = head_from +
                                          btVector3(0, lift + btScalar(0.05), 0);
                    btCollisionWorld::ClosestRayResultCallback hc(head_from, head_to);
                    hc.m_collisionFilterGroup = btBroadphaseProxy::CharacterFilter;
                    hc.m_collisionFilterMask  = btBroadphaseProxy::StaticFilter |
                                                btBroadphaseProxy::DefaultFilter;
                    bw->world->rayTest(head_from, head_to, hc);
                    if (!hc.hasHit() || hc.m_collisionObject == b) {
                        btTransform xf = b->getWorldTransform();
                        xf.setOrigin(c + btVector3(0, lift + btScalar(0.02), 0)
                                       + dir * btScalar(0.02));
                        b->setWorldTransform(xf);
                        b->setInterpolationWorldTransform(xf);
                        btVector3 vv = b->getLinearVelocity();
                        if (vv.y() < btScalar(0.0)) {
                            vv.setY(0);
                            b->setLinearVelocity(vv);
                        }
                    }
                }
            }
        }
    }
}

/* Launch the jump.  Grounded/coyote gating is the RUNTIME's job (it has
 * the timers); here we only refuse re-triggering mid-ascent.  Returns
 * whether the jump actually fired so the caller doesn't consume buffers
 * or pulse animation triggers on a refusal. */
bool jce_bullet_character_jump(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return false;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return false;
    JceBulletCharFeel *f = &bw->char_feel[idx];
    if (f->jumping) return false;   /* already mid-jump */
    btVector3 v = b->getLinearVelocity();
    v.setY(static_cast<btScalar>(bw->char_jump[idx]));
    b->setLinearVelocity(v);
    b->activate();
    f->jumping  = true;
    f->grounded = false;
    return true;
}

void jce_bullet_character_get_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *pos)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx] || !pos) return;
    if (!bw->char_bodies[idx]) return;
    *pos = from_bt_v3(bw->char_bodies[idx]->getWorldTransform().getOrigin());
}

/* Teleport the capsule CENTER to `pos` (clears momentum). */
void jce_bullet_character_set_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 pos)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    btTransform xf = b->getWorldTransform();
    xf.setOrigin(to_bt(pos));
    b->setWorldTransform(xf);
    b->setLinearVelocity(btVector3(0, 0, 0));
    b->setInterpolationWorldTransform(xf);
    b->setInterpolationLinearVelocity(btVector3(0, 0, 0));
    b->activate();
    /* Teleport invalidates the cached ground state and any in-flight jump. */
    bw->char_feel[idx].probe_valid = false;
    bw->char_feel[idx].jumping     = false;
}

bool jce_bullet_character_is_grounded(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return false;
    if (!bw->char_bodies[idx]) return false;
    JceBulletCharFeel *f = &bw->char_feel[idx];
    /* move() refreshes the probe every fixed tick; fall back to a fresh
     * probe only for queries before the first move (e.g. spawn frame). */
    if (!f->probe_valid) return char_ground_probe(bw, idx);
    return f->grounded;
}

void jce_bullet_character_get_velocity(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *out_vel)
{
    if (!out_vel) return;
    *out_vel = jce_v3(0.0f, 0.0f, 0.0f);
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    *out_vel = from_bt_v3(b->getLinearVelocity());
}

void jce_bullet_character_cut_jump(JceBulletWorld *bw, uint32_t idx,
                                    float factor)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    btRigidBody *b = bw->char_bodies[idx];
    if (!b) return;
    if (factor < 0.0f) factor = 0.0f;
    if (factor > 1.0f) factor = 1.0f;
    btVector3 v = b->getLinearVelocity();
    if (v.y() > btScalar(0.0)) {
        v.setY(v.y() * btScalar(factor));
        b->setLinearVelocity(v);
    }
}

/* ================================================================== */
/* Vehicle controller                                                  */
/* ================================================================== */

uint32_t jce_bullet_vehicle_create(JceBulletWorld *bw,
                                    jce_vec3 pos, jce_quat rot,
                                    jce_vec3 chassis_half_ext,
                                    float chassis_mass,
                                    float max_engine_force,
                                    float max_brake_force,
                                    float max_steering_rad,
                                    uint32_t col_group, uint32_t col_mask)
{
    if (!bw) return UINT32_MAX;

    uint32_t idx = UINT32_MAX;
    for (uint32_t n = 0; n < bw->vehicle_capacity; ++n) {
        uint32_t i = (bw->vehicle_alloc_cursor + n) % bw->vehicle_capacity;
        if (!bw->vehicle_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;
    bw->vehicle_alloc_cursor = (idx + 1u) % bw->vehicle_capacity;

    /* Chassis collision shape (box). */
    btCollisionShape *chassis_shape = new btBoxShape(to_bt(chassis_half_ext));
    btVector3 inertia(0, 0, 0);
    if (chassis_mass > 0.0f) chassis_shape->calculateLocalInertia(chassis_mass, inertia);

    btTransform xf;
    xf.setIdentity();
    xf.setOrigin(to_bt(pos));
    xf.setRotation(to_bt_q(rot));
    auto *motion = new btDefaultMotionState(xf);
    btRigidBody::btRigidBodyConstructionInfo ci(chassis_mass, motion, chassis_shape, inertia);
    auto *chassis = new btRigidBody(ci);

    /* Chassis must never sleep — wheels rely on continuous integration. */
    chassis->setActivationState(DISABLE_DEACTIVATION);
    chassis->setUserPointer(reinterpret_cast<void *>(static_cast<uintptr_t>(idx)));
    bw->world->addRigidBody(chassis,
        static_cast<int>(col_group), static_cast<int>(col_mask));

    /* Raycaster + vehicle. */
    auto *raycaster = new btDefaultVehicleRaycaster(bw->world);
    btRaycastVehicle::btVehicleTuning tuning;
    auto *vehicle = new btRaycastVehicle(tuning, chassis, raycaster);

    /* Bullet vehicle convention: forward = Z (axis index 2), up = Y (1), right = X (0). */
    vehicle->setCoordinateSystem(0, 1, 2);

    bw->world->addVehicle(vehicle);

    bw->vehicles[idx]                = vehicle;
    bw->vehicle_raycasters[idx]      = raycaster;
    bw->vehicle_chassis[idx]         = chassis;
    bw->vehicle_chassis_shapes[idx]  = chassis_shape;
    bw->vehicle_alive[idx]           = true;
    bw->vehicle_max_engine[idx]      = max_engine_force;
    bw->vehicle_max_brake[idx]       = max_brake_force;
    bw->vehicle_max_steer[idx]       = max_steering_rad;
    bw->vehicle_count++;
    return idx;
}

void jce_bullet_vehicle_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    if (bw->vehicles[idx]) {
        bw->world->removeVehicle(bw->vehicles[idx]);
        delete bw->vehicles[idx];
        bw->vehicles[idx] = nullptr;
    }
    delete bw->vehicle_raycasters[idx];
    bw->vehicle_raycasters[idx] = nullptr;
    if (bw->vehicle_chassis[idx]) {
        bw->world->removeRigidBody(bw->vehicle_chassis[idx]);
        delete bw->vehicle_chassis[idx]->getMotionState();
        delete bw->vehicle_chassis[idx];
        bw->vehicle_chassis[idx] = nullptr;
    }
    delete bw->vehicle_chassis_shapes[idx];
    bw->vehicle_chassis_shapes[idx] = nullptr;
    bw->vehicle_alive[idx] = false;
    bw->vehicle_count--;
}

uint32_t jce_bullet_vehicle_add_wheel(JceBulletWorld *bw, uint32_t idx,
                                       jce_vec3 connection,
                                       jce_vec3 wheel_dir,
                                       jce_vec3 wheel_axle,
                                       float suspension_rest_len,
                                       float wheel_radius,
                                       bool is_front,
                                       float susp_stiffness,
                                       float susp_damping,
                                       float susp_compression,
                                       float friction_slip,
                                       float roll_influence)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx])
        return UINT32_MAX;
    btRaycastVehicle *vehicle = bw->vehicles[idx];
    btRaycastVehicle::btVehicleTuning tuning;
    btWheelInfo &wi = vehicle->addWheel(to_bt(connection), to_bt(wheel_dir),
                                          to_bt(wheel_axle),
                                          static_cast<btScalar>(suspension_rest_len),
                                          static_cast<btScalar>(wheel_radius),
                                          tuning, is_front);
    if (susp_stiffness   > 0) wi.m_suspensionStiffness   = susp_stiffness;
    if (susp_damping     > 0) wi.m_wheelsDampingRelaxation = susp_damping;
    if (susp_compression > 0) wi.m_wheelsDampingCompression = susp_compression;
    if (friction_slip    > 0) wi.m_frictionSlip           = friction_slip;
    wi.m_rollInfluence   = roll_influence;
    return static_cast<uint32_t>(vehicle->getNumWheels()) - 1u;
}

void jce_bullet_vehicle_set_input(JceBulletWorld *bw, uint32_t idx,
                                   float throttle, float brake, float steer)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btRaycastVehicle *vehicle = bw->vehicles[idx];

    /* Clamp inputs. */
    if (throttle >  1.0f) throttle =  1.0f;
    if (throttle < -1.0f) throttle = -1.0f;
    if (brake    <  0.0f) brake    =  0.0f;
    if (brake    >  1.0f) brake    =  1.0f;
    if (steer    >  1.0f) steer    =  1.0f;
    if (steer    < -1.0f) steer    = -1.0f;

    float engine_force = throttle * bw->vehicle_max_engine[idx];
    float brake_force  = brake    * bw->vehicle_max_brake[idx];
    float steer_rad    = steer    * bw->vehicle_max_steer[idx];

    /* Wake the chassis whenever the user is driving. */
    if (bw->vehicle_chassis[idx])
        bw->vehicle_chassis[idx]->activate(true);

    int n = vehicle->getNumWheels();
    for (int i = 0; i < n; ++i) {
        const btWheelInfo &wi = vehicle->getWheelInfo(i);
        /* Drive: rear-wheel-drive on non-steering wheels, brake everywhere,
         * steer only on front wheels.  Sane GTA-style default. */
        if (wi.m_bIsFrontWheel) {
            vehicle->applyEngineForce(0.0f, i);
            vehicle->setSteeringValue(steer_rad, i);
        } else {
            vehicle->applyEngineForce(engine_force, i);
            vehicle->setSteeringValue(0.0f, i);
        }
        vehicle->setBrake(brake_force, i);
    }
}

void jce_bullet_vehicle_get_chassis_transform(JceBulletWorld *bw, uint32_t idx,
                                                jce_vec3 *pos, jce_quat *rot)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btTransform xf;
    bw->vehicle_chassis[idx]->getMotionState()->getWorldTransform(xf);
    if (pos) *pos = from_bt_v3(xf.getOrigin());
    if (rot) *rot = from_bt_q(xf.getRotation());
}

void jce_bullet_vehicle_get_wheel_transform(JceBulletWorld *bw, uint32_t idx,
                                              uint32_t wheel,
                                              jce_vec3 *pos, jce_quat *rot)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return;
    btRaycastVehicle *vehicle = bw->vehicles[idx];
    if (static_cast<int>(wheel) >= vehicle->getNumWheels()) return;
    /* Update interpolated wheel transform from current suspension state. */
    vehicle->updateWheelTransform(static_cast<int>(wheel), true);
    const btTransform &xf = vehicle->getWheelInfo(wheel).m_worldTransform;
    if (pos) *pos = from_bt_v3(xf.getOrigin());
    if (rot) *rot = from_bt_q(xf.getRotation());
}

float jce_bullet_vehicle_get_speed(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->vehicle_capacity || !bw->vehicle_alive[idx]) return 0.0f;
    /* Bullet returns km/h — convert to m/s for SI consistency. */
    return static_cast<float>(bw->vehicles[idx]->getCurrentSpeedKmHour()) * (1.0f / 3.6f);
}

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
