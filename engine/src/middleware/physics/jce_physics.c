/*
 * jce_physics.c  3D physics implementation (Bullet3 backend).
 *
 * Pure C99 front-end that delegates all simulation work to the C++
 * Bullet3 shim via the jce_bullet_* functions declared in
 * jce_physics_internal.h.
 */

#include <jce/middleware/physics/jce_physics.h>
#include <jce/middleware/physics/jce_physics_debug.h>
#include <jce/middleware/physics/jce_physics_material.h>
#include <jce/middleware/physics/jce_cloth.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_alloc.h>
#include <jce/os/core/jce_math.h>
#include <jce/os/core/jce_profiler.h>
#include <jce/renderer/jce_render_pipeline.h>

#include "jce_physics_internal.h"
#include "os/core/jce_memory.h"

#include <string.h>
#include <stdlib.h>

#define LOG_TAG "physics"

/* ── World struct ──────────────────────────────────────────────────── */

struct JcePhysicsWorld {
    JceBulletWorld *bullet;

    /* Cached config. */
    jce_vec3  gravity;
    float     fixed_timestep;
    int32_t   max_sub_steps;
    uint32_t  body_capacity;

    /* Public-facing contact callbacks.  The bullet layer uses its own
       simpler signature; we adapt between the two in the trampoline. */
    jce_contact_fn contact_begin_fn;
    void          *contact_begin_ud;
    jce_contact_fn contact_end_fn;
    void          *contact_end_ud;

    /* ── P3-C.5: opaque per-body entity tags (parallel to bullet pool). */
    uint64_t      *body_entity;

    /* ── P3-C.5: BEGIN/STAY/END listeners. */
    struct {
        jce_contact_listener_fn fn;
        void                   *ud;
    } listeners[JCE_PHYSICS_MAX_LISTENERS];
    uint32_t       listener_count;

    /* ── P3-C.5: manifold pair diff state.
     *
     * Pairs are encoded as uint64 with the smaller body index in the
     * high 32 bits.  Two arrays alternate as "current" and "previous"
     * frame so we never re-allocate per step.  Each slot also caches
     * the deepest contact for the pair so STAY events carry useful
     * data instead of stale geometry. */
    struct PairRecord {
        uint64_t key;
        float    normal[3];
        float    point[3];
        float    depth;
        uint8_t  is_trigger;
    } *pairs_a;
    uint32_t pairs_a_count;
    uint32_t pairs_capacity;

    struct PairRecord *pairs_b;
    uint32_t pairs_b_count;

    /* Toggle: which buffer is "current".  Flipped per step. */
    uint8_t use_a;
};

/* ── Contact-callback trampoline ──────────────────────────────────── */

/*
 * The Bullet layer fires jce_bullet_contact_fn with raw indices and
 * float[3] arrays.  This trampoline converts them to JceContactEvent
 * and forwards to the public jce_contact_fn registered by the user.
 */
static void contact_begin_trampoline(uint32_t body_a, uint32_t body_b,
                                     const float normal[3],
                                     const float point[3],
                                     float depth, void *ud)
{
    JcePhysicsWorld *w = (JcePhysicsWorld *)ud;
    if (!w || !w->contact_begin_fn) return;

    JceContactEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.body_a    = (JceBodyHandle){ body_a };
    ev.body_b    = (JceBodyHandle){ body_b };
    ev.normal[0] = normal[0];
    ev.normal[1] = normal[1];
    ev.normal[2] = normal[2];
    ev.point[0]  = point[0];
    ev.point[1]  = point[1];
    ev.point[2]  = point[2];
    ev.depth     = depth;

    w->contact_begin_fn(&ev, w->contact_begin_ud);
}

static void contact_end_trampoline(uint32_t body_a, uint32_t body_b,
                                   const float normal[3],
                                   const float point[3],
                                   float depth, void *ud)
{
    JcePhysicsWorld *w = (JcePhysicsWorld *)ud;
    if (!w || !w->contact_end_fn) return;

    JceContactEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.body_a    = (JceBodyHandle){ body_a };
    ev.body_b    = (JceBodyHandle){ body_b };
    ev.normal[0] = normal[0];
    ev.normal[1] = normal[1];
    ev.normal[2] = normal[2];
    ev.point[0]  = point[0];
    ev.point[1]  = point[1];
    ev.point[2]  = point[2];
    ev.depth     = depth;

    w->contact_end_fn(&ev, w->contact_end_ud);
}

/* ── Helpers ───────────────────────────────────────────────────────── */

static JcePhysicsWorldDesc defaults(void)
{
    JcePhysicsWorldDesc d;
    memset(&d, 0, sizeof(d));
    d.gravity        = jce_v3(0.0f, -9.81f, 0.0f);
    d.max_bodies     = 4096;
    d.fixed_timestep = 1.0f / 60.0f;
    d.max_sub_steps  = 4;
    return d;
}

/* ── Create / Destroy ──────────────────────────────────────────────── */

