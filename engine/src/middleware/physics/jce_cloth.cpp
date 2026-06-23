/*
 * jce_cloth.cpp  Cloth / soft-body backend (Bullet btSoftBody).
 *
 * Mass-spring cloth via btSoftBodyHelpers::CreatePatch on an isolated
 * secondary btSoftRigidDynamicsWorld.  Rigid dynamics live in the
 * existing btDiscreteDynamicsWorld owned by jce_physics_bullet — this
 * file does NOT mutate that world.  See jce_cloth.h for design notes
 * and the rationale for the two-world split.
 *
 * Anchor support: appendAnchor() only records the rigid pointer and
 * applies forces during the soft solver — it does not require the
 * rigid body to live in the same world, so cross-world anchors are
 * safe for v1.  Contact response between rigid bodies and cloth is
 * therefore one-way only (cloth feels rigid pose, rigid is unaffected
 * outside of anchor impulses).  Full two-way coupling is a follow-up
 * that would require upgrading the rigid world to btSoftRigid.
 */

#include "jce_physics_internal.h"
#include "os/core/jce_memory.h"

extern "C" {
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/middleware/physics/jce_softbody.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_profiler.h>
}

#include <BulletSoftBody/btSoftBody.h>
#include <BulletSoftBody/btSoftBodyHelpers.h>
#include <BulletSoftBody/btSoftBodyRigidBodyCollisionConfiguration.h>
#include <BulletSoftBody/btSoftRigidDynamicsWorld.h>
#include <btBulletDynamicsCommon.h>

#include <cstdint>
#include <cstring>

#define LOG_TAG "cloth"

/* ================================================================== */
/* Module-global soft world (lazy-init)                                */
/* ================================================================== */

namespace {

struct ClothSlot {
    btSoftBody *body;
    uint32_t    nodes;
    bool        alive;
};

/* Static collision proxy: a zero-mass btRigidBody added to the SHARED soft
 * world so pressure soft bodies can rest / bounce on it (e.g. the ground). */
struct StaticProxy {
    btRigidBody      *body;
    btCollisionShape *shape;
};

struct SoftCtx {
    btSoftBodyRigidBodyCollisionConfiguration *config;
    btCollisionDispatcher                     *dispatcher;
    btDbvtBroadphase                          *broadphase;
    btSequentialImpulseConstraintSolver       *solver;
    btSoftRigidDynamicsWorld                  *world;
    btSoftBodyWorldInfo                        info;

    ClothSlot *slots;
    uint32_t   slot_capacity;
    uint32_t   slot_count;

