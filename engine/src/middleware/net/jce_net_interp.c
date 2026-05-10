/*
 * jce_net_interp.c  Snapshot ring + render-time interpolation.
 *
 * Storage = circular array of {server_time, owned snapshot copy}.
 * Push appends at head, evicting the oldest entry when full.  Sample
 * binary-searches for the bracketing pair.
 *
 * Field-wise interpolation: linear lerp for vectors / floats, slerp
 * for the rotation quaternion (we approximate with normalised lerp
 * since `t` is small per step and the quat math kept lightweight).
 */

#include <jce/middleware/net/jce_net_interp.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

typedef struct {
    double             server_time;
    JceNetEntitySnap  *entities;
    uint32_t           count;
} Slot;

struct JceNetSnapshotRing {
    Slot     *slots;
    uint32_t  capacity;
    uint32_t  count;     /* live entries */
    uint32_t  head;      /* index of next write */
};

JceNetSnapshotRing *jce_net_snapshot_ring_create(uint32_t capacity)
{
    if (capacity == 0) capacity = 16;
    JceNetSnapshotRing *r = (JceNetSnapshotRing *)JCE_CALLOC(1, sizeof(*r));
    if (!r) return NULL;
    r->capacity = capacity;
    r->slots = (Slot *)JCE_CALLOC(capacity, sizeof(Slot));
    if (!r->slots) { JCE_FREE(r); return NULL; }
    return r;
}

static void slot_free(Slot *s)
{
    JCE_FREE(s->entities);
    s->entities = NULL;
    s->count = 0;
    s->server_time = 0.0;
}

void jce_net_snapshot_ring_destroy(JceNetSnapshotRing *r)
{
    if (!r) return;
    for (uint32_t i = 0; i < r->capacity; ++i) slot_free(&r->slots[i]);
    JCE_FREE(r->slots);
    JCE_FREE(r);
}

uint32_t jce_net_snapshot_ring_count(const JceNetSnapshotRing *r)
{
    return r ? r->count : 0;
}

/* Iterate ring slots in chronological order, oldest first. */
static uint32_t logical_to_physical(const JceNetSnapshotRing *r, uint32_t logical)
{
    /* When count == capacity, head points at the slot that will be
     * overwritten next (= the oldest).  When count < capacity, the
     * oldest is at index 0. */
    uint32_t base = (r->count == r->capacity) ? r->head : 0;
    return (base + logical) % r->capacity;
}

double jce_net_snapshot_ring_oldest_time(const JceNetSnapshotRing *r)
{
    if (!r || r->count == 0) return 0.0;
    return r->slots[logical_to_physical(r, 0)].server_time;
}

double jce_net_snapshot_ring_newest_time(const JceNetSnapshotRing *r)
{
    if (!r || r->count == 0) return 0.0;
    return r->slots[logical_to_physical(r, r->count - 1)].server_time;
}

bool jce_net_snapshot_ring_push(JceNetSnapshotRing *r, double t,
                                 const JceNetEntitySnap *entities,
                                 uint32_t count)
{
    if (!r) return false;
    /* Drop snapshots older than the newest we've already accepted. */
    if (r->count > 0 && t <= jce_net_snapshot_ring_newest_time(r)) return false;

    Slot *target = &r->slots[r->head];
    slot_free(target);
    if (count > 0 && entities) {
        target->entities = (JceNetEntitySnap *)JCE_MALLOC(count * sizeof(JceNetEntitySnap));
        if (!target->entities) return false;
        memcpy(target->entities, entities, count * sizeof(JceNetEntitySnap));
    }
    target->count       = count;
    target->server_time = t;

    r->head = (r->head + 1) % r->capacity;
    if (r->count < r->capacity) r->count++;
    return true;
}

/* ── Sampling ────────────────────────────────────────────────────── */

static const JceNetEntitySnap *find_entity(const Slot *s, uint32_t id)
{
    for (uint32_t i = 0; i < s->count; ++i)
        if (s->entities[i].entity_id == id) return &s->entities[i];
    return NULL;
}

/* Vector / quaternion lerp helpers — kept local to avoid pulling in
 * the math library's slerp (which we'd rather not couple to). */
static jce_vec3 v3_lerp(jce_vec3 a, jce_vec3 b, float t)
{
    jce_vec3 r = { a.x + (b.x - a.x) * t,
                   a.y + (b.y - a.y) * t,
                   a.z + (b.z - a.z) * t };
    return r;
}

