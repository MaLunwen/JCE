/*
 * jce_physics_bullet.cpp  C++ Bullet3 back-end for jce_physics.
 *
 * Wraps btDiscreteDynamicsWorld behind the extern "C" bridge declared
 * in jce_physics_internal.h.  Compiled as C++17.
 */

#include "jce_physics_internal.h"

#include <btBulletDynamicsCommon.h>
#include <BulletDynamics/Character/btKinematicCharacterController.h>
#include <BulletCollision/CollisionDispatch/btGhostObject.h>

#include "os/core/jce_memory.h"

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

    /* Constraint pool. */
    btTypedConstraint **constraints;
    bool               *con_alive;
    uint32_t            con_capacity;
    uint32_t            con_count;

    /* Character controller pool. */
    btKinematicCharacterController **characters;
    btPairCachingGhostObject       **ghosts;
    btConvexShape                  **char_shapes;
    bool                            *char_alive;
    uint32_t                         char_capacity;
    uint32_t                         char_count;

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

    /* Register ghost pair callback for character controllers. */
    bw->broadphase->getOverlappingPairCache()->setInternalGhostPairCallback(
        new btGhostPairCallback());

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

    /* Allocate constraint pool. */
    bw->con_capacity = max_bodies / 2;
    if (bw->con_capacity < 64) bw->con_capacity = 64;
    bw->con_count = 0;
    bw->constraints = static_cast<btTypedConstraint **>(
        JCE_CALLOC(bw->con_capacity, sizeof(btTypedConstraint *)));
    bw->con_alive = static_cast<bool *>(
        JCE_CALLOC(bw->con_capacity, sizeof(bool)));

    /* Allocate character controller pool. */
    bw->char_capacity = 32;
    bw->char_count = 0;
    bw->characters = static_cast<btKinematicCharacterController **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btKinematicCharacterController *)));
    bw->ghosts = static_cast<btPairCachingGhostObject **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btPairCachingGhostObject *)));
    bw->char_shapes = static_cast<btConvexShape **>(
        JCE_CALLOC(bw->char_capacity, sizeof(btConvexShape *)));
    bw->char_alive = static_cast<bool *>(
        JCE_CALLOC(bw->char_capacity, sizeof(bool)));

    return bw;
}