    /* Static rigid proxies for soft bodies to collide with (soft-body slice). */
    StaticProxy *statics;
    uint32_t     static_capacity;
    uint32_t     static_count;
};

static SoftCtx  g_ctx          = {};
static bool     g_initialised  = false;
static bool     g_sim_enabled  = false; /* HW gate: default OFF (baseline). */
static uint32_t g_total_nodes  = 0;

constexpr uint32_t SLOT_INITIAL_CAPACITY = 32;

/* Handle encoding: 1-based index into `slots` so 0 == JCE_CLOTH_INVALID. */
static inline JceClothHandle make_handle(uint32_t idx)
{
    return (JceClothHandle)(idx + 1u);
}
static inline uint32_t handle_to_idx(JceClothHandle h)
{
    return (uint32_t)h - 1u;
}
static inline bool handle_valid(JceClothHandle h)
{
    if (h == JCE_CLOTH_INVALID || !g_initialised) return false;
    uint32_t i = handle_to_idx(h);
    return i < g_ctx.slot_capacity && g_ctx.slots[i].alive;
}

static inline btVector3 to_bt(jce_vec3 v) { return btVector3(v.x, v.y, v.z); }

/* Ensure the soft world exists.  No-op if already initialised. */
static bool ensure_world(void)
{
    if (g_initialised) return true;

    g_ctx.config     = new btSoftBodyRigidBodyCollisionConfiguration();
    g_ctx.dispatcher = new btCollisionDispatcher(g_ctx.config);
    g_ctx.broadphase = new btDbvtBroadphase();
    g_ctx.solver     = new btSequentialImpulseConstraintSolver();
    g_ctx.world      = new btSoftRigidDynamicsWorld(
        g_ctx.dispatcher, g_ctx.broadphase, g_ctx.solver, g_ctx.config);
    g_ctx.world->setGravity(btVector3(0.0f, -9.81f, 0.0f));

    g_ctx.info.m_broadphase = g_ctx.broadphase;
    g_ctx.info.m_dispatcher = g_ctx.dispatcher;
    g_ctx.info.m_gravity.setValue(0.0f, -9.81f, 0.0f);
    g_ctx.info.m_sparsesdf.Initialize();

    g_ctx.slot_capacity = SLOT_INITIAL_CAPACITY;
    g_ctx.slot_count    = 0;
    g_ctx.slots = static_cast<ClothSlot *>(
        JCE_CALLOC(g_ctx.slot_capacity, sizeof(ClothSlot)));
    if (!g_ctx.slots) {
        delete g_ctx.world;
        delete g_ctx.solver;
        delete g_ctx.broadphase;
        delete g_ctx.dispatcher;
        delete g_ctx.config;
        memset(&g_ctx, 0, sizeof(g_ctx));
        LOG_ERROR(LOG_TAG, "soft world OOM");
        return false;
    }

    g_initialised = true;
    LOG_INFO(LOG_TAG,
             "soft-body world initialised (secondary btSoftRigidDynamicsWorld)");
    return true;
}

/* Grow slot pool 2x. */
static bool grow_slots(void)
{
    uint32_t new_cap = g_ctx.slot_capacity * 2u;
    ClothSlot *p = static_cast<ClothSlot *>(
        JCE_REALLOC(g_ctx.slots, new_cap * sizeof(ClothSlot)));
    if (!p) return false;
    memset(&p[g_ctx.slot_capacity], 0,
           (new_cap - g_ctx.slot_capacity) * sizeof(ClothSlot));
    g_ctx.slots         = p;
    g_ctx.slot_capacity = new_cap;
    return true;
}

static uint32_t find_free_slot(void)
{
    for (uint32_t i = 0; i < g_ctx.slot_capacity; ++i) {
        if (!g_ctx.slots[i].alive) return i;
    }
    uint32_t old = g_ctx.slot_capacity;
    if (!grow_slots()) return UINT32_MAX;
    return old;
}

} /* namespace */

/* ================================================================== */
/* Public API                                                         */
/* ================================================================== */

extern "C" void jce_cloth_set_simulation_enabled(bool enabled)
{
    g_sim_enabled = enabled;
    LOG_INFO(LOG_TAG, "simulation %s", enabled ? "enabled" : "disabled");
}

extern "C" bool jce_cloth_is_simulation_enabled(void)
{
    return g_sim_enabled;
}

extern "C" uint32_t jce_cloth_active_count(void)
{
    return g_initialised ? g_ctx.slot_count : 0u;
}

extern "C" uint32_t jce_cloth_total_nodes(void)
{
    return g_total_nodes;
}