static jce_quat q_nlerp(jce_quat a, jce_quat b, float t)
{
    /* Take shortest path. */
    float dot = a.x*b.x + a.y*b.y + a.z*b.z + a.w*b.w;
    if (dot < 0.0f) { b.x=-b.x; b.y=-b.y; b.z=-b.z; b.w=-b.w; }
    jce_quat r;
    r.x = a.x + (b.x - a.x) * t;
    r.y = a.y + (b.y - a.y) * t;
    r.z = a.z + (b.z - a.z) * t;
    r.w = a.w + (b.w - a.w) * t;
    float lsq = r.x*r.x + r.y*r.y + r.z*r.z + r.w*r.w;
    if (lsq > 0.0f) {
        float inv = 1.0f / sqrtf(lsq);
        r.x *= inv; r.y *= inv; r.z *= inv; r.w *= inv;
    }
    return r;
}

static void blend_entity(const JceNetEntitySnap *a, const JceNetEntitySnap *b,
                         float t, JceNetEntitySnap *out)
{
    /* Use `b`'s mask as the post-blend mask; fields present in both
     * are interpolated.  Fields only in `b` are taken straight from
     * `b` (just-appeared).  Fields only in `a` are dropped (just-
     * removed). */
    *out = *b;
    if (!a) return;
    if (a->mask & JCE_NET_FIELD_TRANSFORM) {
        out->position = v3_lerp(a->position, b->position, t);
        out->rotation = q_nlerp(a->rotation, b->rotation, t);
        out->scale    = v3_lerp(a->scale,    b->scale,    t);
    }
    if (a->mask & JCE_NET_FIELD_VELOCITY) {
        out->linear_velocity  = v3_lerp(a->linear_velocity,
                                        b->linear_velocity,  t);
        out->angular_velocity = v3_lerp(a->angular_velocity,
                                        b->angular_velocity, t);
    }
    if (a->mask & JCE_NET_FIELD_HEALTH)
        out->health = a->health + (b->health - a->health) * t;
    if (a->mask & JCE_NET_FIELD_INPUT) {
        for (int i = 0; i < 6; ++i)
            out->input[i] = a->input[i] + (b->input[i] - a->input[i]) * t;
    }
    /* Anim state name: discrete — pick `b`'s. */
}

bool jce_net_interp_sample(const JceNetSnapshotRing *r, double rt,
                            uint32_t entity_id, bool extrapolate,
                            JceNetEntitySnap *out)
{
    if (!r || !out || r->count == 0) return false;
    if (r->count == 1) {
        const Slot *s = &r->slots[logical_to_physical(r, 0)];
        const JceNetEntitySnap *e = find_entity(s, entity_id);
        if (!e) return false;
        *out = *e;
        return true;
    }

    /* Find bracketing pair.  Linear walk over `count` slots is fine
     * for typical 8-32 entries; could swap for binary search later. */
    int a_idx = -1, b_idx = -1;
    for (uint32_t i = 1; i < r->count; ++i) {
        double t_prev = r->slots[logical_to_physical(r, i - 1)].server_time;
        double t_cur  = r->slots[logical_to_physical(r, i)].server_time;
        if (rt >= t_prev && rt <= t_cur) {
            a_idx = (int)(i - 1);
            b_idx = (int)i;
            break;
        }
    }

    if (a_idx < 0) {
        /* Out of range. */
        if (rt < jce_net_snapshot_ring_oldest_time(r)) {
            /* Hold the oldest snapshot regardless of extrapolate
             * setting — rendering historic state is rarely useful. */
            const Slot *s = &r->slots[logical_to_physical(r, 0)];
            const JceNetEntitySnap *e = find_entity(s, entity_id);
            if (!e) return false;
            *out = *e;
            return true;
        }
        /* rt past newest. */
        const Slot *s_new  = &r->slots[logical_to_physical(r, r->count - 1)];
        const Slot *s_prev = &r->slots[logical_to_physical(r, r->count - 2)];
        const JceNetEntitySnap *en = find_entity(s_new,  entity_id);
        const JceNetEntitySnap *ep = find_entity(s_prev, entity_id);
        if (!en) return false;
        if (!extrapolate || !ep) { *out = *en; return true; }
        /* t > 1 extrapolation. */
        double dt = s_new->server_time - s_prev->server_time;
        if (dt <= 0.0) { *out = *en; return true; }
        float t = (float)((rt - s_prev->server_time) / dt);
        blend_entity(ep, en, t, out);
        return true;
    }

    const Slot *s_a = &r->slots[logical_to_physical(r, (uint32_t)a_idx)];
    const Slot *s_b = &r->slots[logical_to_physical(r, (uint32_t)b_idx)];
    const JceNetEntitySnap *ea = find_entity(s_a, entity_id);
    const JceNetEntitySnap *eb = find_entity(s_b, entity_id);
    if (!eb) {
        /* Removed by `b`; fall back to `a` if present. */
        if (!ea) return false;
        *out = *ea;
        return true;
    }
    double dt = s_b->server_time - s_a->server_time;
    float t = dt > 0.0 ? (float)((rt - s_a->server_time) / dt) : 0.0f;
    if (t < 0.0f) t = 0.0f;
    if (t > 1.0f) t = 1.0f;
    blend_entity(ea, eb, t, out);
    return true;
}