/* P3-C.4 — RPA observer: routes the cloth feature flag into the cloth
 * subsystem.  Registered once on first jce_physics_create(). */
static void cloth_rpa_observer(const JceRenderPipelineDesc *desc, void *ud)
{
    (void)ud;
    if (!desc) return;
    jce_cloth_set_simulation_enabled(desc->enable_cloth);
}

JcePhysicsWorld *jce_physics_create(const JcePhysicsWorldDesc *desc)
{
    JcePhysicsWorldDesc cfg = desc ? *desc : defaults();
    if (cfg.max_bodies == 0)     cfg.max_bodies     = 4096;
    if (cfg.fixed_timestep <= 0) cfg.fixed_timestep  = 1.0f / 60.0f;
    if (cfg.max_sub_steps <= 0)  cfg.max_sub_steps   = 4;

    JcePhysicsWorld *w = (JcePhysicsWorld *)JCE_CALLOC(1, sizeof(*w));
    if (!w) return NULL;

    w->bullet = jce_bullet_create(cfg.gravity, cfg.max_bodies);
    if (!w->bullet) {
        JCE_FREE(w);
        LOG_ERROR(LOG_TAG, "failed to create Bullet3 world");
        return NULL;
    }

    w->gravity        = cfg.gravity;
    w->fixed_timestep = cfg.fixed_timestep;
    w->max_sub_steps  = cfg.max_sub_steps;
    w->body_capacity  = cfg.max_bodies;

    /* P3-C.5 — per-body entity tags (one slot per pool index). */
    w->body_entity = (uint64_t *)JCE_CALLOC(cfg.max_bodies,
                                            sizeof(uint64_t));

    /* P3-C.5 — pair-diff buffers.  Sized as a fraction of the body
     * pool: a fully populated n*(n-1)/2 set is unrealistic; max_bodies*4
     * gives plenty of headroom for typical scenes while keeping memory
     * bounded.  Clamped to a 256-pair minimum for tiny worlds. */
    uint32_t pair_cap = cfg.max_bodies * 4u;
    if (pair_cap < 256u) pair_cap = 256u;
    w->pairs_capacity = pair_cap;
    w->pairs_a = (struct PairRecord *)JCE_CALLOC(pair_cap,
                                                 sizeof(*w->pairs_a));
    w->pairs_b = (struct PairRecord *)JCE_CALLOC(pair_cap,
                                                 sizeof(*w->pairs_b));
    if (!w->body_entity || !w->pairs_a || !w->pairs_b) {
        JCE_FREE(w->body_entity);
        JCE_FREE(w->pairs_a);
        JCE_FREE(w->pairs_b);
        jce_bullet_destroy(w->bullet);
        JCE_FREE(w);
        LOG_ERROR(LOG_TAG, "out of memory allocating physics state");
        return NULL;
    }

    /* Wire up trampolines so Bullet contacts reach the C callbacks. */
    jce_bullet_set_contact_begin(w->bullet,
                                 contact_begin_trampoline, w);
    jce_bullet_set_contact_end(w->bullet,
                               contact_end_trampoline, w);

    /* P3-C.4 — register as the "default" rigid world for cloth anchoring. */
    jce_physics_set_default_bullet_world_(w->bullet);

    /* P3-C.4 — install the RPA observer (idempotent: re-registering
     * just overwrites the slot; fires once now with the current desc). */
    jce_render_pipeline_set_observer(cloth_rpa_observer, NULL);

    LOG_SUCCESS(LOG_TAG,
                "3D physics world created (Bullet3, capacity=%u)",
                cfg.max_bodies);
    return w;
}

void jce_physics_destroy(JcePhysicsWorld *world)
{
    if (!world) return;
    /* P3-C.4 — clear default-world pointer if it was us; tear down all
     * cloth/soft bodies because they may hold anchors into the rigid
     * pool we're about to delete. */
    if (jce_physics_default_bullet_world_() == world->bullet) {
        jce_cloth_shutdown_();
        jce_physics_set_default_bullet_world_(NULL);
    }
    jce_bullet_destroy(world->bullet);
    JCE_FREE(world->body_entity);
    JCE_FREE(world->pairs_a);
    JCE_FREE(world->pairs_b);
    JCE_FREE(world);
    LOG_INFO(LOG_TAG, "3D physics world destroyed");
}

/* ── Step ──────────────────────────────────────────────────────────── */

/* qsort comparator for PairRecord by key ascending. */
static int pair_cmp(const void *a, const void *b)
{
    uint64_t ka = ((const struct PairRecord *)a)->key;
    uint64_t kb = ((const struct PairRecord *)b)->key;
    return (ka < kb) ? -1 : (ka > kb) ? 1 : 0;
}