extern "C" JceClothHandle jce_cloth_create(const JceClothDesc *desc)
{
    if (!desc) return JCE_CLOTH_INVALID;
    if (desc->res_u < 2 || desc->res_v < 2) {
        LOG_WARN(LOG_TAG, "cloth_create: res_u/v must be >=2");
        return JCE_CLOTH_INVALID;
    }
    if (!ensure_world()) return JCE_CLOTH_INVALID;

    uint32_t slot = find_free_slot();
    if (slot == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "cloth_create: cannot allocate slot");
        return JCE_CLOTH_INVALID;
    }

    btSoftBody *sb = btSoftBodyHelpers::CreatePatch(
        g_ctx.info,
        to_bt(desc->corner_00), to_bt(desc->corner_10),
        to_bt(desc->corner_01), to_bt(desc->corner_11),
        (int)desc->res_u, (int)desc->res_v,
        /*fixeds=*/0, /*gendiags=*/true);
    if (!sb) {
        LOG_ERROR(LOG_TAG, "btSoftBodyHelpers::CreatePatch failed");
        return JCE_CLOTH_INVALID;
    }

    /* Material / config. */
    btSoftBody::Material *mat = sb->m_materials[0];
    mat->m_kLST = desc->stiffness_linear;
    mat->m_kAST = desc->stiffness_angular;
    mat->m_kVST = desc->stiffness_linear; /* volume — same as linear for cloth */

    sb->m_cfg.kDP = desc->damping;
    uint32_t iters = desc->iterations > 0 ? desc->iterations : 4u;
    sb->m_cfg.viterations = (int)iters;
    sb->m_cfg.piterations = (int)iters;
    sb->m_cfg.diterations = (int)iters;
    sb->m_cfg.citerations = (int)iters;

    /* Self-collision is expensive (O(n^2)).  Off unless requested. */
    if (desc->self_collision) {
        sb->m_cfg.collisions |= btSoftBody::fCollision::VF_SS;
    }

    /* Wind. */
    if (desc->wind_enabled) {
        sb->m_cfg.kAHR = 0.5f;
        sb->m_cfg.kVCF = 1.0f;
        sb->setWindVelocity(to_bt(desc->wind_velocity));
    }

    /* Mass + pinning. */
    sb->setTotalMass(desc->mass_total > 0.0f ? desc->mass_total : 1.0f,
                     /*fromfaces=*/true);
    int node_count_i = sb->m_nodes.size();
    if (desc->pinned_indices && desc->pinned_count) {
        for (uint32_t k = 0; k < desc->pinned_count; ++k) {
            int ni = (int)desc->pinned_indices[k];
            if (ni >= 0 && ni < node_count_i) {
                sb->setMass(ni, 0.0f);
            }
        }
    }

    g_ctx.world->addSoftBody(sb);

    g_ctx.slots[slot].body  = sb;
    g_ctx.slots[slot].nodes = (uint32_t)node_count_i;
    g_ctx.slots[slot].alive = true;
    ++g_ctx.slot_count;
    g_total_nodes += (uint32_t)node_count_i;

    return make_handle(slot);
}

extern "C" void jce_cloth_destroy(JceClothHandle h)
{
    if (!handle_valid(h)) return;
    uint32_t i = handle_to_idx(h);
    btSoftBody *sb = g_ctx.slots[i].body;
    if (sb) {
        g_ctx.world->removeSoftBody(sb);
        delete sb;
    }
    g_total_nodes -= g_ctx.slots[i].nodes;
    g_ctx.slots[i].body  = nullptr;
    g_ctx.slots[i].nodes = 0;
    g_ctx.slots[i].alive = false;
    --g_ctx.slot_count;
}

extern "C" void jce_cloth_set_wind(JceClothHandle h, jce_vec3 velocity,
                                   bool enabled)
{
    if (!handle_valid(h)) return;
    btSoftBody *sb = g_ctx.slots[handle_to_idx(h)].body;
    if (!sb) return;
    if (enabled) {
        sb->m_cfg.kAHR = 0.5f;
        sb->m_cfg.kVCF = 1.0f;
        sb->setWindVelocity(to_bt(velocity));
    } else {
        sb->m_cfg.kAHR = 0.0f;
        sb->m_cfg.kVCF = 0.0f;
        sb->setWindVelocity(btVector3(0, 0, 0));
    }
}

extern "C" uint32_t jce_cloth_node_count(JceClothHandle h)
{
    if (!handle_valid(h)) return 0;
    return g_ctx.slots[handle_to_idx(h)].nodes;
}

