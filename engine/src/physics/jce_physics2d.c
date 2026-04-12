/*
 * jce_physics2d.c  2D physics implementation (Box2D 3.x backend).
 *
 * Wraps the Box2D v3 C API to provide 2D rigid-body simulation with
 * real collision detection, shape support, and ray casting.
 */

#include <jce/physics/jce_physics2d.h>
#include <jce/core/jce_log.h>
#include "core/jce_memory.h"

#include <box2d/box2d.h>

#include <jce/core/jce_math.h>

#include <string.h>
#include <math.h>
#include <stdint.h>

#define LOG_TAG        "physics2d"
#define SUB_STEP_COUNT 4

/* ── Helpers: jce_vec2 <-> b2Vec2 ─────────────────────────────────── */

static inline b2Vec2 to_b2(jce_vec2 v)
{
    return (b2Vec2){ v.x, v.y };
}

static inline jce_vec2 from_b2(b2Vec2 v)
{
    jce_vec2 r;
    r.x = v.x;
    r.y = v.y;
    return r;
}

/* Map JceBodyType -> b2BodyType. */
static inline b2BodyType to_b2_body_type(JceBodyType t)
{
    switch (t) {
        case JCE_BODY_DYNAMIC:   return b2_dynamicBody;
        case JCE_BODY_KINEMATIC: return b2_kinematicBody;
        default:                 return b2_staticBody;
    }
}

/* ── World struct ─────────────────────────────────────────────────── */

struct JcePhysics2D {
    b2WorldId   world_id;
    b2BodyId   *body_ids;      /* pool: slot index -> b2BodyId          */
    bool       *slot_alive;    /* pool: is this slot occupied?          */
    uint32_t    capacity;
    uint32_t    count;
};

/* ── Shape area (for density = mass / area) ───────────────────────── */

static float shape_area(JceShape2DType shape, jce_vec2 half_extents)
{
    float hx = half_extents.x;
    float hy = half_extents.y;

    switch (shape) {
        case JCE_SHAPE2D_BOX:
            /* full width * full height */
            return 4.0f * hx * hy;

        case JCE_SHAPE2D_CIRCLE:
            /* radius stored in half_extents.x */
            return JCE_PI * hx * hx;

        case JCE_SHAPE2D_CAPSULE: {
            /* half_extents: (radius, half_length) */
            float r  = hx;
            float hl = hy;
            /* rectangle + two semicircles = full circle */
            return 2.0f * r * 2.0f * hl + JCE_PI * r * r;
        }

        case JCE_SHAPE2D_SEGMENT:
            /* line segment has no area; use a small fallback so
               density stays finite when mass > 0.                 */
            return 1.0f;

        default:
            return 1.0f;
    }
}

/* ── Attach a shape to a newly created body ───────────────────────── */

static void attach_shape(b2BodyId body_id,
                         JceShape2DType shape,
                         jce_vec2 half_extents,
                         float density,
                         float friction,
                         float restitution)
{
    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.density              = density;
    shape_def.material.friction    = friction;
    shape_def.material.restitution = restitution;

    switch (shape) {
        case JCE_SHAPE2D_BOX: {
            b2Polygon box = b2MakeBox(half_extents.x, half_extents.y);
            b2CreatePolygonShape(body_id, &shape_def, &box);
            break;
        }

        case JCE_SHAPE2D_CIRCLE: {
            b2Circle circle;
            circle.center = (b2Vec2){ 0.0f, 0.0f };
            circle.radius = half_extents.x;
            b2CreateCircleShape(body_id, &shape_def, &circle);
            break;
        }

        case JCE_SHAPE2D_CAPSULE: {
            float r  = half_extents.x;
            float hl = half_extents.y;
            b2Capsule capsule;
            capsule.center1 = (b2Vec2){ 0.0f, -hl };
            capsule.center2 = (b2Vec2){ 0.0f,  hl };
            capsule.radius  = r;
            b2CreateCapsuleShape(body_id, &shape_def, &capsule);
            break;
        }

        case JCE_SHAPE2D_SEGMENT: {
            float hx = half_extents.x;
            b2Segment seg;
            seg.point1 = (b2Vec2){ -hx, 0.0f };
            seg.point2 = (b2Vec2){  hx, 0.0f };
            b2CreateSegmentShape(body_id, &shape_def, &seg);
            break;
        }

        default:
            LOG_WARN(LOG_TAG, "unknown shape type %d, defaulting to box", (int)shape);
            {
                b2Polygon box = b2MakeBox(half_extents.x, half_extents.y);
                b2CreatePolygonShape(body_id, &shape_def, &box);
            }
            break;
    }
}

