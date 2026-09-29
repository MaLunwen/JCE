/*
 * jce_physics2d.c  2D physics implementation (Box2D 3.x backend).
 *
 * Wraps the Box2D v3 C API to provide 2D rigid-body simulation with
 * real collision detection, shape support, and ray casting.
 */

#include <jce/middleware/physics/jce_physics2d.h>
#include <jce/middleware/physics/jce_physics_layers.h>
#include <jce/os/core/jce_log.h>
#include <jce/os/core/jce_math.h>

#include "os/core/jce_memory.h"

#include <box2d/box2d.h>
#include <math.h>
#include <stdint.h>
#include <string.h>

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

/* One live joint slot.  Mirrors the body pool: a parallel id + alive array.
 * ground_id is the implicit static body created for a world-anchored joint
 * (b2_nullBodyId when the joint binds two real bodies), torn down with the
 * joint so it does not leak. */
typedef struct {
    b2JointId joint_id;
    b2BodyId  ground_id;   /* world-anchor ground body, or b2_nullBodyId */
    bool      alive;
} Joint2DSlot;

#define JOINT2D_POOL_CAP 1024u   /* generous fixed cap (matches body-pool style) */

struct JcePhysics2D {
    b2WorldId   world_id;
    b2BodyId   *body_ids;      /* pool: slot index -> b2BodyId          */
    bool       *slot_alive;    /* pool: is this slot occupied?          */
    uint32_t   *slot_layer;    /* pool: collision layer 0..31           */
    uint32_t    capacity;
    uint32_t    count;

    /* Joint pool (mirrors the body pool's slot scheme). */
    Joint2DSlot *joints;       /* pool: slot index -> joint record       */
    uint32_t     joint_capacity;
    uint32_t     joint_count;
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

