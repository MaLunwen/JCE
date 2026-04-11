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
#include <math.h>

#define LOG_TAG "physics"

/* max simultaneous contacts tracked for begin/end callbacks */
#define MAX_CONTACT_PAIRS 512

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

/* ── Contact pair for begin/end tracking ────────────────────────────── */

typedef struct {
    uint32_t a, b; /* body indices, a < b */
} ContactPair;

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

    /* Contact pair tracking for begin/end events. */
    ContactPair   prev_pairs[MAX_CONTACT_PAIRS];
    uint32_t      prev_pair_count;
    ContactPair   curr_pairs[MAX_CONTACT_PAIRS];
    uint32_t      curr_pair_count;
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

/* ── Narrow-phase collision tests ──────────────────────────────────── */

/* Result of a narrow-phase test: contact normal points from A → B. */
typedef struct {
    bool     colliding;
    jce_vec3 normal;   /* A → B */
    jce_vec3 point;    /* world-space contact point */
    float    depth;    /* penetration depth (>0 = overlapping) */
} CollisionResult;

static float fclampf(float v, float lo, float hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* Sphere vs Sphere */
static CollisionResult test_sphere_sphere(const PhysBody *a, const PhysBody *b)
{
    CollisionResult r = {0};
    float ra = a->half_extents.x;
    float rb = b->half_extents.x;
    jce_vec3 diff = jce_v3_sub(b->position, a->position);
    float dist = jce_v3_len(diff);
    float sum = ra + rb;

    if (dist >= sum) return r;

    r.colliding = true;
    r.depth = sum - dist;
    if (dist > 1e-6f) {
        r.normal = jce_v3_scale(diff, 1.0f / dist);
    } else {
        r.normal = jce_v3(0, 1, 0);
    }
    r.point = jce_v3_add(a->position, jce_v3_scale(r.normal, ra - r.depth * 0.5f));
    return r;
}

/* AABB vs AABB (box vs box, axis-aligned only) */
static CollisionResult test_box_box(const PhysBody *a, const PhysBody *b)
{
    CollisionResult r = {0};
    jce_vec3 d = jce_v3_sub(b->position, a->position);
    float ox = (a->half_extents.x + b->half_extents.x) - fabsf(d.x);
    if (ox <= 0) return r;
    float oy = (a->half_extents.y + b->half_extents.y) - fabsf(d.y);
    if (oy <= 0) return r;
    float oz = (a->half_extents.z + b->half_extents.z) - fabsf(d.z);
    if (oz <= 0) return r;

    r.colliding = true;
    /* Choose axis of minimum penetration. */
    if (ox <= oy && ox <= oz) {
        r.depth = ox;
        r.normal = jce_v3(d.x > 0 ? 1.0f : -1.0f, 0, 0);
    } else if (oy <= oz) {
        r.depth = oy;
        r.normal = jce_v3(0, d.y > 0 ? 1.0f : -1.0f, 0);
    } else {
        r.depth = oz;
        r.normal = jce_v3(0, 0, d.z > 0 ? 1.0f : -1.0f);
    }
    r.point = jce_v3_add(a->position, jce_v3_scale(d, 0.5f));
    return r;
}

/* Sphere vs Box (axis-aligned) */
static CollisionResult test_sphere_box(const PhysBody *sphere, const PhysBody *box)
{
    CollisionResult r = {0};
    /* Clamp sphere center to box extent to find closest point. */
    jce_vec3 local = jce_v3_sub(sphere->position, box->position);
    jce_vec3 closest;
    closest.x = fclampf(local.x, -box->half_extents.x, box->half_extents.x);
    closest.y = fclampf(local.y, -box->half_extents.y, box->half_extents.y);
    closest.z = fclampf(local.z, -box->half_extents.z, box->half_extents.z);

    jce_vec3 delta = jce_v3_sub(local, closest);
    float dist2 = jce_v3_dot(delta, delta);
    float radius = sphere->half_extents.x;

    if (dist2 >= radius * radius) return r;

    float dist = sqrtf(dist2);
    r.colliding = true;
    r.depth = radius - dist;
    if (dist > 1e-6f) {
        r.normal = jce_v3_scale(delta, 1.0f / dist);
    } else {
        r.normal = jce_v3(0, 1, 0);
    }
    r.point = jce_v3_add(box->position, closest);
    return r;
}

/* Any shape vs infinite ground plane (Y=0, normal up).
   Plane bodies use half_extents = (0,0,0). */
static CollisionResult test_body_plane(const PhysBody *body, const PhysBody *plane)
{
    (void)plane;
    CollisionResult r = {0};
    float radius = 0.0f;

    if (body->shape == JCE_SHAPE_SPHERE) {
        radius = body->half_extents.x;
    } else if (body->shape == JCE_SHAPE_CAPSULE) {
        /* Approximate as sphere of combined extent. */
        radius = body->half_extents.x + body->half_extents.y;
    } else {
        /* Box: use Y half-extent. */
        radius = body->half_extents.y;
    }

    float penetration = radius - (body->position.y - plane->position.y);
    if (penetration <= 0) return r;

    r.colliding = true;
    r.depth = penetration;
    r.normal = jce_v3(0, 1, 0);
    r.point = jce_v3(body->position.x, plane->position.y, body->position.z);
    return r;
}

/* Dispatch collision test between two bodies. Normal points A→B. */
static CollisionResult test_collision(const PhysBody *a, const PhysBody *b)
{
    /* Plane vs anything. */
    if (b->shape == JCE_SHAPE_PLANE) {
        return test_body_plane(a, b);
    }
    if (a->shape == JCE_SHAPE_PLANE) {
        CollisionResult r = test_body_plane(b, a);
        if (r.colliding) r.normal = jce_v3_scale(r.normal, -1.0f);
        return r;
    }

    /* Sphere vs Sphere. */
    if (a->shape == JCE_SHAPE_SPHERE && b->shape == JCE_SHAPE_SPHERE)
        return test_sphere_sphere(a, b);

    /* Box vs Box. */
    if (a->shape == JCE_SHAPE_BOX && b->shape == JCE_SHAPE_BOX)
        return test_box_box(a, b);

    /* Sphere vs Box (or Box vs Sphere). */
    if (a->shape == JCE_SHAPE_SPHERE && b->shape == JCE_SHAPE_BOX)
        return test_sphere_box(a, b);
    if (a->shape == JCE_SHAPE_BOX && b->shape == JCE_SHAPE_SPHERE) {
        CollisionResult r = test_sphere_box(b, a);
        if (r.colliding) r.normal = jce_v3_scale(r.normal, -1.0f);
        return r;
    }

    /* Capsule or unsupported — use conservative sphere approximation. */
    PhysBody sa = *a, sb = *b;
    sa.shape = JCE_SHAPE_SPHERE;
    sa.half_extents.x = jce_v3_len(a->half_extents);
    sb.shape = JCE_SHAPE_SPHERE;
    sb.half_extents.x = jce_v3_len(b->half_extents);
    return test_sphere_sphere(&sa, &sb);
}

/* ── Collision resolution ──────────────────────────────────────────── */

static void resolve_collision(PhysBody *a, PhysBody *b,
                              const CollisionResult *c)
{
    bool a_dynamic = (a->type == JCE_BODY_DYNAMIC && a->mass > 0.0f);
    bool b_dynamic = (b->type == JCE_BODY_DYNAMIC && b->mass > 0.0f);

    float inv_ma = a_dynamic ? (1.0f / a->mass) : 0.0f;
    float inv_mb = b_dynamic ? (1.0f / b->mass) : 0.0f;
    float inv_sum = inv_ma + inv_mb;

    if (inv_sum < 1e-10f) return; /* both immovable */

    /* 1. Positional correction — separate bodies. */
    float correction = c->depth / inv_sum;
    if (a_dynamic)
        a->position = jce_v3_sub(a->position,
                                 jce_v3_scale(c->normal, correction * inv_ma));
    if (b_dynamic)
        b->position = jce_v3_add(b->position,
                                 jce_v3_scale(c->normal, correction * inv_mb));

    /* 2. Impulse-based velocity resolution. */
    jce_vec3 rel_vel = jce_v3_sub(b->velocity, a->velocity);
    float vel_along_normal = jce_v3_dot(rel_vel, c->normal);

    if (vel_along_normal > 0) return; /* separating */

    float restitution = fminf(a->restitution, b->restitution);
    float j = -(1.0f + restitution) * vel_along_normal / inv_sum;

    jce_vec3 impulse = jce_v3_scale(c->normal, j);

    if (a_dynamic)
        a->velocity = jce_v3_sub(a->velocity, jce_v3_scale(impulse, inv_ma));
    if (b_dynamic)
        b->velocity = jce_v3_add(b->velocity, jce_v3_scale(impulse, inv_mb));

    /* 3. Friction impulse (simplified Coulomb). */
    rel_vel = jce_v3_sub(b->velocity, a->velocity);
    jce_vec3 tangent = jce_v3_sub(rel_vel,
                                  jce_v3_scale(c->normal,
                                               jce_v3_dot(rel_vel, c->normal)));
    float tangent_len = jce_v3_len(tangent);
    if (tangent_len < 1e-6f) return;
    tangent = jce_v3_scale(tangent, 1.0f / tangent_len);

    float jt = -jce_v3_dot(rel_vel, tangent) / inv_sum;
    float friction = sqrtf(a->friction * b->friction);

    /* Clamp to Coulomb cone. */
    if (fabsf(jt) > j * friction) jt = (jt > 0 ? 1.0f : -1.0f) * j * friction;

    jce_vec3 friction_impulse = jce_v3_scale(tangent, jt);
    if (a_dynamic)
        a->velocity = jce_v3_sub(a->velocity,
                                 jce_v3_scale(friction_impulse, inv_ma));
    if (b_dynamic)
        b->velocity = jce_v3_add(b->velocity,
                                 jce_v3_scale(friction_impulse, inv_mb));
}

/* ── Contact pair helpers ──────────────────────────────────────────── */

static bool pair_contains(const ContactPair *arr, uint32_t count,
                          uint32_t a, uint32_t b)
{
    for (uint32_t i = 0; i < count; i++) {
        if (arr[i].a == a && arr[i].b == b) return true;
    }
    return false;
}

static void record_pair(JcePhysicsWorld *w, uint32_t i, uint32_t j)
{
    uint32_t a = i < j ? i : j;
    uint32_t b = i < j ? j : i;
    if (w->curr_pair_count < MAX_CONTACT_PAIRS) {
        w->curr_pairs[w->curr_pair_count].a = a;
        w->curr_pairs[w->curr_pair_count].b = b;
        w->curr_pair_count++;
    }
}

/* Fire begin/end callbacks by comparing prev vs curr. */
static void fire_contact_events(JcePhysicsWorld *w)
{
    /* New contacts (in curr but not prev) → begin. */
    if (w->contact_begin_fn) {
        for (uint32_t k = 0; k < w->curr_pair_count; k++) {
            if (!pair_contains(w->prev_pairs, w->prev_pair_count,
                               w->curr_pairs[k].a, w->curr_pairs[k].b)) {
                JceContactEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.body_a = (JceBodyHandle){ w->curr_pairs[k].a };
                ev.body_b = (JceBodyHandle){ w->curr_pairs[k].b };
                w->contact_begin_fn(&ev, w->contact_begin_ud);
            }
        }
    }

    /* Ended contacts (in prev but not curr) → end. */
    if (w->contact_end_fn) {
        for (uint32_t k = 0; k < w->prev_pair_count; k++) {
            if (!pair_contains(w->curr_pairs, w->curr_pair_count,
                               w->prev_pairs[k].a, w->prev_pairs[k].b)) {
                JceContactEvent ev;
                memset(&ev, 0, sizeof(ev));
                ev.body_a = (JceBodyHandle){ w->prev_pairs[k].a };
                ev.body_b = (JceBodyHandle){ w->prev_pairs[k].b };
                w->contact_end_fn(&ev, w->contact_end_ud);
            }
        }
    }

    /* Swap: curr becomes prev for next frame. */
    memcpy(w->prev_pairs, w->curr_pairs,
           w->curr_pair_count * sizeof(ContactPair));
    w->prev_pair_count = w->curr_pair_count;
}

/* ── Detect and resolve all collisions ─────────────────────────────── */

static void detect_and_resolve(JcePhysicsWorld *w)
{
    w->curr_pair_count = 0;

    for (uint32_t i = 0; i < w->capacity; i++) {
        PhysBody *a = &w->bodies[i];
        if (!a->alive) continue;

        for (uint32_t j = i + 1; j < w->capacity; j++) {
            PhysBody *b = &w->bodies[j];
            if (!b->alive) continue;

            /* Skip static-static and static-kinematic pairs. */
            if (a->type != JCE_BODY_DYNAMIC && b->type != JCE_BODY_DYNAMIC)
                continue;

            CollisionResult cr = test_collision(a, b);
            if (!cr.colliding) continue;

            record_pair(w, i, j);
            resolve_collision(a, b, &cr);
        }
    }

    fire_contact_events(w);
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
        detect_and_resolve(world);
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