/* Bullet pair-enumerate callback — accumulates into world->pairs_*[use_a]. */
static void pair_collect(uint32_t a, uint32_t b,
                         const float normal[3], const float point[3],
                         float depth, bool is_trigger, void *ud)
{
    JcePhysicsWorld *w = (JcePhysicsWorld *)ud;
    struct PairRecord *cur = w->use_a ? w->pairs_a : w->pairs_b;
    uint32_t *count        = w->use_a ? &w->pairs_a_count : &w->pairs_b_count;
    if (*count >= w->pairs_capacity) return;

    uint32_t lo = a < b ? a : b;
    uint32_t hi = a < b ? b : a;

    struct PairRecord *p = &cur[(*count)++];
    p->key  = ((uint64_t)lo << 32) | (uint64_t)hi;
    p->normal[0] = normal[0]; p->normal[1] = normal[1]; p->normal[2] = normal[2];
    p->point[0]  = point[0];  p->point[1]  = point[1];  p->point[2]  = point[2];
    p->depth      = depth;
    p->is_trigger = is_trigger ? 1u : 0u;
}

/* Fire a single typed event to every registered listener. */
static void emit_event(const JcePhysicsWorld *w,
                       const struct PairRecord *p,
                       JceContactEventType type)
{
    if (w->listener_count == 0) return;

    uint32_t lo = (uint32_t)(p->key >> 32);
    uint32_t hi = (uint32_t)(p->key & 0xFFFFFFFFu);

    JceContactEvent ev;
    memset(&ev, 0, sizeof(ev));
    ev.body_a    = (JceBodyHandle){ lo };
    ev.body_b    = (JceBodyHandle){ hi };
    ev.normal[0] = p->normal[0];
    ev.normal[1] = p->normal[1];
    ev.normal[2] = p->normal[2];
    ev.point[0]  = p->point[0];
    ev.point[1]  = p->point[1];
    ev.point[2]  = p->point[2];
    ev.depth     = p->depth;
    ev.is_trigger = p->is_trigger ? true : false;
    ev.type      = (JceContactEventTypeRaw)type;
    ev.entity_a  = (lo < w->body_capacity && w->body_entity)
                       ? w->body_entity[lo] : 0;
    ev.entity_b  = (hi < w->body_capacity && w->body_entity)
                       ? w->body_entity[hi] : 0;

    for (uint32_t i = 0; i < w->listener_count; ++i) {
        w->listeners[i].fn(&ev, w->listeners[i].ud);
    }
}

/* Merge-walk the sorted current vs previous arrays and emit BEGIN /
 * STAY / END events.  Both inputs must be sorted by `key`. */
static void diff_pairs_and_emit(JcePhysicsWorld *w)
{
    const struct PairRecord *cur, *prev;
    uint32_t ncur, nprev;
    if (w->use_a) {
        cur = w->pairs_a; ncur = w->pairs_a_count;
        prev = w->pairs_b; nprev = w->pairs_b_count;
    } else {
        cur = w->pairs_b; ncur = w->pairs_b_count;
        prev = w->pairs_a; nprev = w->pairs_a_count;
    }

    uint32_t i = 0, j = 0;
    while (i < ncur && j < nprev) {
        if (cur[i].key == prev[j].key) {
            emit_event(w, &cur[i], JCE_CONTACT_STAY);
            ++i; ++j;
        } else if (cur[i].key < prev[j].key) {
            emit_event(w, &cur[i], JCE_CONTACT_BEGIN);
            ++i;
        } else {
            emit_event(w, &prev[j], JCE_CONTACT_END);
            ++j;
        }
    }
    for (; i < ncur; ++i)  emit_event(w, &cur[i],  JCE_CONTACT_BEGIN);
    for (; j < nprev; ++j) emit_event(w, &prev[j], JCE_CONTACT_END);
}

void jce_physics_step(JcePhysicsWorld *world, float dt)
{
    if (!world) return;
    JCE_PROFILE_ZONE_N("Physics::Step");
    jce_bullet_step(world->bullet, dt,
                    world->fixed_timestep,
                    world->max_sub_steps);

    /* P3-C.4 — step the cloth/soft-body world.  Internally no-ops when
     * simulation is globally disabled or no soft bodies exist. */
    jce_cloth_step_(dt);

    /* P3-C.5 — collect this frame's manifold pairs, diff against the
     * previous frame, dispatch BEGIN/STAY/END to listeners, then flip
     * the double-buffer for the next step. */
    if (world->listener_count > 0) {
        if (world->use_a) world->pairs_a_count = 0;
        else              world->pairs_b_count = 0;

        jce_bullet_enumerate_pairs(world->bullet, pair_collect, world);

        struct PairRecord *cur = world->use_a ? world->pairs_a : world->pairs_b;
        uint32_t           n   = world->use_a ? world->pairs_a_count
                                              : world->pairs_b_count;
        if (n > 1) qsort(cur, n, sizeof(*cur), pair_cmp);

        diff_pairs_and_emit(world);
        world->use_a = !world->use_a;
    } else {
        /* No listeners — keep state coherent so the first listener
         * doesn't see a synthetic flood of END events later. */
        world->pairs_a_count = 0;
        world->pairs_b_count = 0;
    }

    JCE_PROFILE_ZONE_END;
}