/* ── Create / Destroy ─────────────────────────────────────────────── */

JcePhysics2D *jce_physics2d_create(const JcePhysics2DDesc *desc)
{
    uint32_t max_bodies = (desc && desc->max_bodies) ? desc->max_bodies : 4096;

    JcePhysics2D *w = (JcePhysics2D *)JCE_CALLOC(1, sizeof(*w));
    if (!w) return NULL;

    w->body_ids   = (b2BodyId *)JCE_CALLOC(max_bodies, sizeof(b2BodyId));
    w->slot_alive = (bool *)JCE_CALLOC(max_bodies, sizeof(bool));
    if (!w->body_ids || !w->slot_alive) {
        JCE_FREE(w->body_ids);
        JCE_FREE(w->slot_alive);
        JCE_FREE(w);
        return NULL;
    }

    w->capacity = max_bodies;
    w->count    = 0;

    /* Create Box2D world. */
    b2WorldDef world_def = b2DefaultWorldDef();
    if (desc) {
        world_def.gravity = to_b2(desc->gravity);
    } else {
        world_def.gravity = (b2Vec2){ 0.0f, -9.81f };
    }

    w->world_id = b2CreateWorld(&world_def);

    LOG_SUCCESS(LOG_TAG, "2D physics world created (capacity=%u)", max_bodies);
    return w;
}

void jce_physics2d_destroy(JcePhysics2D *world)
{
    if (!world) return;

    b2DestroyWorld(world->world_id);

    JCE_FREE(world->body_ids);
    JCE_FREE(world->slot_alive);
    JCE_FREE(world);

    LOG_INFO(LOG_TAG, "2D physics world destroyed");
}

/* ── Step ──────────────────────────────────────────────────────────── */

void jce_physics2d_step(JcePhysics2D *world, float dt)
{
    if (!world) return;
    b2World_Step(world->world_id, dt, SUB_STEP_COUNT);
}

/* ── Body create / destroy ────────────────────────────────────────── */

JceBodyHandle jce_physics2d_body_create(JcePhysics2D *world,
                                        const JceBody2DDesc *desc)
{
    if (!world || !desc) return JCE_BODY_INVALID;

    /* Find a free slot. */
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < world->capacity; i++) {
        if (!world->slot_alive[i]) {
            slot = i;
            break;
        }
    }
    if (slot == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "2D body pool exhausted (%u)", world->capacity);
        return JCE_BODY_INVALID;
    }

    /* Body definition. */
    b2BodyDef body_def    = b2DefaultBodyDef();
    body_def.type         = to_b2_body_type(desc->type);
    body_def.position     = to_b2(desc->position);
    body_def.rotation     = b2MakeRot(desc->angle);
    body_def.linearDamping  = desc->linear_damping;
    body_def.angularDamping = desc->angular_damping;
    body_def.fixedRotation  = desc->fixed_rotation;
    /* Store pool index so raycast can recover the JceBodyHandle. */
    body_def.userData       = (void *)(uintptr_t)slot;

    b2BodyId body_id = b2CreateBody(world->world_id, &body_def);

    /* Compute density from mass (density = mass / area).
       For static bodies or zero mass, density stays 0.           */
    float density = 0.0f;
    if (desc->type == JCE_BODY_DYNAMIC && desc->mass > 0.0f) {
        float area = shape_area(desc->shape, desc->half_extents);
        density = desc->mass / area;
    }

    float friction    = desc->friction > 0.0f ? desc->friction : 0.5f;
    float restitution = desc->restitution;

    attach_shape(body_id, desc->shape, desc->half_extents,
                 density, friction, restitution);

    /* Record in pool. */
    world->body_ids[slot]   = body_id;
    world->slot_alive[slot] = true;
    world->count++;

    return (JceBodyHandle){ slot };
}

void jce_physics2d_body_destroy(JcePhysics2D *world, JceBodyHandle body)
{
    if (!world || !jce_body_valid(body)) return;
    if (body.idx >= world->capacity)     return;
    if (!world->slot_alive[body.idx])    return;

    b2DestroyBody(world->body_ids[body.idx]);

    world->slot_alive[body.idx] = false;
    world->count--;
}

