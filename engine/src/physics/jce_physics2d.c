/*
 * jce_physics2d.c  2D physics implementation (Box2D 3.x concepts).
 *
 * Provides a lightweight 2D physics simulation with rigid bodies,
 * collision shapes, and ray casting.  Bodies are stored in a dense
 * pool for cache-friendly iteration.
 */

#include <jce/physics/jce_physics2d.h>
#include <jce/core/jce_log.h>

#include <string.h>
#include <stdlib.h>
#include <math.h>

#define LOG_TAG "physics2d"

/* ── Internal body record ──────────────────────────────────────────── */

typedef struct {
    bool            alive;
    JceBodyType     type;
    JceShape2DType  shape;
    jce_vec2        position;
    float           angle;
    jce_vec2        velocity;
    float           angular_velocity;
    jce_vec2        half_extents;
    float           mass;
    float           friction;
    float           restitution;
    float           linear_damping;
    float           angular_damping;
    bool            fixed_rotation;
    /* Accumulated forces (cleared each step). */
    jce_vec2        force;
    float           torque;
} Phys2DBody;

/* ── World struct ──────────────────────────────────────────────────── */

struct JcePhysics2D {
    Phys2DBody *bodies;
    uint32_t    capacity;
    uint32_t    count;
    jce_vec2    gravity;
};

/* ── Create / Destroy ──────────────────────────────────────────────── */

JcePhysics2D *jce_physics2d_create(const JcePhysics2DDesc *desc)
{
    uint32_t max_bodies = (desc && desc->max_bodies) ? desc->max_bodies : 4096;

    JcePhysics2D *w = (JcePhysics2D *)calloc(1, sizeof(*w));
    if (!w) return NULL;

    w->bodies = (Phys2DBody *)calloc(max_bodies, sizeof(Phys2DBody));
    if (!w->bodies) { free(w); return NULL; }

    w->capacity = max_bodies;
    w->count    = 0;

    if (desc) {
        w->gravity = desc->gravity;
    } else {
        w->gravity.x = 0.0f;
        w->gravity.y = -9.81f;
    }

    LOG_SUCCESS(LOG_TAG, "2D physics world created (capacity=%u)", max_bodies);
    return w;
}

void jce_physics2d_destroy(JcePhysics2D *world)
{
    if (!world) return;
    free(world->bodies);
    free(world);
    LOG_INFO(LOG_TAG, "2D physics world destroyed");
}

/* ── Step ──────────────────────────────────────────────────────────── */

static void integrate_body_2d(Phys2DBody *b, float dt, jce_vec2 gravity)
{
    if (!b->alive || b->type != JCE_BODY_DYNAMIC) return;

    float inv_mass = (b->mass > 0.0f) ? (1.0f / b->mass) : 0.0f;

    /* Linear acceleration. */
    jce_vec2 accel;
    accel.x = gravity.x + b->force.x * inv_mass;
    accel.y = gravity.y + b->force.y * inv_mass;
    b->velocity.x += accel.x * dt;
    b->velocity.y += accel.y * dt;

    /* Angular acceleration. */
    if (!b->fixed_rotation) {
        b->angular_velocity += b->torque * inv_mass * dt;
    }

    /* Damping. */
    float ld = 1.0f / (1.0f + dt * b->linear_damping);
    b->velocity.x *= ld;
    b->velocity.y *= ld;
    b->angular_velocity *= 1.0f / (1.0f + dt * b->angular_damping);

    /* Integrate position. */
    b->position.x += b->velocity.x * dt;
    b->position.y += b->velocity.y * dt;
    b->angle      += b->angular_velocity * dt;

    /* Clear forces. */
    b->force.x = 0.0f;
    b->force.y = 0.0f;
    b->torque  = 0.0f;
}

void jce_physics2d_step(JcePhysics2D *world, float dt)
{
    if (!world) return;
    for (uint32_t i = 0; i < world->capacity; i++) {
        integrate_body_2d(&world->bodies[i], dt, world->gravity);
    }
}

/* ── Body create / destroy ─────────────────────────────────────────── */

JceBodyHandle jce_physics2d_body_create(JcePhysics2D *world,
                                        const JceBody2DDesc *desc)
{
    if (!world || !desc) return JCE_BODY_INVALID;

    for (uint32_t i = 0; i < world->capacity; i++) {
        if (!world->bodies[i].alive) {
            Phys2DBody *b       = &world->bodies[i];
            memset(b, 0, sizeof(*b));
            b->alive            = true;
            b->type             = desc->type;
            b->shape            = desc->shape;
            b->position         = desc->position;
            b->angle            = desc->angle;
            b->half_extents     = desc->half_extents;
            b->mass             = desc->mass;
            b->friction         = desc->friction  > 0.0f ? desc->friction  : 0.5f;
            b->restitution      = desc->restitution;
            b->linear_damping   = desc->linear_damping;
            b->angular_damping  = desc->angular_damping;
            b->fixed_rotation   = desc->fixed_rotation;

            world->count++;
            return (JceBodyHandle){ i };
        }
    }

    LOG_ERROR(LOG_TAG, "2D body pool exhausted (%u)", world->capacity);
    return JCE_BODY_INVALID;
}