extern "C" bool jce_cloth_get_positions(JceClothHandle h,
                                        float *out_positions,
                                        uint32_t out_capacity_floats)
{
    if (!handle_valid(h) || !out_positions) return false;
    btSoftBody *sb = g_ctx.slots[handle_to_idx(h)].body;
    if (!sb) return false;
    int n = sb->m_nodes.size();
    if ((uint32_t)n * 3u > out_capacity_floats) return false;
    for (int i = 0; i < n; ++i) {
        const btVector3 &p = sb->m_nodes[i].m_x;
        out_positions[i * 3 + 0] = p.x();
        out_positions[i * 3 + 1] = p.y();
        out_positions[i * 3 + 2] = p.z();
    }
    return true;
}

extern "C" bool jce_cloth_anchor_to_body(JceClothHandle cloth,
                                         uint32_t node_index,
                                         JcePhysicsBody body,
                                         jce_vec3 local_pivot,
                                         bool disable_collision)
{
    if (!handle_valid(cloth)) return false;
    btSoftBody *sb = g_ctx.slots[handle_to_idx(cloth)].body;
    if (!sb) return false;
    if ((int)node_index >= sb->m_nodes.size()) return false;

    JceBulletWorld *bw = jce_physics_default_bullet_world_();
    if (!bw) {
        LOG_WARN(LOG_TAG, "anchor_to_body: no default rigid world registered");
        return false;
    }
    void *native = jce_bullet_body_get_rigid_native_(bw, body.idx);
    if (!native) {
        LOG_WARN(LOG_TAG, "anchor_to_body: invalid body handle %u", body.idx);
        return false;
    }
    btRigidBody *rb = static_cast<btRigidBody *>(native);
    sb->appendAnchor((int)node_index, rb, to_bt(local_pivot),
                     disable_collision);
    return true;
}

namespace {

/* Shared world-step body used by BOTH jce_cloth_step_ and jce_softbody_step_
 * (cloth and soft bodies live in the same secondary soft world).  Behaviour is
 * byte-identical to the original jce_cloth_step_ so cloth stepping is unchanged. */
static void soft_world_step(float dt)
{
    if (!g_initialised || !g_sim_enabled || g_ctx.slot_count == 0) return;
    if (dt <= 0.0f) return;
    JCE_PROFILE_ZONE_N("Cloth::Step");
    /* Clamp dt for stability on hitches. */
    if (dt > 1.0f / 30.0f) dt = 1.0f / 30.0f;
    /* maxSubSteps=4 (matches the rigid world default) so cloth keeps real-time
     * pace below 60 fps instead of running in slow motion; the dt clamp above
     * bounds catch-up to avoid a spiral of death. */
    g_ctx.world->stepSimulation(dt, 4, 1.0f / 60.0f);
}

} /* namespace */

extern "C" void jce_cloth_step_(float dt)
{
    soft_world_step(dt);
}

extern "C" void jce_cloth_shutdown_(void)
{
    if (!g_initialised) return;

    if (g_ctx.slots) {
        for (uint32_t i = 0; i < g_ctx.slot_capacity; ++i) {
            if (g_ctx.slots[i].alive && g_ctx.slots[i].body) {
                g_ctx.world->removeSoftBody(g_ctx.slots[i].body);
                delete g_ctx.slots[i].body;
            }
        }
        JCE_FREE(g_ctx.slots);
    }
    /* CRITICAL: free the static rigid proxies (soft-body slice) BEFORE deleting
     * the world below, otherwise the world would be gone when we removeRigidBody.
     * Frees the list itself too; the memset at the end resets the fields. */
    jce_softbody_clear_statics();
    if (g_ctx.statics) {
        JCE_FREE(g_ctx.statics);
    }
    delete g_ctx.world;
    delete g_ctx.solver;
    delete g_ctx.broadphase;
    delete g_ctx.dispatcher;
    delete g_ctx.config;
    memset(&g_ctx, 0, sizeof(g_ctx));
    g_total_nodes = 0;
    g_initialised = false;
    LOG_INFO(LOG_TAG, "soft-body world destroyed");
}

/* ================================================================== */
/* Volumetric / pressure soft body (shares the cloth soft world)      */
/* ================================================================== */

