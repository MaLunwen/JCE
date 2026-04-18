/*
 * jce_physics.c  3D physics implementation (Bullet3 backend).
 *
 * Pure C99 front-end that delegates all simulation work to the C++
 * Bullet3 shim via the jce_bullet_* functions declared in
 * jce_physics_internal.h.
 */

#include <jce/physics/jce_physics.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"
#include "jce_physics_internal.h"

#include <string.h>

#define LOG_TAG "physics"

/* ── World struct ──────────────────────────────────────────────────── */

struct JcePhysicsWorld {
    JceBulletWorld *bullet;

    /* Cached config. */
    jce_vec3  gravity;
    float     fixed_timestep;
    int32_t   max_sub_steps;

    /* Public-facing contact callbacks.  The bullet layer uses its own
       simpler signature; we adapt between the two in the trampoline. */
    jce_contact_fn contact_begin_fn;
    void          *contact_begin_ud;
    jce_contact_fn contact_end_fn;
    void          *contact_end_ud;
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

    /* Wire up trampolines so Bullet contacts reach the C callbacks. */
    jce_bullet_set_contact_begin(w->bullet,
                                 contact_begin_trampoline, w);
    jce_bullet_set_contact_end(w->bullet,
                               contact_end_trampoline, w);

    LOG_SUCCESS(LOG_TAG,
                "3D physics world created (Bullet3, capacity=%u)",
                cfg.max_bodies);
    return w;
}

void jce_physics_destroy(JcePhysicsWorld *world)
{
    if (!world) return;
    jce_bullet_destroy(world->bullet);
    JCE_FREE(world);
    LOG_INFO(LOG_TAG, "3D physics world destroyed");
}

/* ── Step ──────────────────────────────────────────────────────────── */

void jce_physics_step(JcePhysicsWorld *world, float dt)
{
    if (!world) return;
    jce_bullet_step(world->bullet, dt,
                    world->fixed_timestep,
                    world->max_sub_steps);
}

/* ── Body create / destroy ─────────────────────────────────────────── */

JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world,
                                      const JceBodyDesc *desc)
{
    if (!world || !desc) return JCE_BODY_INVALID;

    float friction = desc->friction > 0.0f ? desc->friction : 0.5f;

    uint16_t group = desc->collision_group;
    uint16_t mask  = desc->collision_mask;
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

void jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_destroy(world->bullet, body.idx);
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
                                           uint16_t group, uint16_t mask)
{
    if (!world || !jce_body_valid(body)) return;
    jce_bullet_body_set_collision_filter(world->bullet, body.idx,
                                          group, mask);
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

/* ── Character controller ─────────────────────────────────────────── */

JceCharacterHandle jce_physics_character_create(JcePhysicsWorld *world,
                                                 const JceCharacterDesc *desc)
{
    if (!world || !desc) return JCE_CHARACTER_INVALID;

    float max_slope_rad = desc->max_slope_deg * 3.14159265f / 180.0f;
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
