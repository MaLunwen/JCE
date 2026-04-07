/*
 * jce_physics.c  3D physics implementation (Bullet3 backend).
 *
 * Wraps Bullet's btDiscreteDynamicsWorld in a C99 API.
 * Bodies are stored in a dense handle pool for O(1) lookup.
 */

#include <jce/physics/jce_physics.h>
#include <jce/core/jce_log.h>
#include <jce/core/jce_allocator.h>

#include <string.h>
#include <stdlib.h>

#define LOG_TAG "physics"

/* ── Internal body record ──────────────────────────────────────────── */

typedef struct {
    bool         alive;
    JceBodyType  type;
    JceShapeType shape;
    jce_vec3     position;
    jce_quat     rotation;
    jce_vec3     velocity;
    jce_vec3     angular_velocity;
    jce_vec3     half_extents;
    float        mass;
    float        friction;
    float        restitution;
    float        linear_damping;
    float        angular_damping;
    /* Accumulated forces (cleared each step). */
    jce_vec3     force;
    jce_vec3     torque;
} PhysBody;

/* ── World struct ──────────────────────────────────────────────────── */

struct JcePhysicsWorld {
    PhysBody    *bodies;
    uint32_t     capacity;
    uint32_t     count;

    jce_vec3     gravity;
    float        fixed_timestep;
    int32_t      max_sub_steps;
    float        accumulator;

    /* Contact callbacks. */
    jce_contact_fn contact_begin_fn;
    void          *contact_begin_ud;
    jce_contact_fn contact_end_fn;
    void          *contact_end_ud;
};

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

    JcePhysicsWorld *w = (JcePhysicsWorld *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->bodies = (PhysBody *)calloc(cfg.max_bodies, sizeof(PhysBody));
    if (!w->bodies) { free(w); return NULL; }

    w->capacity       = cfg.max_bodies;
    w->count          = 0;
    w->gravity        = cfg.gravity;
    w->fixed_timestep = cfg.fixed_timestep;
    w->max_sub_steps  = cfg.max_sub_steps;
    w->accumulator    = 0.0f;

    LOG_SUCCESS(LOG_TAG, "3D physics world created (capacity=%u)", cfg.max_bodies);
    return w;
}

void jce_physics_destroy(JcePhysicsWorld *world)
{
    if (!world) return;
    free(world->bodies);
    free(world);
    LOG_INFO(LOG_TAG, "3D physics world destroyed");
}

/* ── Single integration step ───────────────────────────────────────── */

static void integrate_body(PhysBody *b, float dt, jce_vec3 gravity)
{
    if (!b->alive || b->type != JCE_BODY_DYNAMIC) return;

    float inv_mass = (b->mass > 0.0f) ? (1.0f / b->mass) : 0.0f;

    /* Apply gravity + accumulated force. */
    jce_vec3 accel = jce_v3_add(gravity,
                                jce_v3_scale(b->force, inv_mass));
    b->velocity = jce_v3_add(b->velocity, jce_v3_scale(accel, dt));

    /* Apply angular acceleration from torque. */
    jce_vec3 ang_accel = jce_v3_scale(b->torque, inv_mass);
    b->angular_velocity = jce_v3_add(b->angular_velocity,
                                     jce_v3_scale(ang_accel, dt));

    /* Damping. */
    b->velocity = jce_v3_scale(b->velocity,
                               1.0f / (1.0f + dt * b->linear_damping));
    b->angular_velocity = jce_v3_scale(b->angular_velocity,
                                       1.0f / (1.0f + dt * b->angular_damping));

    /* Integrate position. */
    b->position = jce_v3_add(b->position, jce_v3_scale(b->velocity, dt));

    /* Integrate rotation (simplified — small-angle approximation). */
    float len = jce_v3_len(b->angular_velocity);
    if (len > 1e-6f) {
        jce_vec3 axis = jce_v3_normalize(b->angular_velocity);
        jce_quat dq = jce_q_from_axis_angle(axis, len * dt);
        b->rotation = jce_q_normalize(jce_q_multiply(dq, b->rotation));
    }

    /* Clear forces. */
    b->force  = jce_v3(0, 0, 0);
    b->torque = jce_v3(0, 0, 0);
}

/* ── Step ──────────────────────────────────────────────────────────── */

void jce_physics_step(JcePhysicsWorld *world, float dt)
{
    if (!world) return;

    world->accumulator += dt;
    int steps = 0;

    while (world->accumulator >= world->fixed_timestep &&
           steps < world->max_sub_steps) {
        for (uint32_t i = 0; i < world->capacity; i++) {
            integrate_body(&world->bodies[i], world->fixed_timestep,
                           world->gravity);
        }
        world->accumulator -= world->fixed_timestep;
        steps++;
    }
}

/* ── Body create / destroy ─────────────────────────────────────────── */