namespace {

constexpr uint32_t STATIC_INITIAL_CAPACITY = 8;

/* Ensure room for one more static proxy.  Grows 2x (or seeds the list). */
static bool ensure_static_capacity(void)
{
    if (g_ctx.static_count < g_ctx.static_capacity) return true;
    uint32_t new_cap = g_ctx.static_capacity ? g_ctx.static_capacity * 2u
                                             : STATIC_INITIAL_CAPACITY;
    StaticProxy *p = static_cast<StaticProxy *>(
        JCE_REALLOC(g_ctx.statics, new_cap * sizeof(StaticProxy)));
    if (!p) return false;
    memset(&p[g_ctx.static_capacity], 0,
           (new_cap - g_ctx.static_capacity) * sizeof(StaticProxy));
    g_ctx.statics         = p;
    g_ctx.static_capacity = new_cap;
    return true;
}

} /* namespace */

extern "C" void jce_softbody_set_simulation_enabled(bool enabled)
{
    /* Same global gate the cloth uses — one shared soft world. */
    jce_cloth_set_simulation_enabled(enabled);
}

extern "C" bool jce_softbody_is_simulation_enabled(void)
{
    return g_sim_enabled;
}

extern "C" void jce_softbody_step_(float dt)
{
    /* Same world step the cloth uses — one shared soft world. */
    soft_world_step(dt);
}

extern "C" uint32_t jce_softbody_add_static_box(jce_vec3 center,
                                                jce_vec3 half_extents)
{
    if (!ensure_world()) return UINT32_MAX;
    if (!ensure_static_capacity()) return UINT32_MAX;

    btBoxShape *shape = new btBoxShape(
        btVector3(half_extents.x, half_extents.y, half_extents.z));

    btTransform xf;
    xf.setIdentity();
    xf.setOrigin(to_bt(center));
    /* Zero mass + no inertia = static collision object. */
    btDefaultMotionState *ms = new btDefaultMotionState(xf);
    btRigidBody::btRigidBodyConstructionInfo ci(
        0.0f, ms, shape, btVector3(0, 0, 0));
    btRigidBody *rb = new btRigidBody(ci);
    rb->setFriction(1.0f);
    g_ctx.world->addRigidBody(rb);

    uint32_t id = g_ctx.static_count;
    g_ctx.statics[id].body  = rb;
    g_ctx.statics[id].shape = shape;
    ++g_ctx.static_count;
    return id;
}

extern "C" void jce_softbody_clear_statics(void)
{
    if (!g_initialised || !g_ctx.statics) return;
    for (uint32_t i = 0; i < g_ctx.static_count; ++i) {
        StaticProxy *sp = &g_ctx.statics[i];
        if (sp->body) {
            if (g_ctx.world) g_ctx.world->removeRigidBody(sp->body);
            delete sp->body->getMotionState();
            delete sp->body;
        }
        delete sp->shape;
        sp->body  = nullptr;
        sp->shape = nullptr;
    }
    g_ctx.static_count = 0;
}