void jce_bullet_destroy(JceBulletWorld *bw)
{
    if (!bw) return;

    /* Remove and delete all live character controllers. */
    if (bw->characters && bw->ghosts && bw->char_shapes && bw->char_alive) {
        for (uint32_t i = 0; i < bw->char_capacity; ++i) {
            if (!bw->char_alive[i]) continue;
            if (bw->characters[i]) {
                bw->world->removeAction(bw->characters[i]);
                delete bw->characters[i];
            }
            if (bw->ghosts[i]) {
                bw->world->removeCollisionObject(bw->ghosts[i]);
                delete bw->ghosts[i];
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
            bw->alive[i] = false;
        }
    }

    /* Tear down Bullet pipeline in reverse order. */
    delete bw->world;
    delete bw->solver;
    delete bw->broadphase;
    delete bw->dispatcher;
    delete bw->config;

    JCE_FREE(bw->char_alive);
    JCE_FREE(bw->char_shapes);
    JCE_FREE(bw->ghosts);
    JCE_FREE(bw->characters);
    JCE_FREE(bw->con_alive);
    JCE_FREE(bw->constraints);
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
                                float lin_damp, float ang_damp,
                                uint16_t col_group, uint16_t col_mask,
                                bool is_trigger)
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

    /* Trigger bodies: no contact response (overlap only). */
    if (is_trigger) {
        body->setCollisionFlags(
            body->getCollisionFlags() |
            btCollisionObject::CF_NO_CONTACT_RESPONSE);
    }

    /* Store pool index in the user-pointer for contact-callback lookup. */
    body->setUserPointer(reinterpret_cast<void *>(
        static_cast<uintptr_t>(idx)));

    /* Add to world with collision group/mask. */
    bw->world->addRigidBody(body,
                             static_cast<int>(col_group),
                             static_cast<int>(col_mask));
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

/* ================================================================== */
/* Collision filter                                                    */
/* ================================================================== */

void jce_bullet_body_set_collision_filter(JceBulletWorld *bw, uint32_t idx,
                                          uint16_t group, uint16_t mask)
{
    if (!bw || idx >= bw->capacity || !bw->alive[idx]) return;

    btRigidBody *body = bw->bodies[idx];
    if (!body) return;

    /* Must remove and re-add to change filter group/mask. */
    bw->world->removeRigidBody(body);
    bw->world->addRigidBody(body,
                             static_cast<int>(group),
                             static_cast<int>(mask));
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
    if (body_a >= bw->capacity || !bw->alive[body_a]) return UINT32_MAX;

    /* Find a free constraint slot. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < bw->con_capacity; ++i) {
        if (!bw->con_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;

    btRigidBody *rb_a = bw->bodies[body_a];
    btRigidBody *rb_b = nullptr;
    bool has_b = (body_b < bw->capacity && bw->alive[body_b]);
    if (has_b) rb_b = bw->bodies[body_b];

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
/* Character controller                                                */
/* ================================================================== */

uint32_t jce_bullet_character_create(JceBulletWorld *bw,
                                      jce_vec3 pos, float radius,
                                      float height, float step_height,
                                      float max_slope_rad,
                                      float gravity, float jump_speed)
{
    if (!bw) return UINT32_MAX;

    /* Find a free slot. */
    uint32_t idx = UINT32_MAX;
    for (uint32_t i = 0; i < bw->char_capacity; ++i) {
        if (!bw->char_alive[i]) { idx = i; break; }
    }
    if (idx == UINT32_MAX) return UINT32_MAX;

    /* Capsule shape: total height = capsule_height + 2*radius. */
    float capsule_height = height - 2.0f * radius;
    if (capsule_height < 0.01f) capsule_height = 0.01f;

    auto *cap_shape = new btCapsuleShape(
        static_cast<btScalar>(radius),
        static_cast<btScalar>(capsule_height));

    auto *ghost = new btPairCachingGhostObject();
    btTransform start_xf;
    start_xf.setIdentity();
    start_xf.setOrigin(to_bt(pos));
    ghost->setWorldTransform(start_xf);
    ghost->setCollisionShape(cap_shape);
    ghost->setCollisionFlags(btCollisionObject::CF_CHARACTER_OBJECT);

    auto *controller = new btKinematicCharacterController(
        ghost, cap_shape, static_cast<btScalar>(step_height));

    controller->setGravity(btVector3(0, -static_cast<btScalar>(gravity), 0));
    controller->setJumpSpeed(static_cast<btScalar>(jump_speed));
    controller->setMaxSlope(static_cast<btScalar>(max_slope_rad));

    bw->world->addCollisionObject(ghost,
                                   btBroadphaseProxy::CharacterFilter,
                                   btBroadphaseProxy::StaticFilter |
                                   btBroadphaseProxy::DefaultFilter);
    bw->world->addAction(controller);

    bw->characters[idx] = controller;
    bw->ghosts[idx] = ghost;
    bw->char_shapes[idx] = cap_shape;
    bw->char_alive[idx] = true;
    bw->char_count++;

    return idx;
}

void jce_bullet_character_destroy(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;

    if (bw->characters[idx]) {
        bw->world->removeAction(bw->characters[idx]);
        delete bw->characters[idx];
        bw->characters[idx] = nullptr;
    }
    if (bw->ghosts[idx]) {
        bw->world->removeCollisionObject(bw->ghosts[idx]);
        delete bw->ghosts[idx];
        bw->ghosts[idx] = nullptr;
    }
    delete bw->char_shapes[idx];
    bw->char_shapes[idx] = nullptr;
    bw->char_alive[idx] = false;
    bw->char_count--;
}

void jce_bullet_character_move(JceBulletWorld *bw, uint32_t idx,
                                jce_vec3 walk_dir, float dt)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    bw->characters[idx]->setWalkDirection(
        to_bt(walk_dir) * static_cast<btScalar>(dt));
}

void jce_bullet_character_jump(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return;
    bw->characters[idx]->jump();
}

void jce_bullet_character_get_position(JceBulletWorld *bw, uint32_t idx,
                                        jce_vec3 *pos)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx] || !pos) return;
    btTransform xf = bw->ghosts[idx]->getWorldTransform();
    *pos = from_bt_v3(xf.getOrigin());
}

bool jce_bullet_character_is_grounded(JceBulletWorld *bw, uint32_t idx)
{
    if (!bw || idx >= bw->char_capacity || !bw->char_alive[idx]) return false;
    return bw->characters[idx]->onGround();
}