/* ── Body create / destroy ─────────────────────────────────────────── */

JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world,
                                      const JceBodyDesc *desc)
{
    if (!world || !desc) return JCE_BODY_INVALID;

    float friction = desc->friction > 0.0f ? desc->friction : 0.5f;

    uint32_t group = desc->collision_group;
    uint32_t mask  = desc->collision_mask;
    if (group == 0) group = JCE_COLLISION_DEFAULT_GROUP;
    if (mask  == 0) mask  = JCE_COLLISION_ALL_MASK;

    uint32_t idx = jce_bullet_body_create(
        world->bullet,
        (uint8_t)desc->type,
        (uint8_t)desc->shape,
        desc->position, desc->rotation,
        desc->half_extents, desc->mass,
        friction, desc->restitution,
        desc->linear_damping, desc->angular_damping,
        group, mask, desc->is_trigger);

    if (idx == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "body pool exhausted");
        return JCE_BODY_INVALID;
    }

    return (JceBodyHandle){ idx };
}

JceBodyHandle jce_physics_body_create_compound(JcePhysicsWorld           *world,
                                               const JceCompoundBodyDesc *desc)
{
    if (!world || !desc || !desc->children || desc->child_count == 0)
        return JCE_BODY_INVALID;

    float friction = desc->friction > 0.0f ? desc->friction : 0.5f;
    uint32_t group = desc->collision_group ? desc->collision_group
                                           : JCE_COLLISION_DEFAULT_GROUP;
    uint32_t mask  = desc->collision_mask  ? desc->collision_mask
                                           : JCE_COLLISION_ALL_MASK;

    JceBulletColliderChild *bc = (JceBulletColliderChild *)jce_malloc(
        (size_t)desc->child_count * sizeof(JceBulletColliderChild));
    if (!bc) return JCE_BODY_INVALID;

    for (uint32_t i = 0; i < desc->child_count; ++i) {
        const JceColliderChild *s = &desc->children[i];
        bc[i].shape        = (uint8_t)s->shape;
        bc[i].position     = s->position;
        bc[i].rotation     = s->rotation;
        bc[i].half_extents = s->half_extents;
        bc[i].vertices     = s->vertices;
        bc[i].vertex_count = s->vertex_count;
        bc[i].indices      = s->indices;
        bc[i].index_count  = s->index_count;
    }

    uint32_t idx = jce_bullet_body_create_compound(
        world->bullet, (uint8_t)desc->type,
        desc->position, desc->rotation,
        desc->mass, friction, desc->restitution,
        desc->linear_damping, desc->angular_damping,
        group, mask, desc->is_trigger,
        bc, desc->child_count);

    jce_free(bc);

    if (idx == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "compound body create failed");
        return JCE_BODY_INVALID;
    }
    return (JceBodyHandle){ idx };
}

void jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_destroy(world->bullet, body.idx);
    if (world->body_entity && body.idx < world->body_capacity) {
        world->body_entity[body.idx] = 0;
    }
}

/* ── Body state queries ────────────────────────────────────────────── */

void jce_physics_body_get_transform(const JcePhysicsWorld *world,
                                    JceBodyHandle body,
                                    jce_vec3 *out_pos, jce_quat *out_rot)
{
    if (!world || !jce_body_valid(body)) return;
    /* cast away const — Bullet's getMotionState is non-const but
       logically read-only here. */
    jce_bullet_body_get_transform(
        (JceBulletWorld *)world->bullet, body.idx, out_pos, out_rot);
}

void jce_physics_body_set_transform(JcePhysicsWorld *world,
                                    JceBodyHandle body,
                                    jce_vec3 pos, jce_quat rot)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_set_transform(world->bullet, body.idx, pos, rot);
}

jce_vec3 jce_physics_body_get_velocity(const JcePhysicsWorld *world,
                                       JceBodyHandle body)
{
    jce_vec3 v = jce_v3(0, 0, 0);
    if (!world || !jce_body_valid(body)) return v;
    jce_bullet_body_get_velocity(
        (JceBulletWorld *)world->bullet, body.idx, &v);
    return v;
}

void jce_physics_body_set_velocity(JcePhysicsWorld *world,
                                   JceBodyHandle body, jce_vec3 vel)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_set_velocity(world->bullet, body.idx, vel);
}

jce_vec3 jce_physics_body_get_angular_velocity(const JcePhysicsWorld *world,
                                               JceBodyHandle body)
{
    jce_vec3 v = jce_v3(0, 0, 0);
    if (!world || !jce_body_valid(body)) return v;
    jce_bullet_body_get_angular_velocity(
        (JceBulletWorld *)world->bullet, body.idx, &v);
    return v;
}