/* ── Body state queries ───────────────────────────────────────────── */

void jce_physics2d_body_get_transform(const JcePhysics2D *world,
                                      JceBodyHandle body,
                                      jce_vec2 *out_pos, float *out_angle)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    if (!world->slot_alive[body.idx]) return;

    b2BodyId bid = world->body_ids[body.idx];
    if (out_pos)   *out_pos   = from_b2(b2Body_GetPosition(bid));
    if (out_angle) *out_angle = b2Rot_GetAngle(b2Body_GetRotation(bid));
}

void jce_physics2d_body_set_transform(JcePhysics2D *world,
                                      JceBodyHandle body,
                                      jce_vec2 pos, float angle)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    if (!world->slot_alive[body.idx]) return;

    b2Body_SetTransform(world->body_ids[body.idx],
                        to_b2(pos), b2MakeRot(angle));
}

jce_vec2 jce_physics2d_body_get_velocity(const JcePhysics2D *world,
                                         JceBodyHandle body)
{
    jce_vec2 zero;
    zero.x = 0.0f;
    zero.y = 0.0f;

    if (!world || !jce_body_valid(body) || body.idx >= world->capacity)
        return zero;
    if (!world->slot_alive[body.idx])
        return zero;

    return from_b2(b2Body_GetLinearVelocity(world->body_ids[body.idx]));
}

void jce_physics2d_body_set_velocity(JcePhysics2D *world,
                                     JceBodyHandle body, jce_vec2 vel)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    if (!world->slot_alive[body.idx]) return;

    b2Body_SetLinearVelocity(world->body_ids[body.idx], to_b2(vel));
}

/* ── Forces & impulses ────────────────────────────────────────────── */

void jce_physics2d_body_apply_force(JcePhysics2D *world,
                                    JceBodyHandle body, jce_vec2 force)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    if (!world->slot_alive[body.idx]) return;

    b2Body_ApplyForceToCenter(world->body_ids[body.idx], to_b2(force), true);
}

void jce_physics2d_body_apply_impulse(JcePhysics2D *world,
                                      JceBodyHandle body, jce_vec2 impulse)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    if (!world->slot_alive[body.idx]) return;

    b2Body_ApplyLinearImpulseToCenter(world->body_ids[body.idx],
                                      to_b2(impulse), true);
}

void jce_physics2d_body_apply_torque(JcePhysics2D *world,
                                     JceBodyHandle body, float torque)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity) return;
    if (!world->slot_alive[body.idx]) return;

    b2Body_ApplyTorque(world->body_ids[body.idx], torque, true);
}

/* ── Ray casting ──────────────────────────────────────────────────── */

JceRaycast2DResult jce_physics2d_raycast(const JcePhysics2D *world,
                                         jce_vec2 origin, jce_vec2 direction,
                                         float max_distance)
{
    JceRaycast2DResult result;
    memset(&result, 0, sizeof(result));
    result.body = JCE_BODY_INVALID;

    if (!world) return result;

    /* Normalize direction, then scale to produce the translation vector. */
    jce_vec2 dir_n = jce_v2_normalize(direction);
    if (dir_n.x == 0 && dir_n.y == 0) return result;

    b2Vec2 translation;
    translation.x = dir_n.x * max_distance;
    translation.y = dir_n.y * max_distance;

    b2QueryFilter filter = b2DefaultQueryFilter();
    b2RayResult ray = b2World_CastRayClosest(world->world_id,
                                             to_b2(origin),
                                             translation,
                                             filter);

    if (!ray.hit) return result;

    result.hit      = true;
    result.point    = from_b2(ray.point);
    result.normal   = from_b2(ray.normal);
    result.fraction = ray.fraction;

    /* Recover the pool index from the body's userData. */
    b2BodyId hit_body = b2Shape_GetBody(ray.shapeId);
    void *ud = b2Body_GetUserData(hit_body);
    result.body = (JceBodyHandle){ (uint32_t)(uintptr_t)ud };

    return result;
}

/* ── Debug ─────────────────────────────────────────────────────────── */

uint32_t jce_physics2d_body_count(const JcePhysics2D *world)
{
    return world ? world->count : 0;
}