void jce_physics2d_body_destroy(JcePhysics2D *world, JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return;
    if (body.idx >= world->capacity) return;

    if (world->bodies[body.idx].alive) {
        world->bodies[body.idx].alive = false;
        world->count--;
    }
}

/* ── Body state queries ────────────────────────────────────────────── */

void jce_physics2d_body_get_transform(const JcePhysics2D *world,
                                      JceBodyHandle body,
                                      jce_vec2 *out_pos, float *out_angle)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    const Phys2DBody *b = &world->bodies[body.idx];
    if (!b->alive) return;
    if (out_pos)   *out_pos   = b->position;
    if (out_angle) *out_angle = b->angle;
}

void jce_physics2d_body_set_transform(JcePhysics2D *world,
                                      JceBodyHandle body,
                                      jce_vec2 pos, float angle)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    Phys2DBody *b = &world->bodies[body.idx];
    if (!b->alive) return;
    b->position = pos;
    b->angle    = angle;
}

jce_vec2 jce_physics2d_body_get_velocity(const JcePhysics2D *world,
                                         JceBodyHandle body)
{
    jce_vec2 zero = {{0, 0}};
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity)
        return zero;
    const Phys2DBody *b = &world->bodies[body.idx];
    return b->alive ? b->velocity : zero;
}

void jce_physics2d_body_set_velocity(JcePhysics2D *world,
                                     JceBodyHandle body, jce_vec2 vel)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    Phys2DBody *b = &world->bodies[body.idx];
    if (b->alive) b->velocity = vel;
}

/* ── Forces & impulses ─────────────────────────────────────────────── */

void jce_physics2d_body_apply_force(JcePhysics2D *world,
                                    JceBodyHandle body, jce_vec2 force)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    Phys2DBody *b = &world->bodies[body.idx];
    if (b->alive && b->type == JCE_BODY_DYNAMIC) {
        b->force.x += force.x;
        b->force.y += force.y;
    }
}

void jce_physics2d_body_apply_impulse(JcePhysics2D *world,
                                      JceBodyHandle body, jce_vec2 impulse)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    Phys2DBody *b = &world->bodies[body.idx];
    if (b->alive && b->type == JCE_BODY_DYNAMIC && b->mass > 0.0f) {
        float inv = 1.0f / b->mass;
        b->velocity.x += impulse.x * inv;
        b->velocity.y += impulse.y * inv;
    }
}

void jce_physics2d_body_apply_torque(JcePhysics2D *world,
                                     JceBodyHandle body, float torque)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    Phys2DBody *b = &world->bodies[body.idx];
    if (b->alive && b->type == JCE_BODY_DYNAMIC && !b->fixed_rotation)
        b->torque += torque;
}

/* ── Ray casting ───────────────────────────────────────────────────── */

JceRaycast2DResult jce_physics2d_raycast(const JcePhysics2D *world,
                                         jce_vec2 origin, jce_vec2 direction,
                                         float max_distance)
{
    JceRaycast2DResult result;
    memset(&result, 0, sizeof(result));
    result.body = JCE_BODY_INVALID;

    if (!world) return result;

    /* Normalize direction. */
    float dlen = sqrtf(direction.x * direction.x + direction.y * direction.y);
    if (dlen < 1e-8f) return result;
    float inv_dlen = 1.0f / dlen;
    jce_vec2 dir;
    dir.x = direction.x * inv_dlen;
    dir.y = direction.y * inv_dlen;

    float closest = max_distance;

    for (uint32_t i = 0; i < world->capacity; i++) {
        const Phys2DBody *b = &world->bodies[i];
        if (!b->alive) continue;

        /* Conservative bounding circle. */
        float radius = sqrtf(b->half_extents.x * b->half_extents.x +
                             b->half_extents.y * b->half_extents.y);

        float ocx = origin.x - b->position.x;
        float ocy = origin.y - b->position.y;
        float a   = dir.x * dir.x + dir.y * dir.y;
        float hb  = ocx * dir.x + ocy * dir.y;
        float c   = ocx * ocx + ocy * ocy - radius * radius;
        float disc = hb * hb - a * c;

        if (disc < 0) continue;

        float sqrtd = sqrtf(disc);
        float t = (-hb - sqrtd) / a;
        if (t < 0) t = (-hb + sqrtd) / a;
        if (t < 0 || t > closest) continue;

        closest        = t;
        result.hit     = true;
        result.fraction = t / max_distance;
        result.point.x = origin.x + dir.x * t;
        result.point.y = origin.y + dir.y * t;
        float nx = result.point.x - b->position.x;
        float ny = result.point.y - b->position.y;
        float nlen = sqrtf(nx * nx + ny * ny);
        if (nlen > 1e-8f) {
            result.normal.x = nx / nlen;
            result.normal.y = ny / nlen;
        }
        result.body = (JceBodyHandle){ i };
    }

    return result;
}

/* ── Debug ─────────────────────────────────────────────────────────── */

uint32_t jce_physics2d_body_count(const JcePhysics2D *world)
{
    return world ? world->count : 0;
}