void jce_physics_body_set_angular_velocity(JcePhysicsWorld *world,
                                           JceBodyHandle body, jce_vec3 vel)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_set_angular_velocity(world->bullet, body.idx, vel);
}

/* ── Forces & impulses ─────────────────────────────────────────────── */

void jce_physics_body_apply_force(JcePhysicsWorld *world,
                                  JceBodyHandle body, jce_vec3 force)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_apply_force(world->bullet, body.idx, force);
}

void jce_physics_body_apply_impulse(JcePhysicsWorld *world,
                                    JceBodyHandle body, jce_vec3 impulse)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_apply_impulse(world->bullet, body.idx, impulse);
}

void jce_physics_body_apply_torque(JcePhysicsWorld *world,
                                   JceBodyHandle body, jce_vec3 torque)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_apply_torque(world->bullet, body.idx, torque);
}

/* ── Ray casting ───────────────────────────────────────────────────── */

JceRaycastResult jce_physics_raycast(const JcePhysicsWorld *world,
                                     jce_vec3 origin, jce_vec3 direction,
                                     float max_distance)
{
    JceRaycastResult result;
    memset(&result, 0, sizeof(result));
    result.body = JCE_BODY_INVALID;

    if (!world) return result;

    JceBulletRayResult br = jce_bullet_raycast(
        (JceBulletWorld *)world->bullet, origin, direction, max_distance);

    result.hit      = br.hit;
    result.point    = br.point;
    result.normal   = br.normal;
    result.distance = br.distance;
    result.body     = (JceBodyHandle){ br.body_idx };

    return result;
}

/* ── Contact callbacks ─────────────────────────────────────────────── */

void jce_physics_set_contact_begin(JcePhysicsWorld *world,
                                   jce_contact_fn fn, void *userdata)
{
    if (!world) return;
    world->contact_begin_fn = fn;
    world->contact_begin_ud = userdata;
}

void jce_physics_set_contact_end(JcePhysicsWorld *world,
                                 jce_contact_fn fn, void *userdata)
{
    if (!world) return;
    world->contact_end_fn = fn;
    world->contact_end_ud = userdata;
}

/* ── Debug ─────────────────────────────────────────────────────────── */

uint32_t jce_physics_body_count(const JcePhysicsWorld *world)
{
    if (!world) return 0;
    return jce_bullet_body_count((JceBulletWorld *)world->bullet);
}

/* ── Collision filter ─────────────────────────────────────────────── */

void jce_physics_body_set_collision_filter(JcePhysicsWorld *world,
                                           JceBodyHandle body,
                                           uint32_t group, uint32_t mask)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_set_collision_filter(world->bullet, body.idx,
                                          group, mask);
}

/* ── Material ─────────────────────────────────────────────────────── */

void jce_physics_body_set_material(JcePhysicsWorld *world,
                                   JceBodyHandle body,
                                   const struct JcePhysicsMaterial *material)
{
    if (!world || !jce_body_valid(body) || !material) return;
    /* v1: feed dynamic_friction + restitution directly to Bullet, which
     * runs its own per-contact combine.  Static friction is recorded in
     * the asset for future use (Bullet has no separate static-friction
     * channel on btRigidBody).  Combine modes are surfaced through
     * jce_physics_material_combine() for advanced users. */
    jce_bullet_body_set_material(world->bullet, body.idx,
                                  material->dynamic_friction,
                                  material->restitution);
}

/* ── Continuous Collision Detection (CCD)  (P3-C.3) ───────────────── */

/*
 * CCD-mode → Bullet mapping.  DISCRETE disables; the three CONTINUOUS
 * modes all enable Bullet's swept-CCD with the same parameters.
 * Distinguishing CONTINUOUS_DYNAMIC / CONTINUOUS_SPECULATIVE matters
 * to the editor (round-trips through scene save/load) but Bullet 3
 * exposes only the single setCcdMotionThreshold / SweptSphereRadius
 * pair.  Aliasing is documented in jce_physics.h.
 */

/* Per-world side-table: remember the user-selected CCD mode so getters
 * round-trip through save/load.  Bullet only stores the threshold +
 * radius, not the enum.  Stored sparsely — DISCRETE = absent. */
struct JceCcdSlot {
    uint32_t   idx;
    JceCcdMode mode;
};

/* Tiny static cache; physics worlds are singletons in practice but we
 * key by world pointer to remain correct if multiple worlds exist. */
#define JCE_CCD_CACHE_CAPACITY 1024
static struct {
    const JcePhysicsWorld *world;
    struct JceCcdSlot      slots[JCE_CCD_CACHE_CAPACITY];
    uint32_t               count;
} s_ccd_cache;

