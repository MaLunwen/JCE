/*
 * jce_perception.c  Agent perception + blackboard (P2-perception-bt-binding).
 *
 * See jce_perception.h for the contract.  The blackboard is a tiny
 * open-addressing-free linear array of named slots (agent working sets are
 * small — a handful of keys — so a flat scan beats a hash table's overhead
 * and keeps the code allocation-light).
 */

#include <jce/middleware/ai/jce_perception.h>
#include <jce/os/core/jce_log.h>

#include "os/core/jce_memory.h"

#include <math.h>
#include <string.h>

#define LOG_TAG "perception"

#define JCE_BB_KEY_MAX 48

/* ── Blackboard ─────────────────────────────────────────────────── */

typedef struct {
    char              key[JCE_BB_KEY_MAX];
    JceBlackboardKind kind;
    union {
        bool     b;
        int      i;
        float    f;
        jce_vec3 v;
        uint64_t e;
    } as;
} JceBbSlot;

struct JceBlackboard {
    JceBbSlot *slots;
    uint32_t   count;
    uint32_t   cap;
};

JceBlackboard *jce_blackboard_create(void)
{
    JceBlackboard *bb = (JceBlackboard *)JCE_CALLOC(1, sizeof(*bb));
    return bb;
}

void jce_blackboard_destroy(JceBlackboard *bb)
{
    if (!bb) return;
    JCE_FREE(bb->slots);
    JCE_FREE(bb);
}

void jce_blackboard_clear(JceBlackboard *bb)
{
    if (bb) bb->count = 0;
}

/* Locate a slot by key, or NULL. */
static JceBbSlot *bb_find(const JceBlackboard *bb, const char *key)
{
    if (!bb || !key) return NULL;
    for (uint32_t i = 0; i < bb->count; ++i)
        if (strncmp(bb->slots[i].key, key, JCE_BB_KEY_MAX) == 0)
            return &bb->slots[i];
    return NULL;
}

/* Find-or-append a writable slot for `key`; returns NULL on OOM. */
static JceBbSlot *bb_slot_for_write(JceBlackboard *bb, const char *key)
{
    if (!bb || !key || !key[0]) return NULL;
    JceBbSlot *s = bb_find(bb, key);
    if (s) return s;

    if (bb->count >= bb->cap) {
        uint32_t new_cap = bb->cap ? bb->cap * 2u : 8u;
        JceBbSlot *p = (JceBbSlot *)JCE_REALLOC(bb->slots,
                                                (size_t)new_cap * sizeof(*p));
        if (!p) return NULL;
        bb->slots = p;
        bb->cap   = new_cap;
    }
    s = &bb->slots[bb->count++];
    memset(s, 0, sizeof(*s));
    size_t kl = strlen(key);
    if (kl >= JCE_BB_KEY_MAX) kl = JCE_BB_KEY_MAX - 1;
    memcpy(s->key, key, kl);
    s->key[kl] = '\0';
    return s;
}

void jce_blackboard_set_bool(JceBlackboard *bb, const char *key, bool v)
{
    JceBbSlot *s = bb_slot_for_write(bb, key);
    if (s) { s->kind = JCE_BB_BOOL; s->as.b = v; }
}

void jce_blackboard_set_int(JceBlackboard *bb, const char *key, int v)
{
    JceBbSlot *s = bb_slot_for_write(bb, key);
    if (s) { s->kind = JCE_BB_INT; s->as.i = v; }
}

void jce_blackboard_set_float(JceBlackboard *bb, const char *key, float v)
{
    JceBbSlot *s = bb_slot_for_write(bb, key);
    if (s) { s->kind = JCE_BB_FLOAT; s->as.f = v; }
}

void jce_blackboard_set_vec3(JceBlackboard *bb, const char *key, jce_vec3 v)
{
    JceBbSlot *s = bb_slot_for_write(bb, key);
    if (s) { s->kind = JCE_BB_VEC3; s->as.v = v; }
}

void jce_blackboard_set_entity(JceBlackboard *bb, const char *key, uint64_t v)
{
    JceBbSlot *s = bb_slot_for_write(bb, key);
    if (s) { s->kind = JCE_BB_ENTITY; s->as.e = v; }
}

bool jce_blackboard_get_bool(const JceBlackboard *bb, const char *key, bool def)
{
    const JceBbSlot *s = bb_find(bb, key);
    return (s && s->kind == JCE_BB_BOOL) ? s->as.b : def;
}

int jce_blackboard_get_int(const JceBlackboard *bb, const char *key, int def)
{
    const JceBbSlot *s = bb_find(bb, key);
    return (s && s->kind == JCE_BB_INT) ? s->as.i : def;
}