extern "C" JceSoftBodyHandle jce_softbody_create_ellipsoid(
    const JceSoftBodyDesc *desc)
{
    if (!desc) return JCE_SOFTBODY_INVALID;
    if (!ensure_world()) return JCE_SOFTBODY_INVALID;

    int res = desc->resolution > 3 ? desc->resolution : 64;

    uint32_t slot = find_free_slot();
    if (slot == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "softbody_create: cannot allocate slot");
        return JCE_SOFTBODY_INVALID;
    }

    /* CreateEllipsoid builds a CLOSED soft body (a sealed shell). */
    btSoftBody *sb = btSoftBodyHelpers::CreateEllipsoid(
        g_ctx.info,
        to_bt(desc->center),
        btVector3(desc->radius.x, desc->radius.y, desc->radius.z),
        res);
    if (!sb) {
        LOG_ERROR(LOG_TAG, "btSoftBodyHelpers::CreateEllipsoid failed");
        return JCE_SOFTBODY_INVALID;
    }

    /* Material stiffness (per the first material the helper created). */
    btSoftBody::Material *mat = sb->m_materials[0];
    mat->m_kLST = desc->stiffness_linear;
    mat->m_kVST = desc->stiffness_volume;

    /* Pressure (kPR > 0) is what makes it a squishy resisting volume. */
    sb->m_cfg.kPR = desc->pressure;
    sb->m_cfg.kDP = desc->damping;
    sb->m_cfg.kDF = desc->friction;

    /* Collide against rigid bodies via the signed-distance field so the body
     * rests / bounces on the static box proxies (and any rigid in this world). */
    sb->m_cfg.collisions |= btSoftBody::fCollision::SDF_RS;

    /* Optional cluster self-collision (cluster-vs-cluster + cluster-vs-rigid). */
    if (desc->self_collision) {
        sb->m_cfg.collisions |= btSoftBody::fCollision::CL_SS;
        sb->m_cfg.collisions |= btSoftBody::fCollision::CL_RS;
        sb->generateClusters(16);
    }

    sb->setTotalMass(desc->mass > 0.0f ? desc->mass : 1.0f, /*fromfaces=*/true);

    g_ctx.world->addSoftBody(sb);

    int node_count_i = sb->m_nodes.size();
    g_ctx.slots[slot].body  = sb;
    g_ctx.slots[slot].nodes = (uint32_t)node_count_i;
    g_ctx.slots[slot].alive = true;
    ++g_ctx.slot_count;
    g_total_nodes += (uint32_t)node_count_i;

    return make_handle(slot);
}

extern "C" void jce_softbody_destroy(JceSoftBodyHandle h)
{
    /* Shared slot pool with cloth — same teardown path. */
    jce_cloth_destroy((JceClothHandle)h);
}

extern "C" uint32_t jce_softbody_node_count(JceSoftBodyHandle h)
{
    return jce_cloth_node_count((JceClothHandle)h);
}

extern "C" bool jce_softbody_get_positions(JceSoftBodyHandle h,
                                           float *out_xyz,
                                           uint32_t out_capacity_floats)
{
    return jce_cloth_get_positions((JceClothHandle)h, out_xyz,
                                   out_capacity_floats);
}

extern "C" bool jce_softbody_get_center(JceSoftBodyHandle h,
                                        jce_vec3 *out_center)
{
    if (!handle_valid((JceClothHandle)h) || !out_center) return false;
    btSoftBody *sb = g_ctx.slots[handle_to_idx((JceClothHandle)h)].body;
    if (!sb) return false;
    int n = sb->m_nodes.size();
    if (n <= 0) return false;
    btVector3 sum(0, 0, 0);
    for (int i = 0; i < n; ++i) sum += sb->m_nodes[i].m_x;
    btScalar inv = btScalar(1) / btScalar(n);
    out_center->x = sum.x() * inv;
    out_center->y = sum.y() * inv;
    out_center->z = sum.z() * inv;
    return true;
}

extern "C" bool jce_softbody_get_aabb(JceSoftBodyHandle h,
                                      jce_vec3 *out_min,
                                      jce_vec3 *out_max)
{
    if (!handle_valid((JceClothHandle)h) || !out_min || !out_max) return false;
    btSoftBody *sb = g_ctx.slots[handle_to_idx((JceClothHandle)h)].body;
    if (!sb) return false;
    int n = sb->m_nodes.size();
    if (n <= 0) return false;
    btVector3 mn = sb->m_nodes[0].m_x;
    btVector3 mx = mn;
    for (int i = 1; i < n; ++i) {
        mn.setMin(sb->m_nodes[i].m_x);
        mx.setMax(sb->m_nodes[i].m_x);
    }
    out_min->x = mn.x(); out_min->y = mn.y(); out_min->z = mn.z();
    out_max->x = mx.x(); out_max->y = mx.y(); out_max->z = mx.z();
    return true;
}