static void ccd_cache_set(const JcePhysicsWorld *world, uint32_t idx,
                          JceCcdMode mode)
{
    if (s_ccd_cache.world != world) {
        s_ccd_cache.world = world;
        s_ccd_cache.count = 0;
    }
    for (uint32_t i = 0; i < s_ccd_cache.count; ++i) {
        if (s_ccd_cache.slots[i].idx == idx) {
            if (mode == JCE_CCD_DISCRETE) {
                /* Compact: swap with last. */
                s_ccd_cache.slots[i] =
                    s_ccd_cache.slots[s_ccd_cache.count - 1];
                s_ccd_cache.count--;
            } else {
                s_ccd_cache.slots[i].mode = mode;
            }
            return;
        }
    }
    if (mode == JCE_CCD_DISCRETE) return;
    if (s_ccd_cache.count >= JCE_CCD_CACHE_CAPACITY) return;
    s_ccd_cache.slots[s_ccd_cache.count].idx  = idx;
    s_ccd_cache.slots[s_ccd_cache.count].mode = mode;
    s_ccd_cache.count++;
}

static JceCcdMode ccd_cache_get(const JcePhysicsWorld *world, uint32_t idx)
{
    if (s_ccd_cache.world != world) return JCE_CCD_DISCRETE;
    for (uint32_t i = 0; i < s_ccd_cache.count; ++i) {
        if (s_ccd_cache.slots[i].idx == idx)
            return s_ccd_cache.slots[i].mode;
    }
    return JCE_CCD_DISCRETE;
}

void jce_physics_body_set_ccd_mode(JcePhysicsWorld *world, JceBodyHandle body,
                                   JceCcdMode mode)
{
    if (!world || !jce_body_valid(body)) return;

    if (mode == JCE_CCD_DISCRETE) {
        jce_bullet_body_set_ccd(world->bullet, body.idx, 0.0f, 0.0f);
    } else {
        /* Apply defaults (auto sphere radius from shape AABB). */
        jce_bullet_body_set_ccd(world->bullet, body.idx,
                                JCE_CCD_DEFAULT_MOTION_THRESHOLD, 0.0f);
    }
    ccd_cache_set(world, body.idx, mode);
}

JceCcdMode jce_physics_body_get_ccd_mode(const JcePhysicsWorld *world,
                                         JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return JCE_CCD_DISCRETE;
    /* Reconcile the cache with the Bullet state: if the threshold has
     * been zeroed externally, demote to DISCRETE. */
    float thr = jce_bullet_body_get_ccd_motion_threshold(world->bullet, body.idx);
    JceCcdMode cached = ccd_cache_get(world, body.idx);
    if (thr <= 0.0f) return JCE_CCD_DISCRETE;
    if (cached == JCE_CCD_DISCRETE) return JCE_CCD_CONTINUOUS;
    return cached;
}

void jce_physics_body_set_ccd_motion_threshold(JcePhysicsWorld *world,
                                               JceBodyHandle body,
                                               float threshold)
{
    if (!world || !jce_body_valid(body)) return;
    float radius = jce_bullet_body_get_ccd_swept_sphere_radius(world->bullet,
                                                                body.idx);
    jce_bullet_body_set_ccd(world->bullet, body.idx, threshold, radius);
}

float jce_physics_body_get_ccd_motion_threshold(const JcePhysicsWorld *world,
                                                JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return 0.0f;
    return jce_bullet_body_get_ccd_motion_threshold(world->bullet, body.idx);
}

void jce_physics_body_set_ccd_swept_sphere_radius(JcePhysicsWorld *world,
                                                  JceBodyHandle body,
                                                  float radius)
{
    if (!world || !jce_body_valid(body)) return;
    float thr = jce_bullet_body_get_ccd_motion_threshold(world->bullet,
                                                          body.idx);
    /* Preserve threshold; only the radius changes.  Pass radius<=0 to
     * trigger auto-derivation from the shape AABB. */
    jce_bullet_body_set_ccd(world->bullet, body.idx, thr, radius);
}

float jce_physics_body_get_ccd_swept_sphere_radius(const JcePhysicsWorld *world,
                                                   JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return 0.0f;
    return jce_bullet_body_get_ccd_swept_sphere_radius(world->bullet, body.idx);
}

/* ── Constraints ──────────────────────────────────────────────────── */

JceConstraintHandle jce_physics_constraint_create(JcePhysicsWorld *world,
                                                   const JceConstraintDesc *desc)
{
    if (!world || !desc) return JCE_CONSTRAINT_INVALID;

    uint32_t idx = jce_bullet_constraint_create(
        world->bullet,
        (uint8_t)desc->type,
        desc->body_a.idx,
        desc->body_b.idx,
        desc->pivot_a, desc->pivot_b,
        desc->axis,
        desc->lower_limit, desc->upper_limit,
        desc->disable_collision);

    if (idx == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "constraint pool exhausted");
        return JCE_CONSTRAINT_INVALID;
    }

    return (JceConstraintHandle){ idx };
}

void jce_physics_constraint_destroy(JcePhysicsWorld *world,
                                     JceConstraintHandle con)
{
    if (!world || !jce_constraint_valid(con)) return;
    jce_bullet_constraint_destroy(world->bullet, con.idx);
}