        case JCE_SHAPE2D_CAPSULE:
        case JCE_SHAPE2D_CAPSULE_X: {
            /* half_extents: (radius, half_length) -- area is the same either
             * way round, so both axes share this branch. */
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

/* One place that turns a layer index into a Box2D filter.
 *
 * b2DefaultShapeDef() gives categoryBits=1, maskBits=UINT32_MAX -- "layer 0,
 * collides with everything" -- which is why an unfiltered 2D world behaved
 * exactly like a fully-permissive matrix and the authored grid changing
 * nothing was invisible.  The default is not wrong, it is just not the
 * author's. */
static b2Filter layer_filter(uint32_t layer)
{
    b2Filter f = b2DefaultFilter();
    if (layer >= JCE_PHYSICS_LAYER_COUNT) layer = 0u;
    f.categoryBits = (uint64_t)1u << layer;
    f.maskBits     = (uint64_t)jce_physics2d_get_layer_collision_mask(layer);
    return f;
}

/* ── Attach a shape to a newly created body ───────────────────────── */

static void attach_shape(b2BodyId body_id,
                         uint32_t layer,
                         bool sensor,
                         JceShape2DType shape,
                         jce_vec2 half_extents,
                         float density,
                         float friction,
                         float restitution,
                         const float *points, int point_count)
{
    b2ShapeDef shape_def = b2DefaultShapeDef();
    /* isSensor alone reports nothing: Box2D gates begin/end touch events on
     * enableSensorEvents, so a "trigger" without it is just a collider that
     * has stopped pushing -- the worst of both. */
    shape_def.isSensor             = sensor;
    shape_def.enableSensorEvents   = sensor;
    shape_def.density              = density;
    shape_def.material.friction    = friction;
    shape_def.material.restitution = restitution;
    shape_def.filter               = layer_filter(layer);

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

        case JCE_SHAPE2D_CAPSULE:
        case JCE_SHAPE2D_CAPSULE_X: {
            float r  = half_extents.x;
            float hl = half_extents.y;
            bool  horiz = (shape == JCE_SHAPE2D_CAPSULE_X);
            b2Capsule capsule;
            capsule.center1 = horiz ? (b2Vec2){ -hl, 0.0f }
                                    : (b2Vec2){ 0.0f, -hl };
            capsule.center2 = horiz ? (b2Vec2){  hl, 0.0f }
                                    : (b2Vec2){ 0.0f,  hl };
            capsule.radius  = r;
            b2CreateCapsuleShape(body_id, &shape_def, &capsule);
            break;
        }

        case JCE_SHAPE2D_POLYGON: {
            /* b2ComputeHull rejects fewer than 3 points and silently drops
             * anything past B2_MAX_POLYGON_VERTICES.  A rejected hull would
             * leave the body with NO shape -- an invisible hole in the level
             * -- so fall back to the half-extents box, which is what this
             * shape did before it existed. */
            b2Vec2 pts[B2_MAX_POLYGON_VERTICES];
            int n = point_count;
            if (n > B2_MAX_POLYGON_VERTICES) n = B2_MAX_POLYGON_VERTICES;
            for (int i = 0; i < n; ++i) {
                pts[i].x = points[i * 2 + 0];
                pts[i].y = points[i * 2 + 1];
            }
            if (n >= 3) {
                b2Hull hull = b2ComputeHull(pts, n);
                if (hull.count >= 3) {
                    b2Polygon poly = b2MakePolygon(&hull, 0.0f);
                    b2CreatePolygonShape(body_id, &shape_def, &poly);
                    break;
                }
            }
            {
                b2Polygon box = b2MakeBox(half_extents.x > 0.0f
                                              ? half_extents.x : 0.5f,
                                          half_extents.y > 0.0f
                                              ? half_extents.y : 0.5f);
                b2CreatePolygonShape(body_id, &shape_def, &box);
            }
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
    w->slot_layer = (uint32_t *)JCE_CALLOC(max_bodies, sizeof(uint32_t));
    w->joints     = (Joint2DSlot *)JCE_CALLOC(JOINT2D_POOL_CAP, sizeof(Joint2DSlot));
    if (!w->body_ids || !w->slot_alive || !w->slot_layer || !w->joints) {
        JCE_FREE(w->body_ids);
        JCE_FREE(w->slot_alive);
        JCE_FREE(w->slot_layer);
        JCE_FREE(w->joints);
        JCE_FREE(w);
        return NULL;
    }

    w->capacity       = max_bodies;
    w->count          = 0;
    w->joint_capacity = JOINT2D_POOL_CAP;
    w->joint_count    = 0;

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

    /* b2DestroyWorld auto-destroys every body AND joint living in the world,
     * so we must NOT call b2DestroyJoint afterwards (double-free).  Just drop
     * the world once; the joint pool's b2JointId/b2BodyId become stale but are
     * never touched again because we free the pool right below. */
    b2DestroyWorld(world->world_id);

    JCE_FREE(world->body_ids);
    JCE_FREE(world->slot_alive);
    JCE_FREE(world->slot_layer);
    JCE_FREE(world->joints);
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

    attach_shape(body_id, desc->physics_layer, desc->sensor, desc->shape,
                 desc->half_extents,
                 density, friction, restitution,
                 desc->points, desc->point_count);

    /* Record in pool. */
    world->body_ids[slot]   = body_id;
    world->slot_alive[slot] = true;
    /* RECORD THE LAYER, so a shape added later through
     * jce_physics2d_body_add_box inherits this body's layer instead of
     * silently landing on Default.  Without this, slot_layer stayed 0 for
     * every body and add_box rebuilt the exact defect this unit removes, on
     * the two-step construction path only. */
    world->slot_layer[slot] = desc->physics_layer < JCE_PHYSICS_LAYER_COUNT
                            ? desc->physics_layer : 0u;
    world->count++;

    return (JceBodyHandle){ slot };
}

JceBodyHandle jce_physics2d_body_create_empty(JcePhysics2D *world,
                                              jce_vec2 pos, float angle,
                                              JceBodyType type)
{
    if (!world) return JCE_BODY_INVALID;

    /* Find a free slot (mirrors jce_physics2d_body_create). */
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

    b2BodyDef body_def = b2DefaultBodyDef();
    body_def.type      = to_b2_body_type(type);
    body_def.position  = to_b2(pos);
    body_def.rotation  = b2MakeRot(angle);
    /* Store pool index so raycast can recover the JceBodyHandle. */
    body_def.userData  = (void *)(uintptr_t)slot;

    b2BodyId body_id = b2CreateBody(world->world_id, &body_def);

    world->body_ids[slot]   = body_id;
    world->slot_alive[slot] = true;
    world->count++;

    return (JceBodyHandle){ slot };
}

bool jce_physics2d_body_add_box(JcePhysics2D *world, JceBodyHandle body,
                                jce_vec2 center_local, jce_vec2 half_extents,
                                float friction, float restitution, bool sensor)
{
    if (!world || !jce_body_valid(body) || body.idx >= world->capacity)
        return false;
    if (!world->slot_alive[body.idx]) return false;
    if (half_extents.x <= 0.0f || half_extents.y <= 0.0f) return false;

    b2ShapeDef shape_def = b2DefaultShapeDef();
    shape_def.material.friction    = friction >= 0.0f ? friction : 0.0f;
    shape_def.material.restitution = restitution >= 0.0f ? restitution : 0.0f;
    shape_def.isSensor             = sensor;
    shape_def.enableSensorEvents   = sensor;
    /* INHERIT the body's layer rather than defaulting to 0.  A shape added
     * after creation that silently landed on "Default" would collide with
     * everything -- the exact behaviour this unit exists to remove, reappearing
     * on the two-step construction path only. */
    shape_def.filter               = layer_filter(world->slot_layer[body.idx]);

    b2Polygon box = b2MakeOffsetBox(half_extents.x, half_extents.y,
                                    to_b2(center_local), b2MakeRot(0.0f));
    b2CreatePolygonShape(world->body_ids[body.idx], &shape_def, &box);
    return true;
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

/* ── Joints (Distance / Hinge / Spring) ───────────────────────────── */

/* Resolve a JceBodyHandle to its live b2BodyId (mirrors the guards used by
 * every body accessor above).  Returns false + b2_nullBodyId if the handle
 * is invalid, out of range, or its slot is dead. */
static bool resolve_b2_body(const JcePhysics2D *world, JceBodyHandle h,
                            b2BodyId *out)
{
    *out = b2_nullBodyId;
    if (!world) return false;
    if (!jce_body_valid(h) || h.idx >= world->capacity) return false;
    if (!world->slot_alive[h.idx]) return false;
    *out = world->body_ids[h.idx];
    return true;
}

JceConstraintHandle JCE_CALL jce_physics2d_joint_create(JcePhysics2D *world,
                                                        const JcePhysics2DJointDesc *desc)
{
    if (!world || !desc) return JCE_CONSTRAINT_INVALID;

    /* body_a MUST be a real, live body. */
    b2BodyId body_a;
    if (!resolve_b2_body(world, desc->body_a, &body_a)) {
        LOG_WARN(LOG_TAG, "2D joint: body_a is not a live body");
        return JCE_CONSTRAINT_INVALID;
    }

    /* body_b: a real body, or — when INVALID — an implicit static ground
     * body the joint owns (Box2D 3.1 has no global fixed body; a joint needs
     * two valid bodies, so the world anchor is a 0-mass static body placed at
     * the desc's connected/world anchor). */
    b2BodyId body_b      = b2_nullBodyId;
    b2BodyId ground_id   = b2_nullBodyId;
    bool     world_anchor = !jce_body_valid(desc->body_b);
    if (world_anchor) {
        b2BodyDef gd  = b2DefaultBodyDef();
        gd.type       = b2_staticBody;
        gd.position   = to_b2(desc->anchor_b);   /* world position of the anchor */
        ground_id     = b2CreateBody(world->world_id, &gd);
        body_b        = ground_id;
    } else if (!resolve_b2_body(world, desc->body_b, &body_b)) {
        LOG_WARN(LOG_TAG, "2D joint: body_b is not a live body");
        return JCE_CONSTRAINT_INVALID;
    }

    /* Find a free joint slot (linear scan mirrors the body pool). */
    uint32_t slot = UINT32_MAX;
    for (uint32_t i = 0; i < world->joint_capacity; i++) {
        if (!world->joints[i].alive) { slot = i; break; }
    }
    if (slot == UINT32_MAX) {
        LOG_ERROR(LOG_TAG, "2D joint pool exhausted (%u)", world->joint_capacity);
        if (world_anchor) b2DestroyBody(ground_id);
        return JCE_CONSTRAINT_INVALID;
    }

    /* For a world-anchored joint the ground body's local anchor is the origin
     * (we placed the ground body AT the anchor point); for a body↔body joint
     * the local anchor is the authored anchor_b in body_b's local space. */
    b2Vec2 local_anchor_b = world_anchor ? (b2Vec2){ 0.0f, 0.0f }
                                         : to_b2(desc->anchor_b);

    b2JointId jid = b2_nullJointId;

    switch (desc->kind) {
        case JCE_PHYSICS2D_JOINT_DISTANCE:
        case JCE_PHYSICS2D_JOINT_SPRING: {
            b2DistanceJointDef def = b2DefaultDistanceJointDef();
            def.bodyIdA      = body_a;
            def.bodyIdB      = body_b;
            def.localAnchorA = to_b2(desc->anchor_a);
            def.localAnchorB = local_anchor_b;
            def.length       = desc->distance > 0.0f ? desc->distance : 0.0f;
            if (desc->kind == JCE_PHYSICS2D_JOINT_SPRING) {
                def.enableSpring = true;
                def.hertz        = desc->frequency_hz;
                def.dampingRatio = desc->damping_ratio;
            } else {
                def.enableSpring = false;   /* rigid distance */
            }
            def.collideConnected = desc->collide_connected;
            jid = b2CreateDistanceJoint(world->world_id, &def);
            break;
        }

        case JCE_PHYSICS2D_JOINT_HINGE: {
            b2RevoluteJointDef def = b2DefaultRevoluteJointDef();
            def.bodyIdA        = body_a;
            def.bodyIdB        = body_b;
            def.localAnchorA   = to_b2(desc->anchor_a);
            def.localAnchorB   = local_anchor_b;
            def.enableMotor    = desc->use_motor;
            def.motorSpeed     = desc->motor_speed_rad_s;
            def.maxMotorTorque = desc->motor_max_torque;
            def.enableLimit    = desc->use_limits;
            def.lowerAngle     = desc->lower_angle_rad;
            def.upperAngle     = desc->upper_angle_rad;
            def.collideConnected = desc->collide_connected;
            jid = b2CreateRevoluteJoint(world->world_id, &def);
            break;
        }

        default:
            LOG_WARN(LOG_TAG, "2D joint: unknown kind %d", desc->kind);
            if (world_anchor) b2DestroyBody(ground_id);
            return JCE_CONSTRAINT_INVALID;
    }

    if (!b2Joint_IsValid(jid)) {
        LOG_ERROR(LOG_TAG, "2D joint: Box2D create failed (kind %d)", desc->kind);
        if (world_anchor) b2DestroyBody(ground_id);
        return JCE_CONSTRAINT_INVALID;
    }

    world->joints[slot].joint_id  = jid;
    world->joints[slot].ground_id = ground_id;   /* b2_nullBodyId if no ground */
    world->joints[slot].alive     = true;
    world->joint_count++;

    return (JceConstraintHandle){ slot };
}

/* Reaction force / torque from the last step.  Box2D has no breakable joint;
 * a break monitor reads these and destroys the joint itself. */
static b2JointId p2d_joint_id(const JcePhysics2D *world, JceConstraintHandle j)
{
    if (!world || !jce_constraint_valid(j)) return b2_nullJointId;
    uint32_t slot = j.idx;
    if (slot >= world->joint_capacity || !world->joints[slot].alive)
        return b2_nullJointId;
    return world->joints[slot].joint_id;
}

float jce_physics2d_joint_get_force(const JcePhysics2D *world,
                                    JceConstraintHandle joint)
{
    b2JointId id = p2d_joint_id(world, joint);
    if (!b2Joint_IsValid(id)) return 0.0f;
    b2Vec2 f = b2Joint_GetConstraintForce(id);
    return sqrtf(f.x * f.x + f.y * f.y);
}

float jce_physics2d_joint_get_torque(const JcePhysics2D *world,
                                     JceConstraintHandle joint)
{
    b2JointId id = p2d_joint_id(world, joint);
    if (!b2Joint_IsValid(id)) return 0.0f;
    return b2Joint_GetConstraintTorque(id);
}

void JCE_CALL jce_physics2d_joint_destroy(JcePhysics2D *world,
                                          JceConstraintHandle joint)
{
    if (!world || !jce_constraint_valid(joint))    return;
    if (joint.idx >= world->joint_capacity)        return;
    if (!world->joints[joint.idx].alive)           return;

    Joint2DSlot *s = &world->joints[joint.idx];

    /* Order: destroy the joint first, then the implicit ground body (a body
     * still referenced by a live joint must not be destroyed under it). */
    if (b2Joint_IsValid(s->joint_id))
        b2DestroyJoint(s->joint_id);
    if (b2Body_IsValid(s->ground_id))
        b2DestroyBody(s->ground_id);

    s->joint_id  = b2_nullJointId;
    s->ground_id = b2_nullBodyId;
    s->alive     = false;
    world->joint_count--;
}

/* ── Debug ─────────────────────────────────────────────────────────── */

uint32_t jce_physics2d_body_count(const JcePhysics2D *world)
{
    return world ? world->count : 0;
}