float jce_blackboard_get_float(const JceBlackboard *bb, const char *key, float def)
{
    const JceBbSlot *s = bb_find(bb, key);
    return (s && s->kind == JCE_BB_FLOAT) ? s->as.f : def;
}

jce_vec3 jce_blackboard_get_vec3(const JceBlackboard *bb, const char *key,
                                 jce_vec3 def)
{
    const JceBbSlot *s = bb_find(bb, key);
    return (s && s->kind == JCE_BB_VEC3) ? s->as.v : def;
}

uint64_t jce_blackboard_get_entity(const JceBlackboard *bb, const char *key,
                                   uint64_t def)
{
    const JceBbSlot *s = bb_find(bb, key);
    return (s && s->kind == JCE_BB_ENTITY) ? s->as.e : def;
}

bool jce_blackboard_has(const JceBlackboard *bb, const char *key)
{
    return bb_find(bb, key) != NULL;
}

JceBlackboardKind jce_blackboard_kind(const JceBlackboard *bb, const char *key)
{
    const JceBbSlot *s = bb_find(bb, key);
    return s ? s->kind : JCE_BB_NONE;
}

uint32_t jce_blackboard_count(const JceBlackboard *bb)
{
    return bb ? bb->count : 0u;
}

/* ── Perception ─────────────────────────────────────────────────── */

bool jce_perception_update(JceBlackboard *bb,
                           const JcePerceptionAgent *agent,
                           const JcePerceptionTarget *targets,
                           uint32_t target_count,
                           jce_perception_los_fn los_fn,
                           void *los_userdata,
                           JcePerceptionResult *out)
{
    JcePerceptionResult r;
    memset(&r, 0, sizeof r);
    if (out) memset(out, 0, sizeof *out);

    if (!agent) return false;

    jce_vec3 fwd = jce_v3_normalize(agent->forward);
    float cos_half = cosf(agent->sight_half_angle);

    float best_seen_d   = 0.0f;   /* nearest visible distance */
    float best_heard_l  = 0.0f;   /* loudest audible "score" (loudness-range) */

    for (uint32_t i = 0; i < target_count; ++i) {
        const JcePerceptionTarget *t = &targets[i];
        jce_vec3 to = jce_v3_sub(t->position, agent->eye_position);
        float dist = jce_v3_len(to);

        /* ── Sight: range, then cone, then line-of-sight. ── */
        if (agent->sight_range > 0.0f && dist <= agent->sight_range) {
            bool in_cone = true;
            if (dist > 1e-4f) {
                jce_vec3 dir = jce_v3_scale(to, 1.0f / dist);
                /* When forward is degenerate (zero), treat the cone as
                 * omnidirectional so a mis-authored facing still senses. */
                float fl = jce_v3_len(fwd);
                if (fl > 1e-4f) {
                    float c = jce_v3_dot(fwd, dir);
                    in_cone = (c >= cos_half);
                }
            }
            if (in_cone) {
                bool blocked = los_fn
                    ? los_fn(agent->eye_position, t->position, los_userdata)
                    : false;
                if (!blocked && (!r.can_see || dist < best_seen_d)) {
                    r.can_see       = true;
                    r.seen_entity   = t->entity;
                    r.seen_position = t->position;
                    r.seen_distance = dist;
                    best_seen_d     = dist;
                }
            }
        }

        /* ── Hearing: candidate loud enough AND within agent hearing range.
         * A sound carries `loudness` metres; the agent hears it if the
         * distance is under BOTH that radius and the agent's hearing range.
         * Walls don't block hearing here (kept simple — noted). ── */
        if (agent->hearing_range > 0.0f && t->loudness > 0.0f &&
            dist <= agent->hearing_range && dist <= t->loudness) {
            float score = t->loudness - dist;   /* louder + closer wins */
            if (!r.can_hear || score > best_heard_l) {
                r.can_hear        = true;
                r.heard_entity    = t->entity;
                r.heard_position  = t->position;
                best_heard_l      = score;
            }
        }
    }

    /* ── Publish stimuli to the blackboard under the conventional keys. ── */
    if (bb) {
        jce_blackboard_set_bool  (bb, "target.visible",  r.can_see);
        jce_blackboard_set_entity(bb, "target.entity",   r.seen_entity);
        jce_blackboard_set_vec3  (bb, "target.position", r.seen_position);
        jce_blackboard_set_float (bb, "target.distance", r.seen_distance);
        if (r.can_see)
            jce_blackboard_set_vec3(bb, "target.last_known_position",
                                    r.seen_position);

        jce_blackboard_set_bool  (bb, "sound.heard",    r.can_hear);
        jce_blackboard_set_entity(bb, "sound.entity",   r.heard_entity);
        jce_blackboard_set_vec3  (bb, "sound.position", r.heard_position);
    }

    if (out) *out = r;
    return r.can_see || r.can_hear;
}