void jce_physics_constraint_set_limits(JcePhysicsWorld *world,
                                        JceConstraintHandle con,
                                        float lower, float upper)
{
    if (!world || !jce_constraint_valid(con)) return;
    jce_bullet_constraint_set_limits(world->bullet, con.idx, lower, upper);
}

/* Internal accessor used by jce_physics_joint_query.c (P3-C.6). */
JceBulletWorld *jce_physics_world_bullet_(const JcePhysicsWorld *world)
{
    return world ? world->bullet : NULL;
}

/* ── Character controller ─────────────────────────────────────────── */

JceCharacterHandle jce_physics_character_create(JcePhysicsWorld *world,
                                                 const JceCharacterDesc *desc)
{
    if (!world || !desc) return JCE_CHARACTER_INVALID;

    float max_slope_rad = desc->max_slope_deg * JCE_DEG2RAD;
    float gravity = desc->gravity > 0.0f ? desc->gravity : 9.81f;
    float jump_speed = desc->jump_speed > 0.0f ? desc->jump_speed : 6.0f;
    float step_height = desc->step_height > 0.0f ? desc->step_height : 0.35f;
    float radius = desc->radius > 0.0f ? desc->radius : 0.3f;
    float height = desc->height > 0.0f ? desc->height : 1.8f;

    uint32_t idx = jce_bullet_character_create(
        world->bullet,
        desc->position, radius, height, step_height,
        max_slope_rad, gravity, jump_speed);

    if (idx == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "character pool exhausted");
        return JCE_CHARACTER_INVALID;
    }

    return (JceCharacterHandle){ idx };
}

void jce_physics_character_destroy(JcePhysicsWorld *world,
                                    JceCharacterHandle ch)
{
    if (!world || !jce_character_valid(ch)) return;
    jce_bullet_character_destroy(world->bullet, ch.idx);
}

void jce_physics_character_move(JcePhysicsWorld *world,
                                 JceCharacterHandle ch,
                                 jce_vec3 walk_dir, float dt)
{
    if (!world || !jce_character_valid(ch)) return;
    jce_bullet_character_move(world->bullet, ch.idx, walk_dir, dt);
}

void jce_physics_character_jump(JcePhysicsWorld *world,
                                 JceCharacterHandle ch)
{
    if (!world || !jce_character_valid(ch)) return;
    jce_bullet_character_jump(world->bullet, ch.idx);
}

void jce_physics_character_get_position(const JcePhysicsWorld *world,
                                         JceCharacterHandle ch,
                                         jce_vec3 *out_pos)
{
    if (!world || !jce_character_valid(ch)) return;
    jce_bullet_character_get_position(
        (JceBulletWorld *)world->bullet, ch.idx, out_pos);
}

bool jce_physics_character_is_grounded(const JcePhysicsWorld *world,
                                        JceCharacterHandle ch)
{
    if (!world || !jce_character_valid(ch)) return false;
    return jce_bullet_character_is_grounded(
        (JceBulletWorld *)world->bullet, ch.idx);
}

/* ── Vehicle controller ───────────────────────────────────────────── */

JceVehicleHandle jce_physics_vehicle_create(JcePhysicsWorld *world,
                                              const JceVehicleDesc *desc)
{
    if (!world || !desc) return JCE_VEHICLE_INVALID;
    uint32_t group = desc->collision_group ? desc->collision_group
                                            : JCE_COLLISION_DEFAULT_GROUP;
    uint32_t mask  = desc->collision_mask  ? desc->collision_mask
                                            : JCE_COLLISION_ALL_MASK;
    uint32_t idx = jce_bullet_vehicle_create(world->bullet,
                                              desc->position, desc->rotation,
                                              desc->chassis_half_extents,
                                              desc->chassis_mass,
                                              desc->max_engine_force,
                                              desc->max_brake_force,
                                              desc->max_steering_rad,
                                              group, mask);
    return (idx == UINT32_MAX) ? JCE_VEHICLE_INVALID
                                : (JceVehicleHandle){ idx };
}

void jce_physics_vehicle_destroy(JcePhysicsWorld *world, JceVehicleHandle veh)
{
    if (!world || !jce_vehicle_valid(veh)) return;
    jce_bullet_vehicle_destroy(world->bullet, veh.idx);
}

uint32_t jce_physics_vehicle_add_wheel(JcePhysicsWorld *world,
                                        JceVehicleHandle veh,
                                        const JceWheelDesc *w)
{
    if (!world || !jce_vehicle_valid(veh) || !w) return UINT32_MAX;
    return jce_bullet_vehicle_add_wheel(world->bullet, veh.idx,
                                         w->connection_point,
                                         w->wheel_direction,
                                         w->wheel_axle,
                                         w->suspension_rest_len,
                                         w->wheel_radius,
                                         w->is_front_wheel,
                                         w->suspension_stiffness,
                                         w->suspension_damping,
                                         w->suspension_compression,
                                         w->friction_slip,
                                         w->roll_influence);
}