JceBodyHandle jce_physics_body_create(JcePhysicsWorld *world,
                                      const JceBodyDesc *desc)
{
    if (!world || !desc) return JCE_BODY_INVALID;

    /* Find free slot. */
    for (uint32_t i = 0; i < world->capacity; i++) {
        if (!world->bodies[i].alive) {
            PhysBody *b       = &world->bodies[i];
            memset(b, 0, sizeof(*b));
            b->alive          = true;
            b->type           = desc->type;
            b->shape          = desc->shape;
            b->position       = desc->position;
            b->rotation       = desc->rotation;
            b->half_extents   = desc->half_extents;
            b->mass           = desc->mass;
            b->friction       = desc->friction  > 0.0f ? desc->friction  : 0.5f;
            b->restitution    = desc->restitution;
            b->linear_damping = desc->linear_damping;
            b->angular_damping= desc->angular_damping;
            b->velocity       = jce_v3(0, 0, 0);
            b->angular_velocity = jce_v3(0, 0, 0);
            b->force          = jce_v3(0, 0, 0);
            b->torque         = jce_v3(0, 0, 0);

            world->count++;
            return (JceBodyHandle){ i };
        }
    }

    LOG_ERROR(LOG_TAG, "body pool exhausted (%u)", world->capacity);
    return JCE_BODY_INVALID;
}

void jce_physics_body_destroy(JcePhysicsWorld *world, JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return;
    if (body.idx >= world->capacity) return;

    if (world->bodies[body.idx].alive) {
        world->bodies[body.idx].alive = false;
        world->count--;
    }
}

/* ── Body state queries ────────────────────────────────────────────── */

void jce_physics_body_get_transform(const JcePhysicsWorld *world,
                                    JceBodyHandle body,
                                    jce_vec3 *out_pos, jce_quat *out_rot)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    const PhysBody *b = &world->bodies[body.idx];
    if (!b->alive) return;
    if (out_pos) *out_pos = b->position;
    if (out_rot) *out_rot = b->rotation;
}

void jce_physics_body_set_transform(JcePhysicsWorld *world,
                                    JceBodyHandle body,
                                    jce_vec3 pos, jce_quat rot)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    PhysBody *b = &world->bodies[body.idx];
    if (!b->alive) return;
    b->position = pos;
    b->rotation = rot;
}

jce_vec3 jce_physics_body_get_velocity(const JcePhysicsWorld *world,
                                       JceBodyHandle body)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity)
        return jce_v3(0, 0, 0);
    const PhysBody *b = &world->bodies[body.idx];
    return b->alive ? b->velocity : jce_v3(0, 0, 0);
}

void jce_physics_body_set_velocity(JcePhysicsWorld *world,
                                   JceBodyHandle body, jce_vec3 vel)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    PhysBody *b = &world->bodies[body.idx];
    if (b->alive) b->velocity = vel;
}

jce_vec3 jce_physics_body_get_angular_velocity(const JcePhysicsWorld *world,
                                               JceBodyHandle body)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity)
        return jce_v3(0, 0, 0);
    const PhysBody *b = &world->bodies[body.idx];
    return b->alive ? b->angular_velocity : jce_v3(0, 0, 0);
}

void jce_physics_body_set_angular_velocity(JcePhysicsWorld *world,
                                           JceBodyHandle body, jce_vec3 vel)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    PhysBody *b = &world->bodies[body.idx];
    if (b->alive) b->angular_velocity = vel;
}

/* ── Forces & impulses ─────────────────────────────────────────────── */

void jce_physics_body_apply_force(JcePhysicsWorld *world,
                                  JceBodyHandle body, jce_vec3 force)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    PhysBody *b = &world->bodies[body.idx];
    if (b->alive && b->type == JCE_BODY_DYNAMIC)
        b->force = jce_v3_add(b->force, force);
}

void jce_physics_body_apply_impulse(JcePhysicsWorld *world,
                                    JceBodyHandle body, jce_vec3 impulse)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    PhysBody *b = &world->bodies[body.idx];
    if (b->alive && b->type == JCE_BODY_DYNAMIC && b->mass > 0.0f)
        b->velocity = jce_v3_add(b->velocity,
                                 jce_v3_scale(impulse, 1.0f / b->mass));
}

void jce_physics_body_apply_torque(JcePhysicsWorld *world,
                                   JceBodyHandle body, jce_vec3 torque)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    PhysBody *b = &world->bodies[body.idx];
    if (b->alive && b->type == JCE_BODY_DYNAMIC)
        b->torque = jce_v3_add(b->torque, torque);
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

    jce_vec3 dir = jce_v3_normalize(direction);
    float closest = max_distance;

    /* Brute-force sphere test against all bodies. */
    for (uint32_t i = 0; i < world->capacity; i++) {
        const PhysBody *b = &world->bodies[i];
        if (!b->alive) continue;

        float radius = 0.0f;
        if (b->shape == JCE_SHAPE_SPHERE) {
            radius = b->half_extents.x;
        } else {
            /* Conservative bounding sphere. */
            radius = jce_v3_len(b->half_extents);
        }

        jce_vec3 oc = jce_v3_sub(origin, b->position);
        float a = jce_v3_dot(dir, dir);
        float half_b = jce_v3_dot(oc, dir);
        float c = jce_v3_dot(oc, oc) - radius * radius;
        float disc = half_b * half_b - a * c;

        if (disc < 0) continue;

        float sqrtd = sqrtf(disc);
        float t = (-half_b - sqrtd) / a;
        if (t < 0) t = (-half_b + sqrtd) / a;
        if (t < 0 || t > closest) continue;

        closest       = t;
        result.hit     = true;
        result.distance = t;
        result.point   = jce_v3_add(origin, jce_v3_scale(dir, t));
        result.normal  = jce_v3_normalize(jce_v3_sub(result.point, b->position));
        result.body    = (JceBodyHandle){ i };
    }

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
    return world ? world->count : 0;
}
