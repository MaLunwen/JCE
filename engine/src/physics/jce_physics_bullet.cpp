/*
 * jce_physics_bullet.cpp  C++ Bullet3 back-end for jce_physics.
 *
 * Wraps btDiscreteDynamicsWorld behind the extern "C" bridge declared
 * in jce_physics_internal.h.  Compiled as C++17.
 */

#include "jce_physics_internal.h"

#include <btBulletDynamicsCommon.h>

#include "core/jce_memory.h"

#include <cstring>

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
/* World definition                                                    */
/* ================================================================== */

struct JceBulletWorld {
    /* Bullet pipeline objects (owned, deleted in reverse order). */
    btDefaultCollisionConfiguration  *config;
    btCollisionDispatcher            *dispatcher;
    btDbvtBroadphase                 *broadphase;
    btSequentialImpulseConstraintSolver *solver;
    btDiscreteDynamicsWorld          *world;

    /* Body / shape pool (parallel arrays). */
    btRigidBody      **bodies;
    btCollisionShape  **shapes;
    bool               *alive;
    uint32_t            capacity;
    uint32_t            count;

    /* Contact callbacks forwarded to the C layer. */
    jce_bullet_contact_fn contact_begin_fn;
    void                 *contact_begin_ud;
    jce_bullet_contact_fn contact_end_fn;
    void                 *contact_end_ud;
};

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

JceBulletWorld *jce_bullet_create(jce_vec3 gravity, uint32_t max_bodies)
{
    auto *bw = static_cast<JceBulletWorld *>(
        JCE_CALLOC(1, sizeof(JceBulletWorld)));
    if (!bw) return nullptr;

    bw->config     = new btDefaultCollisionConfiguration();
    bw->dispatcher = new btCollisionDispatcher(bw->config);
    bw->broadphase = new btDbvtBroadphase();
    bw->solver     = new btSequentialImpulseConstraintSolver();
    bw->world      = new btDiscreteDynamicsWorld(
        bw->dispatcher, bw->broadphase, bw->solver, bw->config);

    bw->world->setGravity(to_bt(gravity));

    /* Store back-pointer so the tick callback can reach JceBulletWorld. */
    bw->world->setWorldUserInfo(bw);
    bw->world->setInternalTickCallback(post_tick_callback,
                                       bw, /*isPreTick=*/false);

    /* Allocate body pool. */
    bw->capacity = max_bodies;
    bw->count    = 0;
    bw->bodies   = static_cast<btRigidBody **>(
        JCE_CALLOC(max_bodies, sizeof(btRigidBody *)));
    bw->shapes   = static_cast<btCollisionShape **>(
        JCE_CALLOC(max_bodies, sizeof(btCollisionShape *)));
    bw->alive    = static_cast<bool *>(
        JCE_CALLOC(max_bodies, sizeof(bool)));

    if (!bw->bodies || !bw->shapes || !bw->alive) {
        jce_bullet_destroy(bw);
        return nullptr;
    }

    return bw;
}

void jce_bullet_destroy(JceBulletWorld *bw)
{
    if (!bw) return;

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
            bw->alive[i] = false;
        }
    }

    /* Tear down Bullet pipeline in reverse order. */
    delete bw->world;
    delete bw->solver;
    delete bw->broadphase;
    delete bw->dispatcher;
    delete bw->config;

    JCE_FREE(bw->alive);
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

uint32_t jce_bullet_body_create(JceBulletWorld *bw,
                                uint8_t type, uint8_t shape,
                                jce_vec3 pos, jce_quat rot,
                                jce_vec3 half_ext, float mass,
                                float friction, float restitution,
                                float lin_damp, float ang_damp)
{
    if (!bw) return UINT32_MAX;

    /* Find a free slot. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < bw->capacity; ++i) {
        if (!bw->alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX; /* pool exhausted */

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

    /* Tag kinematic bodies so Bullet treats them correctly. */
    if (static_cast<JceBodyType>(type) == JCE_BODY_KINEMATIC) {
        body->setCollisionFlags(
            body->getCollisionFlags() |
            btCollisionObject::CF_KINEMATIC_OBJECT);
        body->setActivationState(DISABLE_DEACTIVATION);
    }

    /* Store pool index in the user-pointer for contact-callback lookup. */
    body->setUserPointer(reinterpret_cast<void *>(
        static_cast<uintptr_t>(idx)));

    /* Add to world and store in pool. */
    bw->world->addRigidBody(body);
    bw->bodies[idx] = body;
    bw->shapes[idx] = col_shape;
    bw->alive[idx]  = true;
    bw->count++;

    return idx;
}

void jce_bullet_body_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;

    btRigidBody *body = bw->bodies[idx];
    if (body) {
        bw->world->removeRigidBody(body);
        delete body->getMotionState();
        delete body;
    }
    delete bw->shapes[idx];

    bw->bodies[idx] = nullptr;
    bw->shapes[idx] = nullptr;
    bw->alive[idx]  = false;
    bw->count--;
}

/* ================================================================== */
/* Transform                                                           */
/* ================================================================== */

void jce_bullet_body_get_transform(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 *pos, jce_quat *rot)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;

    btTransform xf;
    btRigidBody *body = bw->bodies[idx];
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
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;

    btTransform xf;
    xf.setOrigin(to_bt(pos));
    xf.setRotation(to_bt_q(rot));

    btRigidBody *body = bw->bodies[idx];
    body->setWorldTransform(xf);
    if (body->getMotionState()) {
        body->getMotionState()->setWorldTransform(xf);
    }
    body->activate();
}

/* ================================================================== */
/* Linear velocity                                                     */
/* ================================================================== */

void jce_bullet_body_get_velocity(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 *vel)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx] || !vel) return;
    *vel = from_bt_v3(bw->bodies[idx]->getLinearVelocity());
}

void jce_bullet_body_set_velocity(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 vel)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;
    bw->bodies[idx]->setLinearVelocity(to_bt(vel));
    bw->bodies[idx]->activate();
}

/* ================================================================== */
/* Angular velocity                                                    */
/* ================================================================== */

void jce_bullet_body_get_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 *vel)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx] || !vel) return;
    *vel = from_bt_v3(bw->bodies[idx]->getAngularVelocity());
}

void jce_bullet_body_set_angular_velocity(JceBulletWorld *bw, uint32_t idx,
                                          jce_vec3 vel)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;
    bw->bodies[idx]->setAngularVelocity(to_bt(vel));
    bw->bodies[idx]->activate();
}

/* ================================================================== */
/* Forces & impulses                                                   */
/* ================================================================== */

void jce_bullet_body_apply_force(JceBulletWorld *bw, uint32_t idx,
                                 jce_vec3 force)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;
    bw->bodies[idx]->applyCentralForce(to_bt(force));
    bw->bodies[idx]->activate();
}

void jce_bullet_body_apply_impulse(JceBulletWorld *bw, uint32_t idx,
                                   jce_vec3 impulse)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;
    bw->bodies[idx]->applyCentralImpulse(to_bt(impulse));
    bw->bodies[idx]->activate();
}

void jce_bullet_body_apply_torque(JceBulletWorld *bw, uint32_t idx,
                                  jce_vec3 torque)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;
    bw->bodies[idx]->applyTorque(to_bt(torque));
    bw->bodies[idx]->activate();
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

    btVector3 from = to_bt(origin);
    btVector3 to   = from + to_bt(dir).normalized() *
                     static_cast<btScalar>(max_dist);

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