void jce_physics_vehicle_set_input(JcePhysicsWorld *world, JceVehicleHandle veh,
                                    float throttle, float brake, float steer)
{
    if (!world || !jce_vehicle_valid(veh)) return;
    jce_bullet_vehicle_set_input(world->bullet, veh.idx,
                                  throttle, brake, steer);
}

void jce_physics_vehicle_get_chassis_transform(const JcePhysicsWorld *world,
                                                 JceVehicleHandle veh,
                                                 jce_vec3 *out_pos,
                                                 jce_quat *out_rot)
{
    if (!world || !jce_vehicle_valid(veh)) return;
    jce_bullet_vehicle_get_chassis_transform(
        (JceBulletWorld *)world->bullet, veh.idx, out_pos, out_rot);
}

void jce_physics_vehicle_get_wheel_transform(const JcePhysicsWorld *world,
                                               JceVehicleHandle veh,
                                               uint32_t wheel_idx,
                                               jce_vec3 *out_pos,
                                               jce_quat *out_rot)
{
    if (!world || !jce_vehicle_valid(veh)) return;
    jce_bullet_vehicle_get_wheel_transform(
        (JceBulletWorld *)world->bullet, veh.idx, wheel_idx, out_pos, out_rot);
}

float jce_physics_vehicle_get_speed(const JcePhysicsWorld *world,
                                     JceVehicleHandle veh)
{
    if (!world || !jce_vehicle_valid(veh)) return 0.0f;
    return jce_bullet_vehicle_get_speed(
        (JceBulletWorld *)world->bullet, veh.idx);
}

/* ── P3-C.5: per-body entity tags ──────────────────────────────────── */

void jce_physics_body_set_entity(JcePhysicsWorld *world,
                                 JceBodyHandle body, uint64_t entity)
{
    if (!world || !jce_body_valid(body)) return;
    if (body.idx >= world->body_capacity || !world->body_entity) return;
    world->body_entity[body.idx] = entity;
}

uint64_t jce_physics_body_get_entity(const JcePhysicsWorld *world,
                                     JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return 0;
    if (body.idx >= world->body_capacity || !world->body_entity) return 0;
    return world->body_entity[body.idx];
}

/* ── P3-C.5: contact listener subscription ─────────────────────────── */

bool jce_physics_add_contact_listener(JcePhysicsWorld *world,
                                      jce_contact_listener_fn fn,
                                      void *ud)
{
    if (!world || !fn) return false;
    if (world->listener_count >= JCE_PHYSICS_MAX_LISTENERS) return false;

    for (uint32_t i = 0; i < world->listener_count; ++i) {
        if (world->listeners[i].fn == fn && world->listeners[i].ud == ud)
            return false;
    }

    world->listeners[world->listener_count].fn = fn;
    world->listeners[world->listener_count].ud = ud;
    world->listener_count++;
    return true;
}

void jce_physics_remove_contact_listener(JcePhysicsWorld *world,
                                         jce_contact_listener_fn fn,
                                         void *ud)
{
    if (!world || !fn) return;
    for (uint32_t i = 0; i < world->listener_count; ++i) {
        if (world->listeners[i].fn == fn && world->listeners[i].ud == ud) {
            for (uint32_t j = i + 1; j < world->listener_count; ++j)
                world->listeners[j - 1] = world->listeners[j];
            world->listener_count--;
            return;
        }
    }
}

/* ── P3-C.5: debug draw per-world flush ────────────────────────────── */
/*
 * The active line-sink and debug-draw flags live in process-wide
 * statics in jce_physics_debug.c — they are installed once by the
 * editor / game.  This per-world function forwards them to the
 * bullet bridge.
 */

extern uint32_t              jce_physics_debug_state_flags_(void);
extern jce_debug_line_fn     jce_physics_debug_state_sink_fn_(void);
extern void                 *jce_physics_debug_state_sink_ud_(void);

static void debug_line_adapter(float fx, float fy, float fz,
                               float tx, float ty, float tz,
                               uint32_t abgr, void *ud)
{
    (void)ud;
    jce_debug_line_fn fn = jce_physics_debug_state_sink_fn_();
    if (!fn) return;
    jce_vec3 a = jce_v3(fx, fy, fz);
    jce_vec3 b = jce_v3(tx, ty, tz);
    fn(a, b, abgr, jce_physics_debug_state_sink_ud_());
}

void jce_physics_debug_flush(JcePhysicsWorld *world)
{
    if (!world) return;
    uint32_t flags       = jce_physics_debug_state_flags_();
    jce_debug_line_fn fn = jce_physics_debug_state_sink_fn_();
    if (flags == 0 || !fn) return;

    jce_bullet_debug_set_mode(world->bullet, flags);
    jce_bullet_debug_draw(world->bullet, debug_line_adapter, NULL);
}

